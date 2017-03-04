//Test nice and scheduler

#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

#define TEST_NAME "schedulertest"
#include "318_test-tapish.h"


void
test(void) {
    int pid;
    pid = fork();
    if (pid == 0) {
        //in child
        printf(1, "in child process ");
        exit();
    } else {
        wait();
        printf(1, "In Parent Process ");
//        kill(pid);
    }
}

void
nicetest(void) {
    int pid;
    pid = fork();
    if (pid == 0) {
        int result = nice(1);
        if (result < 0) {
            //bad
            exit();
        }
        printf(1, "in child process 2");
        exit();
    } else {
        printf(1, "In Parent Process 2");
        wait();
//        kill(pid);
    }
}

void nicetest2(void) {
    int pid;
    pid = fork();
    if (pid == 0) {
        int result = nice(-1);
        if (result < 0) {
            //bad
            exit();
        }
        printf(1, "in child process 3");
        exit();
    } else {
        int result = nice(-2);
        if (result < 0) {
            //bad
            exit();
        }
        printf(1, "In Parent Process 3");
        wait();
//        kill(pid);
    }
}


int
main(int argc, char *argv[])
{
  printf(1, "schedulertest starting\n");

  test();
  printf(1, "\n");
  nicetest();
  printf(1, "\n");
  nicetest2();

  exit();
}
