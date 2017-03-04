#include "types.h"
#include "user.h"
#include "uthrdefs.h"
#include "x86.h"

// Check whether this cpu is holding the lock.
int uholding(struct uspin *lock)
{
  return lock->locked && lock->tid == gettid();
}


int uspin_init(struct uspin *sl) //init
{
    xchg(&sl->tid, 0);
    xchg(&sl->locked, 0);
    return 0;
}

void uspin_destroy(struct uspin *sl)
{
    //kill the whole thing
    sl = 0;
}

void uspin_acquire(struct uspin *sl)
{
    int tid = gettid();
    if(sl == 0) {
        return;
    }

    if(uholding(sl)){
    	exit();
    }

    while(xchg(&sl->locked, 1) != 0) {
        yield(tid);
    }

    __sync_synchronize();
    xchg(&sl->tid, tid);

}

void uspin_release(struct uspin *sl)
{
    if(!uholding(sl)){
        return;
    }

    __sync_synchronize();
    xchg(&sl->tid, 0);
    xchg(&sl->locked, 0);
}

