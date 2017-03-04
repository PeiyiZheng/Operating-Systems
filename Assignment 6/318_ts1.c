#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"
#include "traps.h"

#define TEST_NAME "318_ts1"
#include "318_test-tapish.h"

 /* Invalid address; cause pagefault if texit does nothing */
#define BAILOUT (*(int *)0x10000) = 3;
#define ARG ((void *)0x15410DE0U)

static void
test(void *a) {
  int pid = getpid();
  int tid = gettid();
  uint where;
  TEST_EXIT_IF(a != ARG, "arg");
  yield(-1);
  yield(-1);
  yield(-1);
  sleep(5);
  TEST_DIAG("pid=%d tid=%d", pid, tid);
  TEST_EXIT_IF(pid == tid, "tid pid match");
  TEST_FINI("spawned");
  texit(&where, 7);
  TEST_EXIT("spawned texit didn't");
  BAILOUT;
}

int main() {
  uint where;
  TEST_STRT(2);
  char tstk[1024];  /* Wheels in wheels, stacks in stacks... Stackception! */

  printf(2, "%d\n", sizeof(tstk));
  TEST_EXIT_IF(tspawn(tstk+sizeof(tstk), test, ARG) < 0, "tspawn");
  TEST_FINI("main");
  texit(&where, 3);
  TEST_EXIT("main texit didn't");
  BAILOUT;
}
