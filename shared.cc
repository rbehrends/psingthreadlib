#include <factory/prelude.h>
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

#include "singthreads.h"

using namespace std;

extern char *global_argv0;

namespace LibThread {

class Command {
private:
  const char *name;
  const char *error;
  leftv result;
  leftv *args;
  int argc;
public:
  Command(const char *n, leftv r, leftv a)
  {
    name = n;
    result = r;
    error = NULL;
    argc = 0;
    for (leftv t = a; t != NULL; t = t->next) {
      argc++;
    }
    args = (leftv *) omAlloc0(sizeof(leftv) * argc);
    int i = 0;
    for (leftv t = a; t != NULL; t = t->next) {
      args[i++] = t;
    }
    result->rtyp = NONE;
    result->data = NULL;
  }
  ~Command() {
    omFree(args);
  }
  void check_argc(int n) {
    if (error) return;
    if (argc != n) error = "wrong number of arguments";
  }
  void check_argc(int lo, int hi) {
    if (error) return;
    if (argc < lo || argc > hi) error = "wrong number of arguments";
  }
  void check_argc_min(int n) {
    if (error) return;
    if (argc < n) error = "wrong number of arguments";
  }
  void check_arg(int i, int type, const char *err) {
    if (error) return;
    if (args[i]->Typ() != type) error = err;
  }
  void check_init(int i, const char *err) {
    if (error) return;
    leftv arg = args[i];
    if (arg->Data() == NULL || *(void **)(arg->Data()) == NULL)
      error = err;
  }
  void check_arg(int i, int type, int type2, const char *err) {
    if (error) return;
    if (args[i]->Typ() != type && args[i]->Typ() != type2) error = err;
  }
  int argtype(int i) {
    return args[i]->Typ();
  }
  int nargs() {
    return argc;
  }
  void *arg(int i) {
    return args[i]->Data();
  }
  long int_arg(int i) {
    return (long)(args[i]->Data());
  }
  void report(const char *err) {
    error = err;
  }
  // intentionally not bool, so we can also do
  // q = p + test_arg(p, type);
  int test_arg(int i, int type) {
    if (i >= argc) return 0;
    return args[i]->Typ() == type;
  }
  void set_result(long n) {
    result->rtyp = INT_CMD;
    result->data = (char *)n;
  }
  void set_result(const char *s) {
    result->rtyp = STRING_CMD;
    result->data = omStrDup(s);
  }
  void set_result(int type, void *p) {
    result->rtyp = type;
    result->data = (char *) p;
  }
  void set_result(int type, long n) {
    result->rtyp = type;
    result->data = (char *) n;
  }
  void no_result() {
    result->rtyp = NONE;
  }
  bool ok() {
    return error == NULL;
  }
  BOOLEAN status() {
    if (error) {
      Werror("%s: %s", name, error);
    }
    return error != NULL;
  }
};

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
  void incref(int by = 1) {
    lock.lock();
    refcount += 1;
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
    // delete obj;
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
Lock master_lock;
SIMPLE_THREAD_VAR long thread_id;
long thread_counter;

int type_region;
int type_regionlock;
int type_channel;
int type_syncvar;
int type_atomic_table;
int type_shared_table;
int type_atomic_list;
int type_shared_list;
int type_thread;
int type_threadpool;
int type_job;
int type_trigger;

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

class SingularChannel : public SharedObject {
private:
  queue<string> q;
  Lock lock;
  ConditionVariable cond;
public:
  SingularChannel(): SharedObject(), lock(), cond(&lock) { }
  virtual ~SingularChannel() { }
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

class SingularSyncVar : public SharedObject {
private:
  string value;
  int init;
  Lock lock;
  ConditionVariable cond;
public:
  SingularSyncVar(): SharedObject(), init(0), lock(), cond(&lock) { }
  virtual ~SingularSyncVar() { }
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
      if (ll->data) {
	shared_destroy(NULL, ll->data);
	omFree(ll->data);
      }
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
  else if (type == type_threadpool) {
    sprintf(buf, "<threadpool #%p>", obj);
    return omStrDup(buf);
  }
  else if (type == type_job) {
    sprintf(buf, "<job #%p>", obj);
    return omStrDup(buf);
  }
  else if (type == type_trigger) {
    sprintf(buf, "<trigger #%p>", obj);
    return omStrDup(buf);
  } else {
    sprintf(buf, "<unknown type %d>", type);
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
  if (arg->Typ() != type_region || !arg->Data()) {
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
  return new SingularChannel();
}

SharedObject *consSyncVar() {
  return new SingularSyncVar();
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
  if (!table) {
    WerrorS("getTable: table has not been initialized");
    return TRUE;
  }
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
  if (!table) {
    WerrorS("inTable: table has not been initialized");
    return TRUE;
  }
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
  if (!table) {
    WerrorS("putTable: table has not been initialized");
    return TRUE;
  }
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
  if (!list) {
    WerrorS("getList: list has not been initialized");
    return TRUE;
  }
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
  if (!list) {
    WerrorS("putList: list has not been initialized");
    return TRUE;
  }
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
  SingularChannel *channel = *(SingularChannel **)arg->Data();
  if (!channel) {
    WerrorS("sendChannel: channel has not been initialized");
    return TRUE;
  }
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
  SingularChannel *channel = *(SingularChannel **)arg->Data();
  if (!channel) {
    WerrorS("receiveChannel: channel has not been initialized");
    return TRUE;
  }
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
  SingularChannel *channel = *(SingularChannel **)arg->Data();
  if (!channel) {
    WerrorS("receiveChannel: channel has not been initialized");
    return TRUE;
  }
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
  SingularSyncVar *syncvar = *(SingularSyncVar **)arg->Data();
  if (!syncvar) {
    WerrorS("writeSyncVar: syncvar has not been initialized");
    return TRUE;
  }
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
  SingularSyncVar *syncvar = *(SingularSyncVar **)arg->Data();
  if (!syncvar) {
    WerrorS("readSyncVar: syncvar has not been initialized");
    return TRUE;
  }
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
  SingularSyncVar *syncvar = *(SingularSyncVar **)arg->Data();
  if (!syncvar) {
    WerrorS("statSyncVar: syncvar has not been initialized");
    return TRUE;
  }
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
  if (type != 0) return;
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
  if (type != 0) return;
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
  void *(*thread_func)(ThreadState *, void *);
  void *arg, *result;
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

void thread_init() {
  master_lock.lock();
  thread_id = ++thread_counter;
  master_lock.unlock();
  onThreadInit();
  siInit(global_argv0);
  setOption('q');
  // setOption('b');
}

void *thread_main(void *arg) {
  ThreadState *ts = (ThreadState *)arg;
  thread_init();
  return ts->thread_func(ts, ts->arg);
}

void *interpreter_thread(ThreadState *ts, void *arg) {
  ts->lock.lock();
  for (;;) {
    bool eval = false;
    while (ts->to_thread.empty())
      ts->to_cond.wait();
    /* TODO */
    string expr = ts->to_thread.front();
    switch (expr[0]) {
      case '\0': case 'q':
        ts->lock.unlock();
	return NULL;
      case 'x':
        eval = false;
	break;
      case 'e':
        eval = true;
	break;
    }
    ts->to_thread.pop();
    expr = ts->to_thread.front();
    /* this will implicitly eval commands */
    leftv val = LinTree::from_string(expr);
    expr = LinTree::to_string(val);
    ts->to_thread.pop();
    if (eval)
      ts->from_thread.push(expr);
    ts->from_cond.signal();
  }
  ts->lock.unlock();
  return NULL;
}

class InterpreterThread : public SharedObject {
private:
  ThreadState *ts;
public:
  InterpreterThread(ThreadState *ts_init) : SharedObject(), ts(ts_init) { }
  virtual ~InterpreterThread() { }
  ThreadState *getThreadState() { return ts; }
  void clearThreadState() {
    ts = NULL;
  }
};

static ThreadState *newThread(void *(*thread_func)(ThreadState *, void *),
    void *arg, const char **error) {
  ThreadState *ts = NULL;
  if (error) *error = NULL;
  thread_lock.lock();
  for (int i=0; i<MAX_THREADS; i++) {
    if (!thread_state[i].active) {
      ts = thread_state + i;
      ts->index = i;
      if (pthread_create(&ts->id, NULL, thread_main, ts)<0) {
	if (error)
	  *error = "createThread: internal error: failed to create thread";
	goto fail;
      }
      ts->parent = pthread_self();
      ts->active = true;
      ts->running = true;
      ts->to_thread = queue<string>();
      ts->from_thread = queue<string>();
      ts->thread_func = thread_func;
      ts->arg = arg;
      ts->result = NULL;
      goto exit;
    }
  }
  if (error) *error = "createThread: too many threads";
  fail:
  ts = NULL;
  exit:
  thread_lock.unlock();
  return ts;
}

ThreadState *createThread(void *(*thread_func)(ThreadState *, void *),
    void *arg) {
  return newThread(thread_func, arg, NULL);
}

void *joinThread(ThreadState *ts) {
  void *result;
  pthread_join(ts->id, NULL);
  result = ts->result;
  thread_lock.lock();
  ts->running = false;
  ts->active = false;
  thread_lock.unlock();
}

static InterpreterThread *createInterpreterThread(const char **error) {
  ThreadState *ts = newThread(interpreter_thread, NULL, error);
  if (*error) return NULL;
  InterpreterThread *thread = new InterpreterThread(ts);
  char buf[10];
  sprintf(buf, "%d", ts->index);
  string name(buf);
  thread->set_name(name);
  thread->set_type(type_thread);
  return thread;
}

static BOOLEAN createThread(leftv result, leftv arg) {
  Command cmd("createThread", result, arg);
  cmd.check_argc(0);
  const char *error;
  if (!cmd.ok()) return cmd.status();
  InterpreterThread *thread = createInterpreterThread(&error);
  if (error) {
    cmd.report(error);
    return cmd.status();
  }
  cmd.set_result(type_thread, new_shared(thread));
  return cmd.status();
}

static bool joinInterpreterThread(InterpreterThread *thread) {
  ThreadState *ts = thread->getThreadState();
  if (ts && ts->parent != pthread_self()) {
    return false;
  }
  ts->lock.lock();
  string quit("q");
  ts->to_thread.push(quit);
  ts->to_cond.signal();
  ts->lock.unlock();
  pthread_join(ts->id, NULL);
  thread_lock.lock();
  ts->running = false;
  ts->active = false;
  thread->clearThreadState();
  thread_lock.unlock();
  return true;
}

static BOOLEAN joinThread(leftv result, leftv arg) {
  if (wrong_num_args("joinThread", arg, 1))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("joinThread: argument is not a thread");
    return TRUE;
  }
  InterpreterThread *thread = *(InterpreterThread **)arg->Data();
  if (!joinInterpreterThread(thread)) {
    WerrorS("joinThread: can only be called from parent thread");
    return TRUE;
  }
  return FALSE;
}

class ThreadPool;

class Job : public SharedObject {
public:
  ThreadPool *pool;
  long pending_index;
  vector<Job *> deps;
  vector<Job *> notify;
  vector<string> args;
  string result; // lintree-encoded
  bool done;
  bool queued;
  bool running;
  Lock lock;
  Job() : SharedObject(), pool(NULL), deps(), pending_index(-1),
    done(false), running(false), queued(false), result(), lock(),
    args(), notify()
  { set_type(type_job); }
  ~Job();
  void addDep(Job *job) {
    deps.push_back(job);
  }
  void addDep(vector<Job *> &jobs);
  void addNotify(vector<Job *> &jobs);
  bool ready();
  virtual void execute() = 0;
};

bool Job::ready() {
  bool result = true;
  vector<Job *>::iterator it;
  for (it = deps.begin(); it != deps.end(); it++) {
    result &= (*it)->done;
  }
  return result;
}

Job::~Job() {
  vector<Job *>::iterator it;
  for (it = deps.begin(); it != deps.end(); it++) {
    releaseShared(*it);
  }
}

typedef queue<Job *> JobQueue;

struct PoolInfo {
  ThreadPool *pool;
  int num;
};

static SIMPLE_THREAD_VAR ThreadPool *currentThreadPoolRef;
static SIMPLE_THREAD_VAR Job *currentJobRef;

class ThreadPool : public SharedObject {
private:
  bool single_threaded;
  int nthreads;
  bool shutting_down;
  int shutdown_counter;
  vector<ThreadState *> threads;
  JobQueue global_queue;
  vector<JobQueue *> thread_queues;
  vector<Job *> pending;
  Lock lock;
  ConditionVariable cond;
  ConditionVariable response;
public:
  ThreadPool(int n) :
    SharedObject(), threads(), global_queue(), thread_queues(),
    single_threaded(n==0), nthreads(n == 0 ? 1 : n),
    lock(), cond(&lock), response(&lock),
    shutting_down(false), shutdown_counter(0)
  {
    thread_queues.push_back(new JobQueue());
  }
  virtual ~ThreadPool() {
    for (int i = 0; i < thread_queues.size(); i++) {
      JobQueue *q = thread_queues[i];
      while (!q->empty()) {
        Job *job = q->front();
	q->pop();
	releaseShared(job);
      }
    }
    thread_queues.clear();
    threads.clear();
  }
  ThreadState *getThread(int i) { return threads[i]; }
  void shutdown(bool wait) {
    if (single_threaded) {
      PoolInfo *info = new PoolInfo();
      info->num = 0;
      info->pool = this;
      ThreadPool::main(NULL, info);
      return;
    }
    lock.lock();
    if (wait) {
      while (!global_queue.empty()) {
        response.wait();
      }
    }
    shutting_down = true;
    while (shutdown_counter < nthreads) {
      cond.broadcast();
      response.wait();
    }
    lock.unlock();
    for (int i = 0; i <threads.size(); i++) {
      joinThread(threads[i]);
    }
  }
  void addThread(ThreadState *thread) {
    threads.push_back(thread);
    thread_queues.push_back(new JobQueue());
  }
  void attachJob(Job *job) {
    lock.lock();
    job->lock.lock();
    job->pool = this;
    acquireShared(job);
    if (job->ready()) {
      global_queue.push(job);
    }
    else if (job->pending_index < 0) {
      job->pool = this;
      job->pending_index = pending.size();
      pending.push_back(job);
    }
    job->lock.unlock();
    lock.unlock();
  }
  void detachJob(Job *job) {
    lock.lock();
    job->lock.lock();
    long i = job->pending_index;
    job->pending_index = -1;
    job->lock.unlock();
    if (i >= 0) {
      job = pending.back();
      job->lock.lock();
      pending.resize(pending.size()-1);
      pending[i] = job;
      job->pending_index = i;
      job->lock.unlock();
    }
    lock.unlock();
  }
  void queueJob(Job *job) {
    // assumes that job is already locked
    lock.lock();
    global_queue.push(job);
    cond.signal();
    lock.unlock();
  }
  void broadcastJob(Job *job) {
    lock.lock();
    for (int i = 0; i <thread_queues.size(); i++) {
      acquireShared(job);
      thread_queues[i]->push(job);
    }
    lock.unlock();
  }
  void waitJob(Job *job) {
    lock.lock();
    for (;;) {
      job->lock.lock();
      if (job->done) {
	job->lock.unlock();
	break;
      }
      job->lock.unlock();
      response.wait();
    }
    response.signal(); // forward signal
    lock.unlock();
  }
  void clearThreadState() {
    threads.clear();
  }
  static void notifyDeps(ThreadPool *pool, Job *job) {
    vector<Job *> &notify = job->notify;
    job->incref(notify.size());
    for (int i = 0; i <notify.size(); i++) {
      Job *next = notify[i];
      next->lock.lock();
      if (!next->queued && next->ready()) {
        next->queued = true;
        next->lock.unlock();
        pool->queueJob(next);
      } else
        next->lock.unlock();
    }
  }
  static void *main(ThreadState *ts, void *arg) {
    PoolInfo *info = (PoolInfo *) arg;
    ThreadPool *pool = info->pool;
    currentThreadPoolRef = pool;
    Lock &lock = pool->lock;
    ConditionVariable &cond = pool->cond;
    ConditionVariable &response = pool->response;
    JobQueue *my_queue = pool->thread_queues[info->num];
    thread_init();
    pool->lock.lock();
    for (;;) {
      if (pool->shutting_down) {
        pool->response.signal();
	break;
      }
      if (!my_queue->empty()) {
       Job *job = my_queue->front();
       my_queue->pop();
       lock.unlock();
       currentJobRef = job;
       job->execute();
       currentJobRef = NULL;
       job->lock.lock();
       job->done = true;
       notifyDeps(pool, job);
       job->lock.unlock();
       releaseShared(job);
       pool->response.signal();
       lock.lock();
      } else if (!pool->global_queue.empty()) {
       Job *job = pool->global_queue.front();
       pool->global_queue.pop();
       lock.unlock();
       currentJobRef = job;
       job->execute();
       currentJobRef = NULL;
       job->done = true;
       notifyDeps(pool, job);
       releaseShared(job);
       pool->response.signal();
       lock.lock();
      } else {
        if (pool->single_threaded) {
          break;
        }
        cond.wait();
      }
    }
    currentThreadPoolRef = NULL;
    pool->lock.unlock();
    delete info;
    return NULL;
  }
};
void Job::addDep(vector<Job *> &jobs) {
  deps.insert(deps.end(), jobs.begin(), jobs.end());
}
void Job::addNotify(vector<Job *> &jobs) {
  lock.lock();
  notify.insert(notify.end(), jobs.begin(), jobs.end());
  if (done) {
    lock.unlock();
    ThreadPool::notifyDeps(pool, this);
  }
  else
    lock.unlock();
}

static BOOLEAN createThreadPool(leftv result, leftv arg) {
  long n;
  Command cmd("createThreadPool", result, arg);
  cmd.check_argc(1, 2);
  cmd.check_arg(0, INT_CMD, "first argument must be an integer");
  if (cmd.ok()) {
    n = (long) cmd.arg(0);
    if (n < 0) cmd.report("number of threads must be non-negative");
    else if (n >= 256) cmd.report("number of threads too large");
  }
  if (cmd.ok()) {
    ThreadPool *pool = new ThreadPool((int) n);
    pool->set_type(type_threadpool);
    for (int i = 0; i <n; i++) {
      const char *error;
      PoolInfo *info = new PoolInfo();
      info->pool = pool;
      info->num = i;
      ThreadState *thread = newThread(ThreadPool::main, info, &error);
      if (!thread) {
        cmd.report(error);
	return cmd.status();
      }
      pool->addThread(thread);
    }
    cmd.set_result(type_threadpool, new_shared(pool));
  }
  return cmd.status();
}

static BOOLEAN closeThreadPool(leftv result, leftv arg) {
  Command cmd("closeThreadPool", result, arg);
  cmd.check_argc(1, 2);
  cmd.check_arg(0, type_threadpool, "first argument must be a threadpool");
  cmd.check_init(0, "threadpool not initialized");
  if (cmd.nargs() > 1)
    cmd.check_arg(1, INT_CMD, "optional argument must be an integer");
  if (cmd.ok()) {
    ThreadPool *pool = *(ThreadPool **)(cmd.arg(0));
    bool wait = cmd.nargs() == 2 ? (cmd.int_arg(1) != 0) : 1;
    pool->shutdown(wait);
    cmd.no_result();
  }
  return cmd.status();
}

BOOLEAN currentThreadPool(leftv result, leftv arg) {
  Command cmd("currentThreadPool", result, arg);
  cmd.check_argc(0);
  ThreadPool *pool = currentThreadPoolRef;
  if (pool) {
    cmd.set_result(type_threadpool, new_shared(pool));
  } else {
    cmd.report("no current threadpool");
  }
  return cmd.status();
}



class EvalJob : public Job {
public:
  EvalJob() : Job() { }
  virtual void execute() {
    leftv val = LinTree::from_string(args[0]);
    result = LinTree::to_string(val);
    val->CleanUp();
    omFreeBin(val, sleftv_bin);
  }
};

class ExecJob : public Job {
public:
  ExecJob() : Job() { }
  virtual void execute() {
    leftv val = LinTree::from_string(args[0]);
    val->CleanUp();
    omFreeBin(val, sleftv_bin);
  }
};

class ProcJob : public Job {
  string procname;
public:
  ProcJob(const char *procname_init) : Job(),
    procname(procname_init) {
  }
  void appendArg(vector<leftv> &argv, string &s) {
    if (s.size() == 0) return;
    leftv val = LinTree::from_string(s);
    if (val->Typ() == NONE) {
      omFreeBin(val, sleftv_bin);
      return;
    }
    argv.push_back(val);
  }
  virtual void execute() {
    vector<leftv> argv;
    for (int i = 0; i <args.size(); i++) {
      appendArg(argv, args[i]);
    }
    for (int i = 0; i < deps.size(); i++) {
      appendArg(argv, deps[i]->result);
    }
    leftv procnode = (leftv) omAlloc0Bin(sleftv_bin);
    procnode->name = omStrDup(procname.c_str());
    procnode->req_packhdl = basePack;
    int error = procnode->Eval();
    if (error) {
      Werror("job execution: procedure \"%s\" not found", procname.c_str());
      return;
    }
    sleftv val;
    memset(&val, 0, sizeof(val));
    leftv *tail = &procnode->next;
    for (int i = 0; i < argv.size(); i++) {
      *tail = argv[i];
      tail = &(*tail)->next;
    }
    *tail = NULL;
    error = iiExprArithM(&val, procnode, '(');
    if (error) {
      Werror("job execution: procedure call of \"%s\" failed", procname.c_str());
      return;
    }
    result = LinTree::to_string(&val);
    // val.CleanUp();
  }
};

static BOOLEAN createJob(leftv result, leftv arg) {
  Command cmd("createJob", result, arg);
  cmd.check_argc_min(1);
  cmd.check_arg(0, STRING_CMD, COMMAND,
    "job name must be a string or quote expression");
  if (cmd.ok()) {
    if (cmd.test_arg(0, STRING_CMD)) {
      ProcJob *job = new ProcJob((char *)(cmd.arg(0)));
      for (leftv a = arg->next; a != NULL; a = a->next) {
        job->args.push_back(LinTree::to_string(a));
      }
      cmd.set_result(type_job, new_shared(job));
    } else {
      cmd.check_argc(1);
      Job *job = new EvalJob();
      job->args.push_back(LinTree::to_string(arg));
      cmd.set_result(type_job, new_shared(job));
    }
  }
  return cmd.status();
}

static BOOLEAN startJob(leftv result, leftv arg) {
  Command cmd("startJob", result, arg);
  cmd.check_argc_min(2);
  cmd.check_arg(0, type_threadpool, "first argument must be a threadpool");
  cmd.check_init(0, "threadpool not initialized");
  cmd.check_arg(1, type_job, STRING_CMD, "second argument must be a job");
  if (cmd.ok() && cmd.argtype(1) == type_job)
    cmd.check_init(1, "job not initialized");
  if (!cmd.ok()) return cmd.status();
  ThreadPool *pool = *(ThreadPool **) cmd.arg(0);
  Job *job;
  if (cmd.argtype(1) == type_job) 
    job = *(Job **)(cmd.arg(1));
  else
    job = new ProcJob((char *)(cmd.arg(1)));
  for (leftv a = arg->next->next; a != NULL; a = a->next) {
    job->args.push_back(LinTree::to_string(a));
  }
  if (job->pool)
    cmd.report("job has already been scheduled");
  else
    pool->attachJob(job);
  cmd.set_result(type_job, new_shared(job));
  return cmd.status();
}

static BOOLEAN waitJob(leftv result, leftv arg) {
  Command cmd("waitJob", result, arg);
  cmd.check_argc(1);
  cmd.check_arg(0, type_job, "argument must be a job");
  cmd.check_init(0, "job not initialized");
  if (cmd.ok()) {
    Job *job = *(Job **)(cmd.arg(0));
    ThreadPool *pool = job->pool;
    if (!pool) {
      cmd.report("job has not yet been started");
      return cmd.status();
    }
    pool->waitJob(job);
    if (job->result.size() == 0)
      cmd.no_result();
    else {
      leftv res = LinTree::from_string(job->result);
      cmd.set_result(res->Typ(), res->Data());
    }
  }
  return cmd.status();
}

static BOOLEAN scheduleJob(leftv result, leftv arg) {
  vector<Job *> jobs;
  vector<Job *> deps;
  Command cmd("scheduleJob", result, arg);
  cmd.check_argc_min(2);
  cmd.check_arg(0, type_threadpool, "first argument must be a threadpool");
  cmd.check_init(0, "threadpool not initialized");
  ThreadPool *pool = *(ThreadPool **) cmd.arg(0);
  if (cmd.test_arg(1, type_job)) {
    jobs.push_back(*(Job **)(cmd.arg(1)));
  } else if (cmd.test_arg(1, STRING_CMD)) {
    jobs.push_back(new ProcJob((char *)(cmd.arg(1))));
  }
  bool error = false;
  for (leftv a = arg->next->next; !error && a; a = a->next) {
    if (a->Typ() == type_job) {
      deps.push_back(*(Job **)(a->Data()));
    } else if (a->Typ() == LIST_CMD) {
      lists l = (lists) a->Data();
      int n = lSize(l);
      for (int i = 0; i < n; i++) {
        if (l->m[i].Typ() == type_job) {
          deps.push_back(*(Job **)(l->m[i].Data()));
        } else {
          error = true;
          break;
        }
      }
    }
  }
  if (error) {
    cmd.report("illegal dependency");
    return cmd.status();
  }
  for (int i = 0; i < jobs.size(); i++) {
    Job *job = jobs[i];
    if (job->pool) {
      cmd.report("job has already been scheduled");
      return cmd.status();
    }
  }
  for (int i = 0; i < deps.size(); i++) {
    Job *job = deps[i];
    if (!job->pool) {
      cmd.report("dependency has not yet been scheduled");
      return cmd.status();
    }
    if (job->pool != pool) {
      cmd.report("dependency has been scheduled on a different threadpool");
      return cmd.status();
    }
  }
  for (int i = 0; i < jobs.size(); i++) {
    jobs[i]->addDep(deps);
  }
  for (int i = 0; i < deps.size(); i++) {
    deps[i]->addNotify(jobs);
  }
  for (int i = 0; i < jobs.size(); i++) {
    pool->attachJob(jobs[i]);
  }
  if (jobs.size() > 0)
    cmd.set_result(type_job, new_shared(jobs[0]));
  return cmd.status();
}

BOOLEAN currentJob(leftv result, leftv arg) {
  Command cmd("currentJob", result, arg);
  cmd.check_argc(0);
  Job *job = currentJobRef;
  if (job) {
    cmd.set_result(type_job, new_shared(job));
  } else {
    cmd.report("no current job");
  }
  return cmd.status();
}


BOOLEAN threadID(leftv result, leftv arg) {
  int i;
  if (wrong_num_args("threadID", arg, 0))
    return TRUE;
  result->rtyp = INT_CMD;
  result->data = (char *)thread_id;
  return FALSE;
}

BOOLEAN mainThread(leftv result, leftv arg) {
  int i;
  if (wrong_num_args("mainThread", arg, 0))
    return TRUE;
  result->rtyp = INT_CMD;
  result->data = (char *)(long)(thread_id == 0L);
  return FALSE;
}

BOOLEAN threadEval(leftv result, leftv arg) {
  if (wrong_num_args("threadEval", arg, 2))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("threadEval: argument is not a thread");
    return TRUE;
  }
  InterpreterThread *thread = *(InterpreterThread **)arg->Data();
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
  ts->to_thread.push("e");
  ts->to_thread.push(expr);
  ts->to_cond.signal();
  ts->lock.unlock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN threadExec(leftv result, leftv arg) {
  if (wrong_num_args("threadExec", arg, 2))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("threadExec: argument is not a thread");
    return TRUE;
  }
  InterpreterThread *thread = *(InterpreterThread **)arg->Data();
  string expr = LinTree::to_string(arg->next);
  ThreadState *ts = thread->getThreadState();
  if (ts && ts->parent != pthread_self()) {
    WerrorS("threadExec: can only be called from parent thread");
    return TRUE;
  }
  if (ts) ts->lock.lock();
  if (!ts || !ts->running || !ts->active) {
    WerrorS("threadExec: thread is no longer running");
    if (ts) ts->lock.unlock();
    return TRUE;
  }
  ts->to_thread.push("x");
  ts->to_thread.push(expr);
  ts->to_cond.signal();
  ts->lock.unlock();
  result->rtyp = NONE;
  return FALSE;
}

BOOLEAN threadPoolExec(leftv result, leftv arg) {
  Command cmd("threadPoolExec", result, arg);
  cmd.check_argc(2);
  cmd.check_arg(0, type_threadpool, "first argument must be a threadpool");
  cmd.check_init(0, "threadpool not initialized");
  if (cmd.ok()) {
    ThreadPool *pool = *(ThreadPool **) (cmd.arg(0));
    string expr = LinTree::to_string(arg->next);
    Job* job = new ExecJob();
    job->args.push_back(expr);
    pool->broadcastJob(job);
  }
  return cmd.status();
}

BOOLEAN threadResult(leftv result, leftv arg) {
  if (wrong_num_args("threadResult", arg, 1))
    return TRUE;
  if (arg->Typ() != type_thread) {
    WerrorS("threadResult: argument is not a thread");
    return TRUE;
  }
  InterpreterThread *thread = *(InterpreterThread **)arg->Data();
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

using namespace LibThread;


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
  makeSharedType(type_threadpool, "threadpool");
  makeSharedType(type_job, "job");
  makeSharedType(type_trigger, "trigger");
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
  fn->iiAddCproc(libname, "createThreadPool", FALSE, createThreadPool);
  fn->iiAddCproc(libname, "closeThreadPool", FALSE, closeThreadPool);
  fn->iiAddCproc(libname, "currentThreadPool", FALSE, currentThreadPool);
  fn->iiAddCproc(libname, "threadPoolExec", FALSE, threadPoolExec);
  fn->iiAddCproc(libname, "threadID", FALSE, threadID);
  fn->iiAddCproc(libname, "mainThread", FALSE, mainThread);
  fn->iiAddCproc(libname, "threadEval", FALSE, threadEval);
  fn->iiAddCproc(libname, "threadExec", FALSE, threadExec);
  fn->iiAddCproc(libname, "threadResult", FALSE, threadResult);
  fn->iiAddCproc(libname, "createJob", FALSE, createJob);
  fn->iiAddCproc(libname, "currentJob", FALSE, currentJob);
  // fn->iiAddCproc(libname, "nameJob", FALSE, nameJob);
  // fn->iiAddCproc(libname, "getJobName", FALSE, getJobName);
  fn->iiAddCproc(libname, "startJob", FALSE, startJob);
  fn->iiAddCproc(libname, "waitJob", FALSE, waitJob);
  fn->iiAddCproc(libname, "scheduleJob", FALSE, scheduleJob);
  fn->iiAddCproc(libname, "scheduleJobs", FALSE, scheduleJob);
  // fn->iiAddCproc(libname, "createTrigger", FALSE, createTrigger);
  // fn->iiAddCproc(libname, "activateTrigger", FALSE, activateTrigger);

  LinTree::init();

  return MAX_TOK;
}
