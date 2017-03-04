#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"
#include "traps.h"
#include "uthr.h"
#include "uthrdefs.h"

#define TEST_NAME "318_usem_ring"
#include "318_test-tapish.h"

#define N 4
#define ROUNDS 1024

int rc = ROUNDS;
struct usem ring[N];

static void
test(void *_a) {
  struct usem *a = _a;
  char x = 'A' + (a - ring);
  struct usem *b = a+1;
  int i;

  if(b == &ring[N]) { b = &ring[0]; }

  usem_acquire(a);
  printf(1, "tid %d reporting in with a=%p b=%p\n", gettid(), a, b);
  usem_release(b);

  while(rc-- > 0) {usem_acquire(a); write(1, &x, 1); usem_release(b); }

  printf(1, "\n");

  usem_release(a);

  for(i = 0; i < N; i++)
    usem_acquire(&ring[i]);
  TEST_FINI("tid %d", gettid());
  for(i = 0; i < N; i++)
    usem_release(&ring[i]);
  texit((uint*)&i, 31337);
}

#define INITER(ss) uthr_init(ss)
#define CREATOR(s,f,a) uthr_create(f,a)

// #define INITER(ss) do {;} while(0)
// #define CREATOR(s,f,a) tspawn(s,f,a)

char tstk[N-1][1024];

int main() {
  TEST_STRT(N);

  sleep(1); // grab in debugger

  TEST_FAIL_IF(usem_init(&ring[0], 1) < 0, "ring 0");
  TEST_FAIL_IF(usem_init(&ring[1], 0) < 0, "ring 1");
  TEST_FAIL_IF(usem_init(&ring[2], 0) < 0, "ring 2");
  TEST_FAIL_IF(usem_init(&ring[3], 0) < 0, "ring 3");

  INITER(1024);

  TEST_EXIT_IF(CREATOR(tstk[0]+sizeof(tstk[0]), test, &ring[0]) < 0, "tspawn 1");
  TEST_EXIT_IF(CREATOR(tstk[1]+sizeof(tstk[0]), test, &ring[1]) < 0, "tspawn 2");
  TEST_EXIT_IF(CREATOR(tstk[2]+sizeof(tstk[0]), test, &ring[2]) < 0, "tspawn 3");
  test(&ring[3]);

  // NOTREACHED
  return 0;
}
