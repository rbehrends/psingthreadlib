#ifndef _singthread_shared_h
#define _singthread_shared_h

#include "thread.h"
#include <map>
#include <string>
#include <stdio.h>

class SharedObject {
private:
  Lock lock;
  long refcount;
  int type;
public:
  SharedObject(): lock(), refcount(0) { }
  virtual ~SharedObject() { }
  void set_type(int type_init) { type = type_init; }
  int get_type() { return type; }
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

#endif
