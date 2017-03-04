#include "types.h"
#include "user.h"
#include "uthr.h"
#include "uthrdefs.h"

struct {
    struct uthr all_threads[NTHR];
    uint threadStackSize;
    int numThreads;
    struct usem sema;
} uthr_table;

static struct hand hands[NTHR];

static struct usem malloc_sem;
static struct usem free_sem;

int tid_pool[NTHR];
static struct usem pool_sem;

/*initializes the thread library and sets the
 * stack size of all created threads to
 * stack_size bytes.
 * */

int
uthr_init(uint ss) {
    uthr_table.threadStackSize = ss;
    //when use user sync, initialize here
    int semcheck;
    semcheck = usem_init(&uthr_table.sema, 1);
    if (semcheck < 0) {
        return -1; //semaphore init failed
    }

    semcheck = usem_init(&malloc_sem, 1);
    semcheck = usem_init(&free_sem, 1);
    semcheck = usem_init(&pool_sem, 1);

    usem_acquire(&uthr_table.sema);
    int i;
    usem_acquire(&pool_sem);
    for (i = 0; i < NTHR; ++i) {
        tid_pool[i] = -1;
        semcheck = usem_init(&hands[i].sema, 0);
    }
    tid_pool[0] = gettid();
    usem_release(&pool_sem);

    uthr_table.numThreads++;
    uthr_table.all_threads[0].tid = 0;
    uthr_table.all_threads[0].state = RUNNABLE;

    usem_release(&uthr_table.sema);
    return 0;
}

/*creates a new thread, with its own stack,
 * running the code f(arg). Returns to the
 * parent the thread identifier of this thread,
 * or a negative number on error (in which
 * case, no thread was created).
 * */
//int tspawn(void *stktop, void (*f)(void *), void *a)

int
uthr_create(void (*f)(void *), void *arg) {

    int i;

    usem_acquire(&uthr_table.sema);

    if (uthr_table.numThreads == NTHR) {
        usem_release(&uthr_table.sema);
        return -1;
    }

    for (i = 0; i < NTHR; i++) {
        if (uthr_table.all_threads[i].state == UNUSED)
            break; //found an opening in all_threads
    }

    if (i == NTHR) {
        return -1; // no more room
    }

    //then use uthr_malloc with set stacksize
    //to allocate correct size of this thread.

    char* start = uthr_malloc((int)uthr_table.threadStackSize);

    int tid = tspawn(start + (int)uthr_table.threadStackSize, *f, (void*)arg);

    if (tid < 0) {
        //tspawn failed
        uthr_free(start);
        usem_release(&uthr_table.sema);
        return -1;
    }

    int j;

    usem_acquire(&pool_sem);
    for (j = 0; j < NTHR; ++j) {
        if (tid_pool[j] < 0) {
            tid_pool[j] = tid;
            break;
        }
    }
    usem_release(&pool_sem);

    uthr_table.numThreads++;
    uthr_table.all_threads[i].tid = j;
    uthr_table.all_threads[i].state = RUNNABLE;
    uthr_table.all_threads[i].stack = start + (int)uthr_table.threadStackSize; // stack grows from high address to low address
    usem_release(&uthr_table.sema);
    //put this thread into all_threads

    return tid;
}

/* ends the execution of the current thread,
 * arranging for the baton to be passed to
 * whichever thread calls... JOIN
 * */

void
uthr_exit(void *baton) {
    int tid = gettid();
    int i, idx;
    int what = 123;
    uint where;
    
    //acquire uthread table semaphore
    //to find exiting thread's index
    //and set it to unused there.
    usem_acquire(&uthr_table.sema);
    usem_acquire(&pool_sem);
    for (i = 0; i < NTHR; ++i) {
        if (uthr_table.all_threads[i].state == UNUSED) {
            continue;
        }

        if (tid_pool[uthr_table.all_threads[i].tid] == tid) {
            uthr_table.all_threads[i].state = ZOMBIE;
            uthr_table.numThreads--;
            idx = uthr_table.all_threads[i].tid;
            hands[idx].hand = baton;
            break;
        }
    }
    
    usem_release(&pool_sem);
    usem_release(&uthr_table.sema);
    
    usem_release(&hands[idx].sema);
    texit(&where, what);
}

/* which waits for the thread identified by
 * tid to call uthr_exit(baton) and then arranges
 * for baton to be placed in *hand.
 * */

int
uthr_join(int tid, void **hand) {
    if (tid == gettid()) {
        return -1;
    }

    if (hand == 0) {
        return -1;
    }

    int i, j;
    do {
        usem_acquire(&pool_sem);
        for (i = 0; i < NTHR; i++) {
            if (tid_pool[i] == tid) {
                break;
            }
        }
        usem_release(&pool_sem);
    } while(i == NTHR);

    usem_acquire(&hands[i].sema);

    *hand = hands[i].hand;

    //free stack
    usem_acquire(&uthr_table.sema);
    for (j = 0; j < NTHR; ++j) {
        if (uthr_table.all_threads[j].tid == i) {
            break;
        }
    }
    if (uthr_table.all_threads[j].state == ZOMBIE){
        uthr_table.all_threads[j].state = UNUSED;
        if (j != 0) {
            uthr_free(uthr_table.all_threads[j].stack - uthr_table.threadStackSize);
        }
    }
    usem_release(&uthr_table.sema);

    usem_acquire(&pool_sem);
    tid_pool[i] = -1;
    usem_release(&pool_sem);

//    if (j == NTHR && uthr_table.all_threads[j].state != UNUSED) {
//        return -1; // we never joined anything
//    }

    return 1;
}

void *
uthr_malloc(int sz) {
    void* mem;
    // for thread safe
    usem_acquire(&malloc_sem);
    mem = malloc(sz);
    usem_release(&malloc_sem);
    return mem;
}

void
uthr_free(void *p) {
    // for thread safe
    usem_acquire(&free_sem);
    free(p);
    usem_release(&free_sem);
}
