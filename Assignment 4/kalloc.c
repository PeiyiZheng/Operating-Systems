// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;

  // Added: reference count
  uint ref_cnt[PHYSTOP >> PGSHIFT];
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  // Added condition: (uint)v < PGSIZE
  if((uint)v % PGSIZE || (uint)v < PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;

  // Added: If we want to free a page, decrease the reference count first
  if(kmem.ref_cnt[V2P(v) >> PGSHIFT] > 0) {
    kmem.ref_cnt[V2P(v) >> PGSHIFT] -= 1;
  }

  // Added: If no reference to a page, then we free it
  if(kmem.ref_cnt[V2P(v) >> PGSHIFT] == 0) {
    // Added: Move position, we don't want to memset the page before
    // we make sure that there is no reference to the page
    // Fill with junk to catch dangling refs.
    memset(v, 1, PGSIZE);
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    kmem.freelist = r->next;

    //Added: when we allocate a page, its initial reference count is 0
    kmem.ref_cnt[V2P((char*)r) >> PGSHIFT] = 1;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

// Added: helper function
void increase_ref_cnt(uint pa) {
  // void function has no return value so we can only use panic to check
  // whether anything goes wrong
  if(pa >= PHYSTOP || pa < (uint)V2P(end)) {
    // invalid pages 
    panic("Bad physical address");
  }

  acquire(&kmem.lock);
  kmem.ref_cnt[pa >> PGSHIFT] += 1;
  release(&kmem.lock);
}

// Added: helper function
void decrease_ref_cnt(uint pa) {
  if(pa >= PHYSTOP || pa < (uint)V2P(end)) {
    // invalid pages 
    panic("Bad physical address");
  }

  acquire(&kmem.lock);
  if(kmem.ref_cnt[pa >> PGSHIFT] > 0) {
    kmem.ref_cnt[pa >> PGSHIFT] -= 1;
  }
  release(&kmem.lock);
}

// Added: helper function
int get_ref_cnt(uint pa) {
  if(pa >= PHYSTOP || pa < (uint)V2P(end)) {
    // invalid pages 
    panic("Bad physical address");
  }

  uint temp;
  acquire(&kmem.lock);
  temp = kmem.ref_cnt[pa >> PGSHIFT];
  release(&kmem.lock);

  return temp;
}