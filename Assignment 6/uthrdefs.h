//OS Assignment 6
#define NTHR 256
struct uspin {
  uint locked;       // Is the lock held?
  // For debugging:
  uint tid;   //The process holding the lock.
};

struct usem {
  uint counter; //the number of things that can get at this semaphore
  int queue[NTHR];
  uint front;
  uint back;
  uint thr_in_queue;
  int guard;
  struct uspin usemLock; 
};

struct uthr {
  char *stack;                // Bottom of stack for this thread
  int tid;                     // Thread ID
  int state;        // Thread state 	    
};

struct hand {
  void *hand;
  struct usem sema;
};

