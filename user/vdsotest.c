// test the read-only pages at USYSCALL and USHARED:
// ugetpid() and uuptime() must agree with the system calls,
// the pages must be read-only, and fork and exec must give
// each process the right pid.

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int failed = 0;

void
check(char *name, int ok)
{
  printf("vdsotest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

// run f in a child; return the child's exit status.
int
inchild(void (*f)(void))
{
  int pid, xstatus;

  pid = fork();
  if (pid < 0) {
    printf("vdsotest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    f();
    exit(0); // f survived
  }
  if (wait(&xstatus) != pid)
    return 99;
  return xstatus;
}

void
pidtest(void)
{
  check("pid", ugetpid() == getpid());
}

void
uptimetest(void)
{
  int a, u, b, t0;

  a = uptime();
  u = uuptime();
  b = uptime();
  check("uptime agrees", a <= u && u <= b);

  // the copy must move on its own: wait for 2 ticks.
  t0 = uuptime();
  pause(2);
  check("uptime advances", uuptime() >= t0 + 2);
}

void
store_usyscall(void)
{
  *(volatile int *)USYSCALL = 1234;
}

void
store_ushared(void)
{
  *(volatile uint *)USHARED = 0;
}

void
readonlytest(void)
{
  // a store from user mode must kill the child (status -1),
  // not succeed (status 0).
  check("usyscall read-only", inchild(store_usyscall) == -1);
  check("ushared read-only", inchild(store_ushared) == -1);
}

void
kerneltest(void)
{
  int fd;

  // system calls may not use the pages either: read() would
  // write them (copyout() needs PTE_W), and write() would read
  // them (copyin() stops at p->sz).
  fd = open("README", 0);
  if (fd < 0) {
    printf("vdsotest: cannot open README\n");
    exit(1);
  }
  check("read() into usyscall fails", read(fd, (char *)USYSCALL, 8) < 0);
  close(fd);
  fd = open("vdsojunk", O_CREATE | O_WRONLY);
  if (fd < 0) {
    printf("vdsotest: cannot create vdsojunk\n");
    exit(1);
  }
  check("write() from ushared fails", write(fd, (char *)USHARED, 8) < 0);
  close(fd);
  unlink("vdsojunk");
  check("pid unchanged", ugetpid() == getpid());
}

void
ceilingtest(void)
{
  char *p;

  // grow the heap lazily as far as it may go: up to MAXHEAP.
  p = sbrklazy(0);
  while ((uint64)p + (1 << 30) <= MAXHEAP)
    p = sbrklazy(1 << 30) + (1 << 30);
  if (sbrklazy(MAXHEAP - (uint64)p) == (char *)-1)
    exit(1);
  if ((uint64)sbrklazy(0) != MAXHEAP)
    exit(2);
  // one more byte would reach USHARED.
  if (sbrklazy(1) != (char *)-1)
    exit(3);
  if (ugetpid() != getpid())
    exit(4);
  // the pages are above p->sz: vmfault() must not "fix"
  // this store by allocating a page.
  store_usyscall();
}

void
forktest(void)
{
  int parent = getpid(), pid, xstatus;

  pid = fork();
  if (pid < 0) {
    printf("vdsotest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    exit(ugetpid() == getpid() && ugetpid() != parent ? 0 : 1);
  }
  wait(&xstatus);
  check("fork", xstatus == 0 && ugetpid() == parent);
}

void
exectest(void)
{
  int pid, xstatus;
  char *argv[] = {"vdsotest", "exec", 0};

  pid = fork();
  if (pid < 0) {
    printf("vdsotest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    exec("vdsotest", argv);
    exit(2);
  }
  wait(&xstatus);
  check("exec", xstatus == 0);
}

void
manytest(void)
{
  int i, j, pid, xstatus, ok = 1;

  // 20 children at once, on all harts, each reading its own page.
  for (i = 0; i < 20; i++) {
    pid = fork();
    if (pid < 0) {
      printf("vdsotest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      pid = getpid();
      for (j = 0; j < 100000; j++)
        if (ugetpid() != pid)
          exit(1);
      exit(0);
    }
  }
  for (i = 0; i < 20; i++) {
    wait(&xstatus);
    if (xstatus != 0)
      ok = 0;
  }
  check("20 children", ok);
}

// try to read the time CSR from user mode.
void
userrdtime(void)
{
  uint64 x;
  asm volatile("rdtime %0" : "=r"(x));
}

// call f for 20 ticks; print the calls made and the cost of one.
// the clock is read with uuptime(), which costs no trap.
void
timeit(char *name, int (*f)(void))
{
  int i, t0;
  uint64 n = 0;

  t0 = uuptime();
  while (uuptime() == t0) // start on a tick boundary
    ;
  t0 = uuptime();
  while (uuptime() - t0 < 20) {
    for (i = 0; i < 1000; i++)
      f();
    n += 1000;
  }
  // a tick is about 0.1 s: 20 ticks are about 2,000,000,000 ns.
  printf("vdsotest: %s: %lu calls in 20 ticks, about %lu ns each\n", name, n,
         2000000000UL / n);
}

void
measure(void)
{
  // the timer CSR would be the best clock, if user mode may read it.
  if (inchild(userrdtime) == -1)
    printf("vdsotest: rdtime in user mode: killed, so time is in ticks\n");
  else
    printf("vdsotest: rdtime in user mode: allowed\n");
  timeit("getpid()", getpid);
  timeit("ugetpid()", ugetpid);
  timeit("uptime()", uptime);
  timeit("uuptime()", uuptime);
}

int
main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "exec") == 0) {
    // after exec: the new page table maps the same pid.
    exit(ugetpid() == getpid() ? 0 : 1);
  }
  if (argc == 2 && strcmp(argv[1], "time") == 0) {
    measure();
    exit(0);
  }

  pidtest();
  uptimetest();
  readonlytest();
  kerneltest();
  check("heap ceiling", inchild(ceilingtest) == -1);
  forktest();
  exectest();
  manytest();

  printf("vdsotest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
