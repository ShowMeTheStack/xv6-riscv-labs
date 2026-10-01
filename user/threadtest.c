//
// tests for clone, join, futexes and the thread library.
// each test prints "threadtest: <name>: OK" or "... FAIL".
// only the first thread prints; other threads report through
// their exit status or shared memory.
//

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define NT 7 // threads besides the first: the kernel allows 8 in all

int failed;

void
result(char *name, int ok)
{
  printf("threadtest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

// count free pages by allocating them all with sbrk.
// only in a single-threaded process: sbrk(-n) needs that.
int
countfree(void)
{
  char *sz0 = sbrk(0);
  int n = 0;

  while (sbrk(PGSIZE) != SBRK_ERROR)
    n++;
  sbrk(-(sbrk(0) - sz0));
  return n;
}

// a starting gate: threads sleep until gateopen() lets them
// all go at once, so that they run at the same time.
int gate;

void
gatewait(void)
{
  while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) == 0)
    futex_wait(&gate, 0);
}

void
gateopen(void)
{
  __atomic_store_n(&gate, 1, __ATOMIC_RELEASE);
  futex_wake(&gate, NT);
}

// start n threads running fn(i), i = 0..n-1; tids[i] gets
// each id. return 1 if all started.
int
startall(int (*fn)(void *), int n, int *tids)
{
  int ok = 1;

  gate = 0;
  for (int i = 0; i < n; i++) {
    tids[i] = thread_create(fn, (void *)(uint64)i);
    if (tids[i] < 0)
      ok = 0;
  }
  gateopen();
  return ok;
}

// join every thread; return 1 if all exited with status 0.
int
joinall(void)
{
  int st, ok = 1;

  while (thread_join(&st) > 0)
    if (st != 0)
      ok = 0;
  return ok;
}

// create and join: exit statuses come back to the creator.
int
exitfn(void *arg)
{
  gatewait();
  return 100 + (int)(uint64)arg;
}

void
jointest(void)
{
  int tids[NT], seen[NT] = {0}, st, tid, ok;

  ok = startall(exitfn, NT, tids);
  // all 8 trapframe slots are taken now (or soon by zombies).
  if (thread_create(exitfn, 0) >= 0)
    ok = 0;
  for (int n = 0; n < NT; n++) {
    if ((tid = thread_join(&st)) < 0) {
      ok = 0;
      break;
    }
    for (int i = 0; i < NT; i++)
      if (tid == tids[i] && st == 100 + i)
        seen[i]++;
  }
  for (int i = 0; i < NT; i++)
    if (seen[i] != 1)
      ok = 0;
  if (thread_join(&st) != -1) // no threads left
    ok = 0;
  result("create and join", ok);
}

// every thread traps into the kernel thousands of times; its
// registers and system-call results must stay its own.
int
regsfn(void *arg)
{
  int me = getpid();
  volatile uint64 a = (uint64)arg, b = 3 * a, c = 7 * a;

  gatewait();
  for (int i = 0; i < 2000; i++) {
    if (getpid() != me)
      return 1;
    a += 1;
    b += 3;
    c += 7;
  }
  if (a != (uint64)arg + 2000 || b != 3 * (uint64)arg + 6000 ||
      c != 7 * (uint64)arg + 14000)
    return 2;
  return 0;
}

void
regstest(void)
{
  int tids[NT];
  int ok = startall(regsfn, NT, tids);
  result("registers", joinall() && ok);
}

// a shared counter, incremented slowly (read, wait, write) by
// all threads: under the futex mutex, and without it.
#define NINC 20000
struct mutex m;
volatile int counter;

void
slowinc(void)
{
  int c = counter;
  for (volatile int j = 0; j < 200; j++)
    ;
  counter = c + 1;
}

int
countfn(void *arg)
{
  gatewait();
  for (int i = 0; i < NINC; i++) {
    mutex_lock(&m);
    slowinc();
    mutex_unlock(&m);
  }
  return 0;
}

int
racefn(void *arg)
{
  gatewait();
  for (int i = 0; i < NINC; i++)
    slowinc();
  return 0;
}

void
mutextest(void)
{
  int tids[NT], ok;

  counter = 0;
  ok = startall(countfn, NT, tids);
  ok = joinall() && ok;
  printf("threadtest: mutex: %d of %d\n", counter, NT * NINC);
  result("mutex", ok && counter == NT * NINC);

  // the same without the mutex: updates get lost whenever
  // threads really run at once. printed, not checked.
  counter = 0;
  startall(racefn, NT, tids);
  joinall();
  printf("threadtest: without the mutex: %d of %d (not checked)\n", counter,
         NT * NINC);
}

// two threads hand a turn back and forth through a futex,
// each sleeping until the other wakes it: a single lost
// wakeup leaves both asleep for ever.
#define NPING 5000
int turn;

int
pingfn(void *arg)
{
  int me = (int)(uint64)arg;

  for (int i = 0; i < NPING; i++) {
    while (__atomic_load_n(&turn, __ATOMIC_ACQUIRE) != me)
      futex_wait(&turn, 1 - me);
    __atomic_store_n(&turn, 1 - me, __ATOMIC_RELEASE);
    futex_wake(&turn, 1);
  }
  return 0;
}

void
pingtest(void)
{
  int ok = 1;

  turn = 0;
  if (thread_create(pingfn, (void *)0) < 0 ||
      thread_create(pingfn, (void *)1) < 0)
    ok = 0;
  result("ping-pong", joinall() && ok);
}

// sbrk from many threads at once: every call must get its
// own memory.
#define NSBRK 50
char *got[NT][NSBRK];

int
sbrkfn(void *arg)
{
  int me = (int)(uint64)arg;

  gatewait();
  for (int k = 0; k < NSBRK; k++) {
    char *p = sbrk(PGSIZE);
    if (p == SBRK_ERROR)
      return 1;
    memset(p, 'a' + me, PGSIZE);
    got[me][k] = p;
  }
  return 0;
}

void
sbrktest(void)
{
  int tids[NT], ok;
  char *before = sbrk(0);

  ok = startall(sbrkfn, NT, tids);
  ok = joinall() && ok;
  if (sbrk(0) != before + NT * NSBRK * PGSIZE)
    ok = 0;
  for (int i = 0; ok && i < NT; i++)
    for (int k = 0; k < NSBRK; k++)
      for (int j = 0; j < PGSIZE; j++)
        if (got[i][k][j] != 'a' + i) {
          ok = 0;
          break;
        }
  result("sbrk", ok);
}

// all threads touch the same lazily allocated pages at once:
// each page faults on several harts at the same moment.
#define NLAZY 512
char *lazy;

int
lazyfn(void *arg)
{
  int me = (int)(uint64)arg;

  gatewait();
  for (int j = 0; j < NLAZY; j++)
    lazy[j * PGSIZE + me] = 'a' + me;
  return 0;
}

void
lazytest(void)
{
  int tids[NT], ok;

  lazy = sbrklazy(NLAZY * PGSIZE);
  ok = startall(lazyfn, NT, tids);
  ok = joinall() && ok;
  for (int j = 0; j < NLAZY; j++)
    for (int i = 0; i < NT; i++)
      if (lazy[j * PGSIZE + i] != 'a' + i)
        ok = 0;
  result("lazy", ok);
}

// while another thread exists, the address space may not
// shrink or be replaced, and an eager sbrk that runs out of
// memory must give up without mapping anything.
int waitword;

int
sleepfn(void *arg)
{
  while (__atomic_load_n(&waitword, __ATOMIC_ACQUIRE) == 0)
    futex_wait(&waitword, 0);
  return 0;
}

void
sharedtest(void)
{
  char *argv[] = {"echo", "exec", "succeeded", 0};
  int ok = 1;

  waitword = 0;
  if (thread_create(sleepfn, 0) < 0)
    ok = 0;
  char *brk = sbrk(0);
  if (sbrk(1 << 27) != SBRK_ERROR || sbrk(0) != brk) // 128 MiB: too much
    ok = 0;
  if (sbrk(-PGSIZE) != SBRK_ERROR)
    ok = 0;
  if (exec("echo", argv) != -1)
    ok = 0;
  __atomic_store_n(&waitword, 1, __ATOMIC_RELEASE);
  futex_wake(&waitword, 1);
  ok = joinall() && ok;
  // alone again: shrinking works.
  if (sbrk(PGSIZE) == SBRK_ERROR || sbrk(-PGSIZE) == SBRK_ERROR)
    ok = 0;
  result("shared memory only grows", ok);
}

// join() reaps threads, wait() reaps child processes.
int
slowfn(void *arg)
{
  pause(3);
  return 7;
}

void
waitjointest(void)
{
  int pid, tid, st, ok = 1;

  pid = fork();
  if (pid == 0)
    exit(42);
  tid = thread_create(slowfn, 0);
  if (pid < 0 || tid < 0)
    ok = 0;
  if (thread_join(&st) != tid || st != 7)
    ok = 0;
  if (wait(&st) != pid || st != 42)
    ok = 0;
  if (thread_join(&st) != -1 || wait(&st) != -1)
    ok = 0;
  result("wait and join", ok);
}

// the first thread's exit, or a kill, ends every thread.
int
spinfn(void *arg)
{
  for (;;)
    ;
}

int
blockfn(void *arg)
{
  int never = 0;
  for (;;)
    futex_wait(&never, 0);
}

int
freeagain(int free0)
{
  // the killed threads are reaped by init, a moment later.
  for (int i = 0; i < 50; i++) {
    if (countfree() == free0)
      return 1;
    pause(1);
  }
  return 0;
}

void
exittest(void)
{
  int free0 = countfree(), pid, st, ok = 1;

  pid = fork();
  if (pid == 0) {
    thread_create(spinfn, 0);
    thread_create(spinfn, 0);
    thread_create(blockfn, 0);
    pause(2);
    exit(5);
  }
  if (wait(&st) != pid || st != 5)
    ok = 0;
  result("exit ends all threads", ok && freeagain(free0));

  ok = 1;
  pid = fork();
  if (pid == 0) {
    thread_create(spinfn, 0);
    thread_create(blockfn, 0);
    thread_create(blockfn, 0);
    joinall(); // never returns
    exit(0);
  }
  pause(2);
  kill(pid);
  if (wait(&st) != pid || st != -1)
    ok = 0;
  result("kill ends all threads", ok && freeagain(free0));
}

// many rounds of everything at once, on all harts: mutex,
// sbrk, lazy faults, and fork + wait from inside threads.
#define ROUNDS 20

int
stressfn(void *arg)
{
  int me = (int)(uint64)arg, pid, st;

  gatewait();
  for (int i = 0; i < 300; i++) {
    mutex_lock(&m);
    counter++;
    mutex_unlock(&m);
  }
  char *p = sbrklazy(2 * PGSIZE);
  if (p == SBRK_ERROR)
    return 1;
  p[0] = p[PGSIZE] = me;
  pid = fork();
  if (pid == 0)
    exit(me);
  if (pid < 0 || wait(&st) != pid || st != me)
    return 2;
  return 0;
}

void
stresstest(void)
{
  int tids[NT], ok = 1;

  counter = 0;
  for (int r = 0; r < ROUNDS; r++) {
    if (!startall(stressfn, NT, tids))
      ok = 0;
    if (!joinall())
      ok = 0;
  }
  result("stress", ok && counter == ROUNDS * NT * 300);
}

int
main(int argc, char *argv[])
{
  int free0, free1, pid, st;

  free0 = countfree();

  // run the in-process tests in a child, so that the memory
  // they leave allocated is freed before the second count.
  pid = fork();
  if (pid == 0) {
    jointest();
    regstest();
    mutextest();
    pingtest();
    sbrktest();
    lazytest();
    sharedtest();
    waitjointest();
    stresstest();
    exit(failed);
  }
  if (wait(&st) != pid || st != 0)
    failed = 1;
  if (st == -1)
    printf("threadtest: the process running the tests was killed\n");

  exittest();

  if (!freeagain(free0))
    failed = 1;
  free1 = countfree();
  printf("threadtest: free pages %d before, %d after\n", free0, free1);
  result("no leaks", free1 == free0);
  printf("threadtest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
