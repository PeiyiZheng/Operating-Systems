#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];

  int ctp_len[NPROC]; // numbers of proc pointers in array
  struct proc* chan_to_proc[NPROC][NPROC]; // mapping channel number to array of proc pointers
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
// Must hold ptable.lock.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = 0; //assignment 3
  p->allowedRuntime = 2;

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  acquire(&ptable.lock);

  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;

  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  acquire(&ptable.lock);

  // Allocate process.
  if((np = allocproc()) == 0){
    release(&ptable.lock);
    return -1;
  }

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    release(&ptable.lock);
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;
  np->priority = 0; //assignment 3
  np->allowedRuntime = 2;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  safestrcpy(np->name, proc->name, sizeof(proc->name));
  release(&ptable.lock);

  // It is safe to release ptable.lock here and yet reference both
  // np-> and proc-> objects because np-> is in state EMBRYO and
  // therefore not moving until we say so, and proc-> is us, so,
  // again, not doing anything with its ofile[]s or its cwd until
  // we let it.
  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  // This acquire-set-release dance forces the compiler and the CPU
  // to do and publish all of the above changes before this write
  acquire(&ptable.lock);

  pid = np->pid;

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

// Assignment 3 priorities to time
static int priorities[5] = {-2, -1, 0, 1, 2};
static int allocatedTimes[5] = {8, 4, 2, 1, 1};
static int offset = 2;

void
scheduler(void)
{
  struct proc *p;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    //assignment 3
    int pri; //priority
    int index; //index in arrays


    for (pri = priorities[0]; pri <= priorities[4]; pri++) {
        acquire(&ptable.lock);
//        int allowedRunTime = allocatedTimes[index];

        // Loop over process table looking for process to run.

        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
            if(p->state != RUNNABLE)
                continue;

          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
            if (p->priority <= pri) {
                if (p->allowedRuntime <= 0) {
                    if (p->priority < 2) {
                        p->priority = p->priority+1;
                        index = p->priority+offset;
                        p->allowedRuntime = allocatedTimes[index];
                    } else {
                        p->allowedRuntime = 1;
                    }
                    continue;
                    // break out of the most inner for loop (the ptable loop)
                    //go to next iteration of the outer for loop, to next priority
                }
                //once time is up for this current priority, continue on with next iteration of the outer loop
                proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&cpu->scheduler, p->context);
                switchkvm();
                p->allowedRuntime--;
            }

            // Process is done running for now.
            // It should have changed its p->state before coming back.
            proc = 0;
        }
        release(&ptable.lock);
    }

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// mapping address to integer in [0, NPROC)
uint hash(uint key) {
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key % NPROC;
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
//void
//sleep(void *chan, struct spinlock *lk)
//{
//  if(proc == 0)
//    panic("sleep");

//  if(lk == 0)
//    panic("sleep without lk");

//  // Must acquire ptable.lock in order to
//  // change p->state and then call sched.
//  // Once we hold ptable.lock, we can be
//  // guaranteed that we won't miss any wakeup
//  // (wakeup runs with ptable.lock locked),
//  // so it's okay to release lk.
//  if(lk != &ptable.lock){  //DOC: sleeplock0
//    acquire(&ptable.lock);  //DOC: sleeplock1
//    release(lk);
//  }

//  // Go to sleep.
//  proc->chan = chan;
//  proc->state = SLEEPING;
//  sched();

//  // Tidy up.
//  proc->chan = 0;

//  // Reacquire original lock.
//  if(lk != &ptable.lock){  //DOC: sleeplock2
//    release(&ptable.lock);
//    acquire(lk);
//  }
//}

void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;

  uint idx = hash((uint)chan); // map the channel number to an potential index

  // if the array is no empty and the first process in the array has different channel --> hash conflict
  // then keep moving to the next possible position until we find one
  // otherwise if the corresponding array is empty or the channel is matched, it is the position we want
  while (ptable.ctp_len[idx] > 0 && ptable.chan_to_proc[idx][0]->chan != proc->chan) {
    idx = (idx + 1) % NPROC;
  }

  // add the new process pointer to the end
  ptable.chan_to_proc[idx][ptable.ctp_len[idx]++] = proc;

  sched();

  // Tidy up.
  proc->chan = 0;
  // the length of the array becomes 0 since all processes have been waken up
//  ptable.ctp_len[idx] = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
//static void
//wakeup1(void *chan)
//{
//  struct proc *p;

//  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
//    if(p->state == SLEEPING && p->chan == chan)
//      p->state = RUNNABLE;
//}

static void
wakeup1(void *chan)
{
  // cnt is the number of processes we have checked
  // cnt must less than NPROC to avoid such a case, there is no corresponding channel and the program keep looping
  uint idx = hash((uint)chan), cnt = 0;

  // keep looping when:
  // 1. not all processes have been checked
  // 2. it is an empty array
  // 3. channel number doesn't match
  while (cnt < NPROC && (ptable.ctp_len[idx] == 0 || ptable.chan_to_proc[idx][0]->chan != chan)) {
    idx = (idx + 1) % NPROC;
    ++cnt;
  }

  // if we successfully find out the array matching the channel number
  if (cnt < NPROC) {
    int i;

    // set the state of all processes in this array to RUNNABLE
    for (i = 0; i < ptable.ctp_len[idx]; ++i) {
        if (ptable.chan_to_proc[idx][i]->state == SLEEPING) {
            ptable.chan_to_proc[idx][i]->state = RUNNABLE;
        }
    }

    // the length of the array becomes 0 since all processes have been waken up
    ptable.ctp_len[idx] = 0;
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    [UNUSED]   = "unused",
    [EMBRYO]   = "embryo",
    [SLEEPING] = "sleep ",
    [RUNNABLE] = "runble",
    [RUNNING]  = "run   ",
    [ZOMBIE]   = "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, NELEM(pc), pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

//OS Assignment 3
//NICE

// int nice(int increment); // nice! increment! it's all so very positive...

int
nice(int increment) {

    proc->priority = (proc->priority+increment);

    //In case increment goes past priority bounds
    if (proc->priority > 2) {
        proc->priority = 2;
    }
    if (proc->priority < -2) {
        proc->priority = -2;
    }

    struct proc *p;

    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        //for every process in the ptable
        //if we found the right process
        if ((p->pid == proc->pid)) {
            p->priority = proc->priority;
            p->allowedRuntime = allocatedTimes[proc->priority + offset];
            release(&ptable.lock);
            return 0; //success
        }
    }
    release(&ptable.lock);
    return -1; // failure
}
