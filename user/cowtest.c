//
// tests for copy-on-write fork.
// each test prints "cowtest: <name>: OK" or "... FAIL".
//

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "user/user.h"

int failed;

void
result(char *name, int ok)
{
  printf("cowtest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

// wait for one child; return 1 if it exited with status 0.
int
childok(void)
{
  int xstatus;

  if (wait(&xstatus) < 0)
    return 0;
  return xstatus == 0;
}

// count free pages by allocating them all with sbrk.
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

// allocate 60% of RAM (more than half of the free memory), fork,
// and let both processes write. without copy-on-write the fork
// fails for lack of memory.
void
bigtest(void)
{
  uint64 sz = (PHYSTOP - KERNBASE) / 10 * 6;
  char *p = sbrk(sz);
  int ok = 1;

  if (p == SBRK_ERROR) {
    printf("cowtest: sbrk(%ld) failed\n", sz);
    result("big", 0);
    return;
  }
  for (char *q = p; q < p + sz; q += PGSIZE)
    *(int *)q = getpid();

  int pid = fork();
  if (pid < 0) {
    printf("cowtest: fork failed\n");
    result("big", 0);
    sbrk(-sz);
    return;
  }
  if (pid == 0) {
    int ppid = *(int *)p;
    // write 100 pages spread over the region.
    for (char *q = p; q < p + sz; q += sz / 100 / PGSIZE * PGSIZE) {
      if (*(int *)q != ppid)
        exit(1);
      *(int *)q = getpid();
    }
    exit(0);
  }
  for (char *q = p; q < p + sz; q += sz / 50 / PGSIZE * PGSIZE)
    *(int *)(q + 8) = 1;
  ok = childok();
  for (char *q = p; q < p + sz; q += PGSIZE)
    if (*(int *)q != getpid())
      ok = 0;
  sbrk(-sz);
  result("big", ok);
}

// pages that were allocated lazily (sbrklazy) and never touched
// are not mapped when fork runs. a store to one must still be a
// lazy allocation, in parent and child, next to shared pages.
void
lazytest(void)
{
  char *p = sbrklazy(8 * PGSIZE);
  int ok = 1;

  p[0] = 'p'; // page 0 mapped before fork, pages 1-7 not
  int pid = fork();
  if (pid < 0) {
    result("lazy", 0);
    return;
  }
  if (pid == 0) {
    if (p[0] != 'p')
      exit(1);
    p[0] = 'c';               // copy-on-write fault
    p[3 * PGSIZE] = 'c';      // lazy-allocation fault
    if (p[PGSIZE * 5] != 0)   // a read fault on a lazy page
      exit(1);
    exit(p[0] == 'c' && p[3 * PGSIZE] == 'c' ? 0 : 1);
  }
  p[3 * PGSIZE] = 'p';
  ok = childok() && p[0] == 'p' && p[3 * PGSIZE] == 'p';
  sbrk(-8 * PGSIZE);
  result("lazy", ok);
}

char buf[3 * PGSIZE];

// the kernel writes into a shared page: read() from a pipe into a
// buffer the child has not touched since fork (copyout's path).
void
copyouttest(void)
{
  int fds[2];
  int ok = 1;
  // six bytes straddling a page boundary inside buf.
  char *dst = (char *)PGROUNDUP((uint64)buf) + PGSIZE - 3;

  memset(buf, 'x', sizeof(buf));
  if (pipe(fds) < 0) {
    result("copyout", 0);
    return;
  }
  int pid = fork();
  if (pid < 0) {
    result("copyout", 0);
    return;
  }
  if (pid == 0) {
    close(fds[1]);
    int n = read(fds[0], dst, 6);
    if (n != 6 || memcmp(dst, "hello!", 6) != 0)
      exit(1);
    exit(0);
  }
  close(fds[0]);
  write(fds[1], "hello!", 6);
  close(fds[1]);
  ok = childok();
  for (int i = 0; i < sizeof(buf); i++)
    if (buf[i] != 'x')
      ok = 0;
  result("copyout", ok);
}

int shared = 1;

// after fork, each process's writes are invisible to the other,
// in both directions.
void
independencetest(void)
{
  int topar[2], tochild[2];
  char c;

  shared = 1;
  if (pipe(topar) < 0 || pipe(tochild) < 0) {
    result("independence", 0);
    return;
  }
  int pid = fork();
  if (pid < 0) {
    result("independence", 0);
    return;
  }
  if (pid == 0) {
    close(topar[0]);
    close(tochild[1]);
    read(tochild[0], &c, 1); // parent has written 2
    if (shared != 1)
      exit(1);
    shared = 3;
    write(topar[1], "x", 1);
    read(tochild[0], &c, 1);
    exit(shared == 3 ? 0 : 1);
  }
  close(topar[1]);
  close(tochild[0]);
  shared = 2;
  write(tochild[1], "x", 1);
  // child has written 3 (read returns 0 if the child exited early)
  int ok = read(topar[0], &c, 1) == 1 && shared == 2;
  write(tochild[1], "x", 1);
  close(topar[0]);
  close(tochild[1]);
  ok = childok() && ok;
  result("independence", ok);
}

// text is read-only, not copy-on-write: a store must still kill.
void
texttest(void)
{
  int xstatus;
  int pid = fork();

  if (pid < 0) {
    result("text", 0);
    return;
  }
  if (pid == 0) {
    *(volatile int *)texttest = 0;
    exit(0);
  }
  wait(&xstatus);
  result("text", xstatus == -1);
}

// many processes sharing and copying pages at once on all harts.
void
forkstest(void)
{
  int ok = 1;

  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < 20; i++) {
      int pid = fork();
      if (pid < 0) {
        ok = 0;
        break;
      }
      if (pid == 0) {
        shared = i;
        int pid2 = fork(); // a grandchild shares the copy
        if (pid2 == 0) {
          exit(shared == i ? 0 : 1);
        }
        int xs = 1;
        wait(&xs);
        exit(xs == 0 && shared == i ? 0 : 1);
      }
    }
    for (int i = 0; i < 20; i++)
      if (!childok())
        ok = 0;
  }
  result("forks", ok);
}

int
main(int argc, char *argv[])
{
  int free0 = countfree();

  bigtest();
  lazytest();
  copyouttest();
  independencetest();
  texttest();
  forkstest();

  int free1 = countfree();
  printf("cowtest: free pages %d before, %d after\n", free0, free1);
  result("no leaks", free1 == free0);

  printf(failed ? "cowtest: SOME TESTS FAILED\n" : "cowtest: ALL OK\n");
  exit(failed);
}
