#include <list>
#include <vector>

#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include "thread.h"

void ThreadError(const char *message) {
  fprintf(stderr, "FATAL ERROR: %s\n", message);
  abort(); // should not happen
}

void Semaphore::wait() {
  lock.lock();
  waiting++;
  while (count == 0)
    condition.wait();
  waiting--;
  count--;
  lock.unlock();
}

void Semaphore::post() {
  lock.lock();
  if (count++ == 0 && waiting)
    condition.signal();
  lock.unlock();
}
