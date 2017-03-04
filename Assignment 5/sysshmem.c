//
// Shared memory system calls.
// Mostly argument checking, since we don't trust
// user code
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "shmem.h"
#include "fcntl.h"

int
sys_shmmk(void)
{
    int shmmkid, shmmksz;
    //requires int id and int size.
    if (argint(0, &shmmkid) < 0 || argint(1, &shmmksz) < 0)
        return -1;
    return shmmk(shmmkid, shmmksz);
}

int
sys_shmpub(void)
{
    int shmmkid;
    if (argint(0, &shmmkid) < 0)
        return -1;
    return shmpub(shmmkid);
}

int
sys_shmat(void)
{
    int shmmkid;
    uint shmmkaddr;
    if (argint(0, &shmmkid) < 0)
        return -1;
    shmmkaddr = shmat(shmmkid);
    return shmmkaddr;
    //can return the address as an (u)int?
}

int
sys_shmdt(void)
{
//    void *p; //needs to be void* though?
    int shm;
//    if (argptr(0, (char**)&p, n) < 0)
    if (argint(0, &shm) < 0)
        return -1;
    return shmd((void*)shm);//??
}
