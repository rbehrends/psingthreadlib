#include <iostream>
#include "kernel/mod2.h"
#include "Singular/ipid.h"
#include "Singular/links/silink.h"
#include "Singular/lists.h"
#include "Singular/blackbox.h"
#include <cstring>
#include <errno.h>
#include <vector>
#include <map>
#include <iterator>
#include <queue>
#include "thread.h"
#include "shared.h"
#include "lintree.h"

using namespace std;

namespace { // putting the implementation inside an anonymous namespace

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
    table.insert(make_pair<string,SharedObject *>(name, result));
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
        WerrorS("table access: region not locked");
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
      SharedObject(), region(NULL) {
    lock = NULL;
    if (!region)
      lock = new Lock();
  }
  Transactional(Region *region_init) :
      SharedObject(), region(region_init) {
    lock = NULL;
    if (!region)
      lock = new Lock();
    else
      lock = region->get_lock();
  }
  virtual ~Transactional() { if (!region) delete lock; }
};

class TxTable: public Transactional {
private:
  map<string, string> entries;
public:
  TxTable() : Transactional(), entries() { }
  TxTable(Region *region_init): Transactional(region_init), entries() { }
  virtual ~TxTable() { }
  int put(string &key, string &value) {
    int result = 0;
    if (!tx_begin()) return -1;
    if (entries.count(key)) {
      entries[key] = value;
    } else {
      entries.insert(make_pair<string, string>(key, value));
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
  TxList(Region *region_init): Transactional(region_init), entries() { }
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
      cond.broadcast();
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
};

void *shared_init(blackbox *b) {
  return omAlloc0(sizeof(SharedObject *));
}

void *new_shared(SharedObject *obj) {
  acquireShared(obj);
  void *result = omAlloc0(sizeof(long));
  *(SharedObject **)result = obj;
  return result;
}

void shared_destroy(blackbox *b, void *d) {
  releaseShared(*(SharedObject **)d);
  *(SharedObject **)d = NULL;
}

void *shared_copy(blackbox *b, void *d) {
  void *result = shared_init(b);
  *(SharedObject **)result = *(SharedObject **)d;
  acquireShared(*(SharedObject **)result);
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
      omFree(ll->data);
      ll->data = shared_copy(NULL,r->Data());
    }
  }
  return FALSE;
}

char *shared_string(blackbox *b, void *d) {
  char buf[32];
  SharedObject *obj = *(SharedObject **)d;
  int type = obj->get_type();
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
  sprintf(buf, "<%s %p>", type_name, (void *)obj);
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
  Region *region = (Region *) arg->Data();
  string s = str(arg->next);
  SharedObject *obj = makeSharedObject(region->objects,
    region->get_lock(), type_shared_table, s, consTable);
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
  Region *region = (Region *) arg->Data();
  string s = str(arg->next);
  SharedObject *obj = makeSharedObject(region->objects,
    region->get_lock(), type_shared_list, s, consList);
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
  TxTable *table = (TxTable *) arg->Data();
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
  TxTable *table = (TxTable *) arg->Data();
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
  TxTable *table = (TxTable *) arg->Data();
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

void makeSharedType(int &type, const char *name) {
  blackbox *b=(blackbox*)omAlloc0(sizeof(blackbox));
  b->blackbox_Init = shared_init;
  b->blackbox_destroy = shared_destroy;
  b->blackbox_Copy = shared_copy;
  b->blackbox_String = shared_string;
  b->blackbox_Assign = shared_assign;
  type = setBlackboxStuff(b, name);
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
  makeSharedType(type_regionlock, "regionlock");

  fn->iiAddCproc(libname, "putTable", FALSE, putTable);
  fn->iiAddCproc(libname, "getTable", FALSE, getTable);
  fn->iiAddCproc(libname, "inTable", FALSE, inTable);
  fn->iiAddCproc(libname, "makeAtomicTable", FALSE, makeAtomicTable);
  fn->iiAddCproc(libname, "makeAtomicList", FALSE, makeAtomicList);
  fn->iiAddCproc(libname, "makeSharedTable", FALSE, makeSharedTable);
  fn->iiAddCproc(libname, "makeSharedList", FALSE, makeSharedList);
  fn->iiAddCproc(libname, "makeChannel", FALSE, makeChannel);
  fn->iiAddCproc(libname, "makeSyncVar", FALSE, makeSyncVar);
  fn->iiAddCproc(libname, "makeRegion", FALSE, makeRegion);
  return MAX_TOK;
}
