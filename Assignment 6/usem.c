#include "types.h"
#include "user.h"
#include "uthr.h"
#include "uthrdefs.h"
#include "x86.h"
#include "limits.h"

int usem_init(struct usem *u, uint c) {
    if (c > INT_MAX) {
        //if initial count greater than max
        return -1;
    }

    uspin_init(&u->usemLock);
    uspin_acquire(&u->usemLock);
    u->counter = c;
    u->front = 0;
    u->back = 0;
    u->guard = 0;
    u->thr_in_queue = 0;
    uspin_release(&u->usemLock);
    return 0;
}

void usem_acquire(struct usem *u) {
    uspin_acquire(&u->usemLock);

    if(u->counter == 0) {
        //no more acquiring allowed
        if(u->thr_in_queue == NTHR) {
            uspin_release(&u->usemLock);
            return;
        }

        //put thread into queue
        u->guard = 1;
        u->queue[u->back] = gettid();
        xchg(&u->back, (u->back + 1) % NTHR);
        xchg(&u->thr_in_queue, u->thr_in_queue + 1);
        uspin_release(&u->usemLock);

        //must desch
        desch(&u->guard);
    }
    else {
        //acquire usem
        xchg(&u->counter, u->counter - 1);
        uspin_release(&u->usemLock);
    }
}

void usem_release(struct usem *u) {
    uspin_acquire(&u->usemLock);
    //check thread queue
    if(u->thr_in_queue > 0)  {
        int tid, res;
        do {
            tid = u->queue[u->front];
            xchg(&u->front, (u->front + 1) % NTHR);
            xchg(&u->thr_in_queue, u->thr_in_queue - 1);
            res = mkrun(tid);
        } while(res < 0 && u->thr_in_queue > 0);

        if (u->thr_in_queue == 0) {
            if (u->guard == 1) {
                u->guard = 0;
            }
            else {
                xchg(&u->counter, u->counter + 1);
            }
        }
        uspin_release(&u->usemLock);
    }
    else {
        if (u->guard == 1) {
            u->guard = 0;
        }
        else {
            xchg(&u->counter, u->counter + 1);
        }
        uspin_release(&u->usemLock);
    }
}

void usem_destroy(struct usem *u) {
    u = 0;
}
