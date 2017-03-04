#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "memlayout.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct {
  struct spinlock lock;
  struct shmem shmem[NSHM];
} shmem_table;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

uint hash(int key) {
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key % NSHM;
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&shmem_table.lock, "shmem_table");
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

  // added:
  p->cnt_shmem = 0;

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
void
scheduler(void)
{
  struct proc *p;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable.lock);

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

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
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
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
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

// Should we initialize the shmem table somewhere?
int shmmk(int id, int size) {
  // we allocate a page each time => size % PGSIZE == 0
  // how to check: there is not enough memory to create the shared memory region?
  // my idea: keep allocating until we fail, then free all pages we have and return -1 ?
  if(size <= 0) {
    return -1;
  }
 
  size = size % PGSIZE == 0 ? size : (size/PGSIZE + 1)*PGSIZE;
 
  // require memory region exceeds our limit
  if(size / PGSIZE > NSHMPGS) {
    return -1;
  }
 
  id = hash(id);
  acquire(&shmem_table.lock);
 
  if(shmem_table.shmem[id].ref_cnt > 0) {
    release(&shmem_table.lock);
    return -1;
  }
 
  shmem_table.shmem[id].ref_cnt = 1;
  size /= PGSIZE;
  int i;
 
  // Try to allocate each page:
  for(i = 0; i < size; ++i) {
    char *mem;
    mem = kalloc();
 
    // If calling kalloc() fails, free all memory we have allocated
    if(mem == 0) {
      kfree(mem);
      --i;
      while(i >= 0) {
        kfree(shmem_table.shmem[id].shared_pages[i]);
        --i;
      }
 
      shmem_table.shmem[id].ref_cnt = 0;
      release(&shmem_table.lock);
      return -1;
    }

    memset(mem, 0, PGSIZE);
    shmem_table.shmem[id].shared_pages[i] = mem;
  }
 
  shmem_table.shmem[id].owner = proc->pid;
  shmem_table.shmem[id].region_size = size * PGSIZE;
  //convert the virtual address to the general physical address
  for(i = 0; i < size; i++){
    shmem_table.shmem[id].shared_pages[i] = V2P(shmem_table.shmem[id].shared_pages[i]);
    //cprintf("addr: %d\n", (uint)shmem_table.shmem[id].shared_pages[i]);
  }
  // default value is 0 so that we don't need to initialize the shmem table
  shmem_table.shmem[id].not_published = 1;
  release(&shmem_table.lock);
  return 0;
}
 
// Added
int shmpub(int id) {
  id = hash(id);
 
  acquire(&shmem_table.lock);
  // if shared memory region doesn't exist:
  if(shmem_table.shmem[id].ref_cnt == 0) {
    release(&shmem_table.lock);
    return -1;
  }
 
  // if shared memory region already been published:
  if(!shmem_table.shmem[id].not_published) {
    release(&shmem_table.lock);
    return -1;
  }
 
  int pid = proc->pid;
  // if owner not matched:
  if(shmem_table.shmem[id].owner != pid) {
    release(&shmem_table.lock);
    return -1;
  }
 
  // published
  shmem_table.shmem[id].not_published = 0;
  release(&shmem_table.lock);
  return 0;
}
 
// Added
// haven't considered: there are too many shared memory regions attached to the calling process
void *shmat(int id) {
  id = hash(id);
  acquire(&shmem_table.lock);
 
  // try to attach an unpublished region
  // exception: current process is the creating process
  if(shmem_table.shmem[id].not_published && shmem_table.shmem[id].owner != proc->pid) {
    release(&shmem_table.lock);
    return -1;
  }
 
  int i;
  if(shmem_table.shmem[id].map_proc_cnt > 0) {
    for(i = 0; i < shmem_table.shmem[id].map_proc_cnt; ++i) {
      // already mapped to this region
      if(shmem_table.shmem[id].map_proc[i] == proc->pid) {
        release(&shmem_table.lock);
        return 0;
      }
    }
  }
 
  // Not sure whether I use PGROUNDUP correctly or not
  uint addr = PGROUNDUP(proc->sz);
  uint start_addr = addr;
  for(i = 0; addr < start_addr + shmem_table.shmem[id].region_size; addr += PGSIZE, ++i) {
    //cprintf("addr: %d\n", (uint)addr);
    if(mappages(proc->pgdir, (char*)addr, PGSIZE, shmem_table.shmem[id].shared_pages[i], PTE_W|PTE_U) < 0){
      // if mappages fails, what should we do???
      // current solution: code from deallocuvm()
      pte_t *pte;
      uint pa;
      for(; start_addr < addr; start_addr += PGSIZE) {
        pte = walkpgdir(proc->pgdir, (char*)start_addr, 0);
        if(!pte) {
          start_addr += (NPTENTRIES - 1) * PGSIZE;
        }
        else if((*pte & PTE_P) != 0) {
          pa = PTE_ADDR(*pte);
          if(pa == 0)
            panic("shmat");
          *pte = 0;
        }
      }
      release(&shmem_table.lock);
      return -1;
    }
  }
 
  shmem_table.shmem[id].map_proc[shmem_table.shmem[id].map_proc_cnt] = proc->pid;
  ++shmem_table.shmem[id].map_proc_cnt;

  proc->shmemStartAddr[proc->cnt_shmem] = start_addr;
  proc->shmemID[proc->cnt_shmem] = id;
  ++proc->cnt_shmem;

  release(&shmem_table.lock);
  return start_addr;
}
 
int shmdt(void *shm) {
  acquire(&shmem_table.lock);
  pte_t *pte;
  int shmID = -1;
  // how to get the size of region?
  // in proc save the identifiers of all of its shared regions
  int i;
  for (i = 0; i < proc->cnt_shmem; i++) {
    if ((char*)shm == proc->shmemStartAddr[i]){
        shmID = proc->shmemID[i];
        break;
    }
  }

  if (shmID < 0) {
      release(&shmem_table.lock);
      return -1;
  }
 
  // we also need to check whether we can truly detach this memory region
  // if the number of processes attached to a region becomes zero, when and where should we free it?
  int size = shmem_table.shmem[shmID].region_size;
  uint a = (uint)shm;
  uint pa;
  for(; a < size + (uint)shm; a += PGSIZE) {
    pte = walkpgdir(proc->pgdir, (char*)a, 0);
    if(!pte) {
      a += (NPTENTRIES - 1) * PGSIZE;
    }
    else if((*pte & PTE_P) != 0) {
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("shmat");
      *pte = 0;
    }
  }

  // !!! we should also remove the mapping between current process and the shared memory region
  shmem_table.shmem[shmID].ref_cnt--;
  if (shmem_table.shmem[shmID].ref_cnt == 0) {
    //no procs pointing to that shared mem anymore
    //garbage collect it up. kfree every page of it
    int j;
    a = 0;
    for(j = 0; a < shmem_table.shmem[shmID].region_size; a += PGSIZE, ++j) {
      kfree(P2V(shmem_table.shmem[shmID].shared_pages[j]));
    }
  }
   
  release(&shmem_table.lock);
  return 0;
}
