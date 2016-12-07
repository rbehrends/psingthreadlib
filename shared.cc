#include <iostream>
#include "kernel/mod2.h"
#include "Singular/ipid.h"
#include "Singular/ipshell.h"
#include "Singular/links/silink.h"
#include "Singular/lists.h"
#include "Singular/blackbox.h"
#include "Singular/feOpt.h"
#include "Singular/libsingular.h"
#include <cstring>
#include <string>
#include <errno.h>
#include <stdio.h>
#include <vector>
#include <map>
#include <iterator>
#include <queue>
#include "thread.h"
#include "lintree.h"

using namespace std;

namespace { // putting the implementation inside an anonymous namespace

class SharedObject {
private:
  Lock lock;
  long refcount;
  int type;
  std::string name;
public:
  SharedObject(): lock(), refcount(0) { }
  virtual ~SharedObject() { }
  void set_type(int type_init) { type = type_init; }
  int get_type() { return type; }
  void set_name(std::string &name_init) { name = name_init; }
  std::string &get_name() { return name; }
  void incref() {
    lock.lock();
    refcount++;
    lock.unlock();
  }
  long decref() {
    int result;
    lock.lock();
    result = --refcount;
    lock.unlock();
    return result;
  }
  long getref() {
    return refcount;
  }
  virtual BOOLEAN op2(int op, leftv res, leftv a1, leftv a2) {
    return TRUE;
  }
  virtual BOOLEAN op3(int op, leftv res, leftv a1, leftv a2, leftv a3) {
    return TRUE;
  }
};

void acquireShared(SharedObject *obj) {
  obj->incref();
}

void releaseShared(SharedObject *obj) {
  if (obj->decref() == 0) {
    delete obj;
  }
}

typedef std::map<std::string, SharedObject *> SharedObjectTable;

class Region : public SharedObject {
private:
  Lock region_lock;
public:
  SharedObjectTable objects;
  Region() : SharedObject(), region_lock(), objects() { }
  virtual ~Region() { }
  Lock *get_lock() { return &region_lock; }
  void lock() {
    if (!region_lock.is_locked())
      region_lock.lock();
  }
  void unlock() {
    if (region_lock.is_locked())
      region_lock.unlock();
  }
  int is_locked() {
    return region_lock.is_locked();
  }
};

Lock global_objects_lock;
SharedObjectTable global_objects;

int type_region;
int type_regionlock;
int type_channel;
int type_syncvar;
int type_atomic_table;
int type_shared_table;
int type_atomic_list;
int type_shared_list;
int type_thread;

typedef SharedObject *SharedObjectPtr;
typedef SharedObjectPtr (*SharedConstructor)();

SharedObject *makeSharedObject(SharedObjectTable &table,
  Lock *lock, int type, string &name, SharedConstructor scons)
{
  int was_locked = lock->is_locked();
  SharedObject *result = NULL;
  if (!was_locked)
    lock->lock();
  if (table.count(name)) {
    result = table[name];
    if (result->get_type() != type)
      result = NULL;
  } else {
    result = scons();
    result->set_type(type);
    result->set_name(name);
    table.insert(pair<string,SharedObject *>(name, result));
  }
  if (!was_locked)
    lock->unlock();
  return result;
}

SharedObject *findSharedObject(SharedObjectTable &table,
  Lock *lock, string &name)
{
  int was_locked = lock->is_locked();
  SharedObject *result = NULL;
  if (!was_locked)
    lock->lock();
  if (table.count(name)) {
    result = table[name];
  }
  if (!was_locked)
    lock->unlock();
  return result;
}

class Transactional: public SharedObject {
private:
  Region *region;
  Lock *lock;
protected:
  int tx_begin() {
    if (!region)
      lock->lock();
    else {
      if (!lock->is_locked()) {
	return 0;
      }
    }
    return 1;
  }
  void tx_end() {
    if (!region)
      lock->unlock();
  }
public:
  Transactional() :
      SharedObject(), region(NULL), lock(NULL) {
  }
  void set_region(Region *region_init) {
    region = region_init;
    if (region_init) {
      lock = region_init->get_lock();
    } else {
      lock = new Lock();
    }
  }
  virtual ~Transactional() { if (!region && lock) delete lock; }
};

class TxTable: public Transactional {
private:
  std::map<string, string> entries;
public:
  TxTable() : Transactional(), entries() { }
  virtual ~TxTable() { }
  int put(string &key, string &value) {
    int result = 0;
    if (!tx_begin()) return -1;
    if (entries.count(key)) {
      entries[key] = value;
    } else {
      entries.insert(pair<string, string>(key, value));
      result = 1;
    }
    tx_end();
    return result;
  }
  int get(string &key, string &value) {
    int result = 0;
    if (!tx_begin()) return -1;
    if (entries.count(key)) {
      value = entries[key];
      result = 1;
    }
    tx_end();
    return result;
  }
  int check(string &key) {
    int result;
    if (!tx_begin()) return -1;
    result = entries.count(key);
    tx_end();
    return result;
  }
};

class TxList: public Transactional {
private:
  vector<string> entries;
public:
  TxList() : Transactional(), entries() { }
  virtual ~TxList() { }
  int put(size_t index, string &value) {
    int result = -1;
    if (!tx_begin()) return -1;
    if (index >= 1 && index <= entries.size()) {
      entries[index-1] = value;
      result = 1;
    } else {
      entries.resize(index+1);
      entries[index-1] = value;
      result = 0;
    }
    tx_end();
    return result;
  }
  int get(size_t index, string &value) {
    int result = 0;
    if (!tx_begin()) return -1;
    if (index >= 1 && index <= entries.size()) {
      result = (entries[index-1].size() != 0);
      if (result)
        value = entries[index-1];
    }
    tx_end();
    return result;
  }
  long size() {
    long result;
    if (!tx_begin()) return -1;
    result = (long) entries.size();
    tx_end();
    return result;
  }
};

class Channel : public SharedObject {
private:
  queue<string> q;
  Lock lock;
  ConditionVariable cond;
public:
  Channel(): SharedObject(), lock(), cond(&lock) { }
  virtual ~Channel() { }
  void send(string item) {
    lock.lock();
    q.push(item);
    cond.signal();
    lock.unlock();
  }
  string receive() {
    lock.lock();
    while (q.empty()) {
      cond.wait();
    }
    string result = q.front();
    q.pop();
    if (!q.empty())
      cond.signal();
    lock.unlock();
    return result;
  }
  long count() {
    lock.lock();
    long result = q.size();
    lock.unlock();
    return result;
  }
};

class SyncVar : public SharedObject {
private:
  string value;
  int init;
  Lock lock;
  ConditionVariable cond;
public:
  SyncVar(): SharedObject(), init(0), lock(), cond(&lock) { }
  virtual ~SyncVar() { }
  int write(string item) {
    int result = 0;
    lock.lock();
    if (!init) {
      value = item;
      init = 1;
      cond.broadcast();
      result = 1;
    }
    lock.unlock();
    return result;
  }
  string read() {
    lock.lock();
    while (!init)
      cond.wait();
    string result = value;
    lock.unlock();
    return result;
  }
  int check() {
    lock.lock();
    int result = init;
    lock.unlock();
    return result;
  }
};

void *shared_init(blackbox *b) {
  return omAlloc0(sizeof(SharedObject *));
}

void *new_shared(SharedObject *obj) {
  acquireShared(obj);
  void *result = omAlloc0(sizeof(SharedObject *));
  *(SharedObject **)result = obj;
  return result;
}

void shared_destroy(blackbox *b, void *d) {
  SharedObject *obj = *(SharedObject **)d;
  if (obj) {
    releaseShared(*(SharedObject **)d);
    *(SharedObject **)d = NULL;
  }
}

void rlock_destroy(blackbox *b, void *d) {
  SharedObject *obj = *(SharedObject **)d;
  ((Region *) obj)->unlock();
  if (obj) {
    releaseShared(*(SharedObject **)d);
    *(SharedObject **)d = NULL;
  }
}

void *shared_copy(blackbox *b, void *d) {
  SharedObject *obj = *(SharedObject **)d;
  void *result = shared_init(b);
  *(SharedObject **)result = obj;
  if (obj)
    acquireShared(obj);
  return result;
}

BOOLEAN shared_assign(leftv l, leftv r) {
  if (r->Typ() == l->Typ()) {
    if (l->rtyp == IDHDL) {
      omFree(IDDATA((idhdl)l->data));
      IDDATA((idhdl)l->data) = (char*)shared_copy(NULL,r->Data());
    } else {
      leftv ll=l->LData();
      if (ll==NULL)
      {
	return TRUE; // out of array bounds or similiar
      }
      shared_destroy(NULL, ll->data);
      omFree(ll->data);
      ll->data = shared_copy(NULL,r->Data());
    }
  } else {
    Werror("assign %s(%d) = %s(%d)",
        Tok2Cmdname(l->Typ()),l->Typ(),Tok2Cmdname(r->Typ()),r->Typ());
    return TRUE;
  }
  return FALSE;
}

BOOLEAN rlock_assign(leftv l, leftv r) {
  if (r->Typ() == l->Typ()) {
    if (l->rtyp == IDHDL) {
      omFree(IDDATA((idhdl)l->data));
      IDDATA((idhdl)l->data) = (char*)shared_copy(NULL,r->Data());
    } else {
      leftv ll=l->LData();
      if (ll==NULL)
      {
	return TRUE; // out of array bounds or similiar
      }
      rlock_destroy(NULL, ll->data);
      omFree(ll->data);
      ll->data = shared_copy(NULL,r->Data());
    }
  } else {
    Werror("assign %s(%d) = %s(%d)",
        Tok2Cmdname(l->Typ()),l->Typ(),Tok2Cmdname(r->Typ()),r->Typ());
    return TRUE;
  }
  return FALSE;
}


BOOLEAN shared_check_assign(blackbox *b, leftv l, leftv r) {
  int lt = l->Typ();
  int rt = r->Typ();
  if (lt != DEF_CMD && lt != rt) {
    const char *rn=Tok2Cmdname(rt);
    const char *ln=Tok2Cmdname(lt);
    Werror("cannot assign %s (%d) to %s (%d)\n", rn, rt, ln, lt);
    return TRUE;
  }
  return FALSE;
}

BOOLEAN shared_op2(int op, leftv res, leftv a1, leftv a2) {
  SharedObject *obj = *(SharedObject **)a1->Data();
  return obj->op2(op, res, a1, a2);
}

BOOLEAN shared_op3(int op, leftv res, leftv a1, leftv a2, leftv a3) {
  SharedObject *obj = *(SharedObject **)a1->Data();
  return obj->op3(op, res, a1, a2, a3);
}

char *shared_string(blackbox *b, void *d) {
  char buf[80];
  SharedObject *obj = *(SharedObject **)d;
  if (!obj)
    return omStrDup("<uninitialized shared object>");
  int type = obj->get_type();
  string &name = obj->get_name();
  const char *type_name = "unknown";
  if (type == type_channel)
    type_name = "channel";
  else if (type == type_atomic_table)
    type_name = "atomic_table";
  else if (type == type_shared_table)
    type_name = "shared_table";
  else if (type == type_atomic_list)
    type_name = "atomic_list";
  else if (type == type_shared_list)
    type_name = "shared_list";
  else if (type == type_syncvar)
    type_name = "syncvar";
  else if (type == type_region)
    type_name = "region";
  else if (type == type_regionlock)
    type_name = "regionlock";
  else if (type == type_thread) {
    sprintf(buf, "<thread #%s>", name.c_str());
    return omStrDup(buf);
  }
  sprintf(buf, "<%s \"%.40s\">", type_name, name.c_str());
  return omStrDup(buf);
}

char *rlock_string(blackbox *b, void *d) {
  char buf[80];
  SharedObject *obj = *(SharedObject **)d;
  if (!obj)
    return omStrDup("<uninitialized region lock>");
  sprintf(buf, "<region lock \"%.40s\">", obj->get_name().c_str());
  return omStrDup(buf);
}

void report(const char *fmt, const char *name) {
  char buf[80];
  sprintf(buf, fmt, name);
  WerrorS(buf);
}

int wrong_num_args(const char *name, leftv arg, int n) {
  for (int i=1; i<=n; i++) {
    if (!arg) {
      report("%s: too few arguments", name);
      return TRUE;
    }
    arg = arg->next;
  }
  if (arg) {
    report("%s: too many arguments", name);
    return TRUE;
  }
  return FALSE;
}

int not_a_uri(const char *name, leftv arg) {
  if (arg->Typ() != STRING_CMD) {
    report("%s: not a valid URI", name);
    return TRUE;
  }
  return FALSE;
}

int not_a_region(const char *name, leftv arg) {
  if (arg->Typ() != type_region) {
    report("%s: not a region", name);
    return TRUE;
  }
  return FALSE;
}


char *str(leftv arg) {
  return (char *)(arg->Data());
}

SharedObject *consTable() {
  return new TxTable();
}

SharedObject *consList() {
  return new TxList();
}

SharedObject *consChannel() {
  return new Channel();
}

SharedObject *consSyncVar() {
  return new SyncVar();
}

SharedObject *consRegion() {
  return new Region();
}

BOOLEAN makeAtomicTable(leftv result, leftv arg) {
  if (wrong_num_args("makeAtomicTable", arg, 1))
    return TRUE;
  if (not_a_uri("makeAtomicTable", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = makeSharedObject(global_objects,
    &global_objects_lock, type_atomic_table, uri, consTable);
  ((TxTable *) obj)->set_region(NULL);
  result->rtyp = type_atomic_table;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeAtomicList(leftv result, leftv arg) {
  if (wrong_num_args("makeAtomicList", arg, 1))
    return TRUE;
  if (not_a_uri("makeAtomicList", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = makeSharedObject(global_objects,
    &global_objects_lock, type_atomic_list, uri, consList);
  ((TxList *) obj)->set_region(NULL);
  result->rtyp = type_atomic_list;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeSharedTable(leftv result, leftv arg) {
  if (wrong_num_args("makeSharedTable", arg, 2))
    return TRUE;
  if (not_a_region("makeSharedTable", arg))
    return TRUE;
  if (not_a_uri("makeSharedTable", arg->next))
    return TRUE;
  Region *region = *(Region **) arg->Data();
  fflush(stdout);
  string s = str(arg->next);
  SharedObject *obj = makeSharedObject(region->objects,
    region->get_lock(), type_shared_table, s, consTable);
  ((TxTable *) obj)->set_region(region);
  result->rtyp = type_shared_table;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeSharedList(leftv result, leftv arg) {
  if (wrong_num_args("makeSharedList", arg, 2))
    return TRUE;
  if (not_a_region("makeSharedList", arg))
    return TRUE;
  if (not_a_uri("makeSharedList", arg->next))
    return TRUE;
  Region *region = *(Region **) arg->Data();
  string s = str(arg->next);
  SharedObject *obj = makeSharedObject(region->objects,
    region->get_lock(), type_shared_list, s, consList);
  ((TxList *) obj)->set_region(region);
  result->rtyp = type_shared_list;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeChannel(leftv result, leftv arg) {
  if (wrong_num_args("makeChannel", arg, 1))
    return TRUE;
  if (not_a_uri("makeChannel", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = makeSharedObject(global_objects,
    &global_objects_lock, type_channel, uri, consChannel);
  result->rtyp = type_channel;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeSyncVar(leftv result, leftv arg) {
  if (wrong_num_args("makeSyncVar", arg, 1))
    return TRUE;
  if (not_a_uri("makeSyncVar", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = makeSharedObject(global_objects,
    &global_objects_lock, type_syncvar, uri, consSyncVar);
  result->rtyp = type_syncvar;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN makeRegion(leftv result, leftv arg) {
  if (wrong_num_args("makeRegion", arg, 1))
    return TRUE;
  if (not_a_uri("makeRegion", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = makeSharedObject(global_objects,
    &global_objects_lock, type_region, uri, consRegion);
  result->rtyp = type_region;
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN findSharedObject(leftv result, leftv arg) {
  if (wrong_num_args("findSharedObject", arg, 1))
    return TRUE;
  if (not_a_uri("findSharedObject", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = findSharedObject(global_objects,
    &global_objects_lock, uri);
  result->rtyp = INT_CMD;
  result->data = (char *)(long)(obj != NULL);
  return FALSE;
}

BOOLEAN typeSharedObject(leftv result, leftv arg) {
  if (wrong_num_args("findSharedObject", arg, 1))
    return TRUE;
  if (not_a_uri("findSharedObject", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = findSharedObject(global_objects,
    &global_objects_lock, uri);
  int type = obj ? obj->get_type() : -1;
  const char *type_name = "undefined";
  if (type == type_channel)
    type_name = "channel";
  else if (type == type_atomic_table)
    type_name = "atomic_table";
  else if (type == type_shared_table)
    type_name = "shared_table";
  else if (type == type_atomic_list)
    type_name = "atomic_list";
  else if (type == type_shared_list)
    type_name = "shared_list";
  else if (type == type_syncvar)
    type_name = "syncvar";
  else if (type == type_region)
    type_name = "region";
  else if (type == type_regionlock)
    type_name = "regionlock";
  result->rtyp = STRING_CMD;
  result->data = (char *)(omStrDup(type_name));
  return FALSE;
}

BOOLEAN bindSharedObject(leftv result, leftv arg) {
  if (wrong_num_args("bindSharedObject", arg, 1))
    return TRUE;
  if (not_a_uri("bindSharedObject", arg))
    return TRUE;
  string uri = str(arg);
  SharedObject *obj = findSharedObject(global_objects,
    &global_objects_lock, uri);
  if (!obj) {
    WerrorS("bindSharedObject: cannot find object");
    return TRUE;
  }
  result->rtyp = obj->get_type();
  result->data = new_shared(obj);
  return FALSE;
}

BOOLEAN getTable(leftv result, leftv arg) {
  if (wrong_num_args("getTable", arg, 2))
    return TRUE;
  if (arg->Typ() != type_atomic_table && arg->Typ() != type_shared_table) {
    WerrorS("getTable: not a valid table");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("getTable: not a valid table key");
    return TRUE;
  }
  TxTable *table = *(TxTable **) arg->Data();
  string key = (char *)(arg->next->Data());
  string value;
  int success = table->get(key, value);
  if (success < 0) {
    WerrorS("getTable: region not acquired");
    return TRUE;
  }
  if (success == 0) {
    WerrorS("getTable: key not found");
    return TRUE;
  }
  leftv tmp = LinTree::from_string(value);
  result->rtyp = tmp->Typ();
  result->data = tmp->Data();
  return FALSE;
}

BOOLEAN inTable(leftv result, leftv arg) {
  if (wrong_num_args("inTable", arg, 2))
    return TRUE;
  if (arg->Typ() != type_atomic_table && arg->Typ() != type_shared_table) {
    WerrorS("inTable: not a valid table");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("inTable: not a valid table key");
    return TRUE;
  }
  TxTable *table = *(TxTable **) arg->Data();
  string key = (char *)(arg->next->Data());
  int success = table->check(key);
  if (success < 0) {
    WerrorS("inTable: region not acquired");
    return TRUE;
  }
  result->rtyp = INT_CMD;
  result->data = (char *)(long)(success);
  return FALSE;
}

BOOLEAN putTable(leftv result, leftv arg) {
  if (wrong_num_args("putTable", arg, 3))
    return TRUE;
  if (arg->Typ() != type_atomic_table && arg->Typ() != type_shared_table) {
    WerrorS("putTable: not a valid table");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("putTable: not a valid table key");
    return TRUE;
  }
  TxTable *table = *(TxTable **) arg->Data();
  string key = (char *)(arg->next->Data());
  string value = LinTree::to_string(arg->next->next);
  int success = table->put(key, value);
  if (success < 0) {
    WerrorS("putTable: region not acquired");
    return TRUE;
  }
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN getList(leftv result, leftv arg) {
  if (wrong_num_args("getList", arg, 2))
    return TRUE;
  if (arg->Typ() != type_atomic_list && arg->Typ() != type_shared_list) {
    WerrorS("getList: not a valid list (atomic or shared)");
    return TRUE;
  }
  if (arg->next->Typ() != INT_CMD) {
    WerrorS("getList: index must be an integer");
    return TRUE;
  }
  TxList *list = *(TxList **) arg->Data();
  long index = (long)(arg->next->Data());
  string value;
  int success = list->get(index, value);
  if (success < 0) {
    WerrorS("getList: region not acquired");
    return TRUE;
  }
  if (success == 0) {
    WerrorS("getList: no value at position");
    return TRUE;
  }
  leftv tmp = LinTree::from_string(value);
  result->rtyp = tmp->Typ();
  result->data = tmp->Data();
  return FALSE;
}

BOOLEAN putList(leftv result, leftv arg) {
  if (wrong_num_args("putList", arg, 3))
    return TRUE;
  if (arg->Typ() != type_atomic_list && arg->Typ() != type_shared_list) {
    WerrorS("putList: not a valid list (shared or atomic)");
    return TRUE;
  }
  if (arg->next->Typ() != INT_CMD) {
    WerrorS("putList: index must be an integer");
    return TRUE;
  }
  TxList *list = *(TxList **) arg->Data();
  long index = (long)(arg->next->Data());
  string value = LinTree::to_string(arg->next->next);
  int success = list->put(index, value);
  if (success < 0) {
    WerrorS("putList: region not acquired");
    return TRUE;
  }
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN lockRegion(leftv result, leftv arg) {
  if (wrong_num_args("lockRegion", arg, 1))
    return TRUE;
  if (not_a_region("lockRegion", arg))
    return TRUE;
  Region *region = *(Region **)arg->Data();
  if (region->is_locked()) {
    WerrorS("lockRegion: region is already locked");
    return TRUE;
  }
  region->lock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN regionLock(leftv result, leftv arg) {
  if (wrong_num_args("lockRegion", arg, 1))
    return TRUE;
  if (not_a_region("lockRegion", arg))
    return TRUE;
  Region *region = *(Region **)arg->Data();
  if (region->is_locked()) {
    WerrorS("lockRegion: region is already locked");
    return TRUE;
  }
  region->lock();
  result->rtyp = type_regionlock;
  result->data = new_shared(region);
  return FALSE;
}


BOOLEAN unlockRegion(leftv result, leftv arg) {
  if (wrong_num_args("unlockRegion", arg, 1))
    return TRUE;
  if (not_a_region("unlockRegion", arg))
    return TRUE;
  Region *region = *(Region **)arg->Data();
  if (!region->is_locked()) {
    WerrorS("unlockRegion: region is not locked");
    return TRUE;
  }
  region->unlock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN sendChannel(leftv result, leftv arg) {
  if (wrong_num_args("sendChannel", arg, 2))
    return TRUE;
  if (arg->Typ() != type_channel) {
    WerrorS("sendChannel: argument is not a channel");
    return TRUE;
  }
  Channel *channel = *(Channel **)arg->Data();
  channel->send(LinTree::to_string(arg->next));
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN receiveChannel(leftv result, leftv arg) {
  if (wrong_num_args("receiveChannel", arg, 1))
    return TRUE;
  if (arg->Typ() != type_channel) {
    WerrorS("receiveChannel: argument is not a channel");
    return TRUE;
  }
  Channel *channel = *(Channel **)arg->Data();
  string item = channel->receive();
  leftv val = LinTree::from_string(item);
  result->rtyp = val->Typ();
  result->data = val->Data();
  return FALSE;
}

BOOLEAN statChannel(leftv result, leftv arg) {
  if (wrong_num_args("statChannel", arg, 1))
    return TRUE;
  if (arg->Typ() != type_channel) {
    WerrorS("statChannel: argument is not a channel");
    return TRUE;
  }
  Channel *channel = *(Channel **)arg->Data();
  long n = channel->count();
  result->rtyp = INT_CMD;
  result->data = (char *)n;
  return FALSE;
}

BOOLEAN writeSyncVar(leftv result, leftv arg) {
  if (wrong_num_args("writeSyncVar", arg, 2))
    return TRUE;
  if (arg->Typ() != type_syncvar) {
    WerrorS("writeSyncVar: argument is not a syncvar");
    return TRUE;
  }
  SyncVar *syncvar = *(SyncVar **)arg->Data();
  if (!syncvar->write(LinTree::to_string(arg->next))) {
    WerrorS("writeSyncVar: variable already has a value");
    return TRUE;
  }
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN readSyncVar(leftv result, leftv arg) {
  if (wrong_num_args("readSyncVar", arg, 1))
    return TRUE;
  if (arg->Typ() != type_syncvar) {
    WerrorS("readSyncVar: argument is not a syncvar");
    return TRUE;
  }
  SyncVar *syncvar = *(SyncVar **)arg->Data();
  string item = syncvar->read();
  leftv val = LinTree::from_string(item);
  result->rtyp = val->Typ();
  result->data = val->Data();
  return FALSE;
}

BOOLEAN statSyncVar(leftv result, leftv arg) {
  if (wrong_num_args("statSyncVar", arg, 1))
    return TRUE;
  if (arg->Typ() != type_syncvar) {
    WerrorS("statSyncVar: argument is not a syncvar");
    return TRUE;
  }
  SyncVar *syncvar = *(SyncVar **)arg->Data();
  int init = syncvar->check();
  result->rtyp = INT_CMD;
  result->data = (char *)(long) init;
  return FALSE;
}

void encode_shared(LinTree::LinTree &lintree, leftv val) {
  SharedObject *obj = *(SharedObject **)(val->Data());
  acquireShared(obj);
  lintree.put(obj);
}

leftv decode_shared(LinTree::LinTree &lintree) {
  int type = lintree.get_prev<int>();
  SharedObject *obj = lintree.get<SharedObject *>();
  leftv result = (leftv) omAlloc0Bin(sleftv_bin);
  result->rtyp = type;
  result->data = (void *)new_shared(obj);
  return result;
}

void ref_shared(LinTree::LinTree &lintree, int by) {
  SharedObject *obj = lintree.get<SharedObject *>();
  while (by > 0) {
    obj->incref();
    by--;
  }
  while (by < 0) {
    obj->decref();
    by++;
  }
}

void installShared(int type) {
  LinTree::install(type, encode_shared, decode_shared, ref_shared);
}

void makeSharedType(int &type, const char *name) {
  blackbox *b=(blackbox*)omAlloc0(sizeof(blackbox));
  b->blackbox_Init = shared_init;
  b->blackbox_destroy = shared_destroy;
  b->blackbox_Copy = shared_copy;
  b->blackbox_String = shared_string;
  b->blackbox_Assign = shared_assign;
  b->blackbox_CheckAssign = shared_check_assign;
  // b->blackbox_Op2 = shared_op2;
  // b->blackbox_Op3 = shared_op3;
  type = setBlackboxStuff(b, name);
  installShared(type);
}

void makeRegionlockType(int &type, const char *name) {
  blackbox *b=(blackbox*)omAlloc0(sizeof(blackbox));
  b->blackbox_Init = shared_init;
  b->blackbox_destroy = rlock_destroy;
  b->blackbox_Copy = shared_copy;
  b->blackbox_String = shared_string;
  b->blackbox_Assign = rlock_assign;
  b->blackbox_CheckAssign = shared_check_assign;
  type = setBlackboxStuff(b, name);
  installShared(type);
}

#define MAX_THREADS 128

class ThreadState {
public:
  bool active;
  bool running;
  int index;
  pthread_t id;
  pthread_t parent;
  Lock lock;
  ConditionVariable to_cond;
  ConditionVariable from_cond;
  queue<string> to_thread;
  queue<string> from_thread;
  ThreadState() : lock(), to_cond(&lock), from_cond(&lock),
                  to_thread(), from_thread() {
    active = false;
    running = false;
    index = -1;
  }
};

Lock thread_lock;

ThreadState thread_state[MAX_THREADS];

void setOption(int ch) {
  int index = feGetOptIndex(ch);
  feSetOptValue((feOptIndex) index, (int) 1);
}

char thread_arg0[] = "<Singular Thread>";

void thread_init() {
  siInit(thread_arg0);
  setOption('q');
  setOption('b');
}

void *thread_main(void *arg) {
  ThreadState *ts = (ThreadState *)arg;
  ts->lock.lock();
  for (;;) {
    while (ts->to_thread.empty())
      ts->to_cond.wait();
    /* TODO */
    string expr = ts->to_thread.front();
    ts->to_thread.pop();
    if (expr.size() == 0) {
      ts->lock.unlock();
      return NULL;
    }
    ts->from_thread.push(expr);
    ts->from_cond.signal();
  }
  ts->lock.unlock();
}

class SingularThread : public SharedObject {
private:
  ThreadState *ts;
public:
  SingularThread(ThreadState *ts_init) : SharedObject(), ts(ts_init) { }
  virtual ~SingularThread() { }
  ThreadState *getThreadState() { return ts; }
  void clearThreadState() {
    ts = NULL;
  }
};

BOOLEAN createThread(leftv result, leftv arg) {
  int i;
  BOOLEAN rcode = FALSE;
  if (wrong_num_args("createThread", arg, 0))
    return TRUE;
  thread_lock.lock();
  for (i=0; i<MAX_THREADS; i++) {
    if (!thread_state[i].active) {
      ThreadState *ts = thread_state + i;
      SingularThread *thread = new SingularThread(ts);
      char buf[10];
      sprintf(buf, "%d", i);
      string name(buf);
      thread->set_name(name);
      ts->index = i;
      if (pthread_create(&ts->id, NULL, thread_main, ts)<0) {
        rcode = TRUE;
	WerrorS("createThread: internal error: failed to create thread");
	goto exit;
      }
      ts->parent = pthread_self();
      ts->active = true;
      ts->running = true;
      queue<string> q0, q1; // clear queues
      ts->to_thread.swap(q0);
      ts->from_thread.swap(q0);
      result->rtyp = type_thread;
      result->data = new_shared(thread);
      goto exit;
    }
  }
  WerrorS("createThread: too many threads");
  rcode = TRUE;
  exit:
  thread_lock.unlock();
  return rcode;
}

BOOLEAN joinThread(leftv result, leftv arg) {
  if (wrong_num_args("joinThread", arg, 1))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("joinThread: argument is not a thread");
    return TRUE;
  }
  SingularThread *thread = *(SingularThread **)arg->Data();
  ThreadState *ts = thread->getThreadState();
  if (ts && ts->parent != pthread_self()) {
    WerrorS("joinThread: can only be called from parent thread");
    return TRUE;
  }
  ts->lock.lock();
  string eof("");
  ts->to_thread.push(eof);
  ts->to_cond.signal();
  ts->lock.unlock();
  pthread_join(ts->id, NULL);
  thread_lock.lock();
  ts->running = false;
  ts->active = false;
  thread->clearThreadState();
  thread_lock.unlock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN threadEval(leftv result, leftv arg) {
  if (wrong_num_args("threadEval", arg, 2))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("threadEval: argument is not a thread");
    return TRUE;
  }
  SingularThread *thread = *(SingularThread **)arg->Data();
  string expr = LinTree::to_string(arg->next);
  ThreadState *ts = thread->getThreadState();
  if (ts && ts->parent != pthread_self()) {
    WerrorS("threadEval: can only be called from parent thread");
    return TRUE;
  }
  if (ts) ts->lock.lock();
  if (!ts || !ts->running || !ts->active) {
    WerrorS("threadEval: thread is no longer running");
    if (ts) ts->lock.unlock();
    return TRUE;
  }
  ts->to_thread.push(expr);
  ts->to_cond.signal();
  ts->lock.unlock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN threadResult(leftv result, leftv arg) {
  if (wrong_num_args("threadResult", arg, 1))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("threadResult: argument is not a thread");
    return TRUE;
  }
  SingularThread *thread = *(SingularThread **)arg->Data();
  ThreadState *ts = thread->getThreadState();
  if (ts && ts->parent != pthread_self()) {
    WerrorS("threadResult: can only be called from parent thread");
    return TRUE;
  }
  if (ts) ts->lock.lock();
  if (!ts || !ts->running || !ts->active) {
    WerrorS("threadResult: thread is no longer running");
    if (ts) ts->lock.unlock();
    return TRUE;
  }
  while (ts->from_thread.empty()) {
    ts->from_cond.wait();
  }
  string expr = ts->from_thread.front();
  ts->from_thread.pop();
  ts->lock.unlock();
  leftv val = LinTree::from_string(expr);
  result->rtyp = val->Typ();
  result->data = val->Data();
  return FALSE;
}

}


extern "C" int mod_init(SModulFunctions *fn)
{
  const char *libname = currPack->libname;
  if (!libname) libname = "";
  makeSharedType(type_atomic_table, "atomic_table");
  makeSharedType(type_atomic_list, "atomic_list");
  makeSharedType(type_shared_table, "shared_table");
  makeSharedType(type_shared_list, "shared_list");
  makeSharedType(type_channel, "channel");
  makeSharedType(type_syncvar, "syncvar");
  makeSharedType(type_region, "region");
  makeSharedType(type_thread, "thread");
  makeRegionlockType(type_regionlock, "regionlock");

  fn->iiAddCproc(libname, "putTable", FALSE, putTable);
  fn->iiAddCproc(libname, "getTable", FALSE, getTable);
  fn->iiAddCproc(libname, "inTable", FALSE, inTable);
  fn->iiAddCproc(libname, "putList", FALSE, putList);
  fn->iiAddCproc(libname, "getList", FALSE, getList);
  fn->iiAddCproc(libname, "lockRegion", FALSE, lockRegion);
  fn->iiAddCproc(libname, "regionLock", FALSE, regionLock);
  fn->iiAddCproc(libname, "unlockRegion", FALSE, unlockRegion);
  fn->iiAddCproc(libname, "sendChannel", FALSE, sendChannel);
  fn->iiAddCproc(libname, "receiveChannel", FALSE, receiveChannel);
  fn->iiAddCproc(libname, "statChannel", FALSE, statChannel);
  fn->iiAddCproc(libname, "writeSyncVar", FALSE, writeSyncVar);
  fn->iiAddCproc(libname, "readSyncVar", FALSE, readSyncVar);
  fn->iiAddCproc(libname, "statSyncVar", FALSE, statSyncVar);

  fn->iiAddCproc(libname, "makeAtomicTable", FALSE, makeAtomicTable);
  fn->iiAddCproc(libname, "makeAtomicList", FALSE, makeAtomicList);
  fn->iiAddCproc(libname, "makeSharedTable", FALSE, makeSharedTable);
  fn->iiAddCproc(libname, "makeSharedList", FALSE, makeSharedList);
  fn->iiAddCproc(libname, "makeChannel", FALSE, makeChannel);
  fn->iiAddCproc(libname, "makeSyncVar", FALSE, makeSyncVar);
  fn->iiAddCproc(libname, "makeRegion", FALSE, makeRegion);
  fn->iiAddCproc(libname, "findSharedObject", FALSE, findSharedObject);
  fn->iiAddCproc(libname, "bindSharedObject", FALSE, bindSharedObject);
  fn->iiAddCproc(libname, "typeSharedObject", FALSE, typeSharedObject);

  fn->iiAddCproc(libname, "createThread", FALSE, createThread);
  fn->iiAddCproc(libname, "joinThread", FALSE, joinThread);
  fn->iiAddCproc(libname, "threadEval", FALSE, threadEval);
  fn->iiAddCproc(libname, "threadResult", FALSE, threadResult);

  LinTree::init();

  return MAX_TOK;
}
