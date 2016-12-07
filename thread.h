#ifndef _THREAD_H
#define _THREAD_H

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <alloca.h>
#include <cstddef>
#include <exception>

typedef pthread_t Thread;

void ThreadError(const char *message);

class ConditionVariable;

class Lock {
private:
  pthread_mutex_t mutex;
  friend class ConditionVariable;
  Thread owner;
  int locked;
  void set_owner() {
    owner = pthread_self();
    locked = 1;
  }
public:
  Lock() {
    pthread_mutex_init(&mutex, NULL);
    locked = 0;
  }
  ~Lock() {
    pthread_mutex_destroy(&mutex);
  }
  void lock() {
    Thread self = pthread_self();
    if (locked && owner == self)
      ThreadError("locking mutex twice");
    pthread_mutex_lock(&mutex);
    locked = 1;
    owner = self;
  }
  void unlock() {
    if (owner != pthread_self())
      ThreadError("unlocking unowned lock");
    locked = 0;
    pthread_mutex_unlock(&mutex);
  }
  int is_locked() {
    return locked && owner == pthread_self();
  }
};

class ConditionVariable {
  friend class Lock;
private:
  pthread_cond_t condition;
  Lock *lock;
  friend class Semaphore;
  ConditionVariable() { }
public:
  ConditionVariable(Lock *lock0) {
    lock = lock0;
    pthread_cond_init(&condition, NULL);
  }
  ~ConditionVariable() {
    pthread_cond_destroy(&condition);
  }
  void wait() {
    if (!lock->is_locked())
      ThreadError("waited on condition without locked mutex");
    pthread_cond_wait(&condition, &lock->mutex);
    lock->set_owner();
  }
  void signal() {
    if (!lock->is_locked())
      ThreadError("signaled condition without locked mutex");
    pthread_cond_signal(&condition);
  }
  void broadcast() {
    if (!lock->is_locked())
      ThreadError("signaled condition without locked mutex");
    pthread_cond_broadcast(&condition);
  }
};

class Semaphore {
private:
  Lock lock;
  ConditionVariable condition;
  unsigned count;
  unsigned waiting;
public:
  Semaphore() {
    condition.lock = &lock;
    count = 0;
  }
  Semaphore(unsigned count0) {
    count = count0;
  }
  void wait();
  void post();
};

#endif // _THREAD_H
