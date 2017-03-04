//shared memory stuff

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "shmem.h"
#include "spinlock.h"
#include "mmu.h"
#include "memlayout.h"

struct {
  struct spinlock lock;
  struct shmem shmem[NSHM];
} shmem_table;

extern int mappages(pde_t *, void *, uint , uint , int );
extern pte_t * walkpgdir(pde_t *, const void *, int );

void
shmeminit(void)
{
    initlock(&shmem_table.lock, "shmemtable");
}

//3. Add hash function(the one I used in assignment 3) into proc.c in order to map given identifier to range [0, NSHM)
uint hash(uint key) {
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key % NSHM;
}

//4. Add the following functions to proc.c:
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

    shmem_table.shmem[id].shared_pages[i] = mem;
  }

  shmem_table.shmem[id].owner = getpid();
  shmem_table.shmem[id].region_size = size * PGSIZE;
  //convert the virtual address to the general physical address
  for (i = 0; i < size; i++){
    shmem_table.shmem[id].shared_pages[i] = V2P(shmem_table.shmem[id].shared_pages[i]);
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

  int pid = getpid();
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
  if(shmem_table.shmem[id].not_published && shmem_table.shmem[id].owner != getpid()) {
    release(&shmem_table.lock);
    return 0;
  }

  int i;
  if(shmem_table.shmem[id].map_proc_cnt > 0) {
    for(i = 0; i < shmem_table.shmem[id].map_proc_cnt; ++i) {
      // already mapped to this region
      if(shmem_table.shmem[id].map_proc[i] == getpid()) {
        release(&shmem_table.lock);
        return 0;
      }
    }
  }

  // Not sure whether I use PGROUNDUP correctly or not
  uint addr = PGROUNDUP(proc->sz);
  uint start_addr = addr;
  for(i = 0; addr < shmem_table.shmem[id].region_size; addr += PGSIZE, ++i) {
    if(mappages(proc->pgdir, (void*)addr, PGSIZE, (uint)shmem_table.shmem[id].shared_pages[i], PTE_W|PTE_U) < 0){
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
      return 0;
    }
  }

  shmem_table.shmem[id].map_proc[shmem_table.shmem[id].map_proc_cnt] = getpid();
  ++shmem_table.shmem[id].map_proc_cnt;
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
  for(; a < a+size; a += PGSIZE) {
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
