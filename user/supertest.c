//
// tests for superpages.
// each test prints "supertest: <name>: OK" or "... FAIL".
//

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define MB    (1024 * 1024)
#define BATCH (64 * PGSIZE) // fragment: 64 pages per turn

int failed;
char *base;      // 4 megabytes of heap, 2-megabyte aligned
char *heap0;     // the break before the test grew the heap

void
result(char *name, int ok)
{
  printf("supertest: %s: %s\n", name, ok ? "OK" : "FAIL");
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

// count free pages by allocating them all with sbrk, in a child:
// the page-table pages that the count needs are freed when it
// exits, so they cannot pin 2-megabyte chunks.
int
countfree(void)
{
  int n = 0;
  int pid = fork();

  if (pid < 0)
    return -1;
  if (pid == 0) {
    while (sbrk(PGSIZE) != SBRK_ERROR)
      n++;
    exit(n);
  }
  wait(&n);
  return n;
}

// the byte that offset i of the region should hold.
char
pattern(uint64 i)
{
  return (i * 131 + (i >> 12)) & 0xff;
}

// do bytes [from, to) of the region hold the pattern?
int
checkpattern(uint64 from, uint64 to)
{
  for (uint64 i = from; i < to; i++)
    if (base[i] != pattern(i))
      return 0;
  return 1;
}

// grow the heap to a 2-megabyte boundary, then by 4 megabytes:
// two superpages.
void
supertest(void)
{
  int free0;
  char *pad;

  heap0 = sbrk(0);
  pad = sbrk(SUPERPGROUNDUP((uint64)heap0) - (uint64)heap0);
  free0 = freesuper();
  base = sbrk(4 * MB);
  if (pad == SBRK_ERROR || base == SBRK_ERROR) {
    printf("supertest: sbrk failed\n");
    exit(1);
  }
  printf("supertest: heap at %p, superpages at %p and %p\n", heap0, base,
         base + 2 * MB);
  result("superpages",
         pglevel(base) == 1 && pglevel(base + 2 * MB) == 1 &&
           pglevel(base + 4 * MB - 1) == 1 && pglevel(heap0) == 0 &&
           freesuper() == free0 - 2);
}

// read and write every byte of the two superpages.
void
bytetest(void)
{
  int ok = 1;

  for (uint64 i = 0; i < 4 * MB; i++)
    if (base[i] != 0)
      ok = 0;
  for (uint64 i = 0; i < 4 * MB; i++)
    base[i] = pattern(i);
  result("every byte", ok && checkpattern(0, 4 * MB));
}

// the kernel reads and writes superpages too (copyin, copyout):
// send 400 bytes that straddle a page boundary in the second
// superpage through a pipe, and read them back into the first
// superpage, across a page boundary there too.
void
syscalltest(void)
{
  uint64 from = 2 * MB + 3 * PGSIZE - 200;
  uint64 to = 1 * MB + 5 * PGSIZE - 100;
  int fds[2], ok;

  if (pipe(fds) < 0) {
    result("system calls", 0);
    return;
  }
  ok = write(fds[1], base + from, 400) == 400 &&
       read(fds[0], base + to, 400) == 400;
  for (int i = 0; i < 400; i++)
    if (base[to + i] != pattern(from + i))
      ok = 0;
  for (uint64 i = to; i < to + 400; i++) // put the pattern back
    base[i] = pattern(i);
  close(fds[0]);
  close(fds[1]);
  result("system calls", ok);
}

// fork: the child must see the same bytes in pages of the given
// level, and its writes must not reach the parent.
int
forkcheck(int level)
{
  int pid = fork();

  if (pid < 0) {
    printf("supertest: fork failed\n");
    return 0;
  }
  if (pid == 0) {
    if (pglevel(base) != level || pglevel(base + 2 * MB) != level)
      exit(1);
    if (!checkpattern(0, 4 * MB))
      exit(2);
    for (uint64 i = 0; i < 4 * MB; i += 997)
      base[i] = ~pattern(i);
    exit(0);
  }
  int ok = childok();
  return ok && checkpattern(0, 4 * MB);
}

// fill memory so that every 2-megabyte chunk holds pages of two
// processes, then let one of them exit: half of memory is free,
// but not one superpage. Returns the pid of the one left (it
// exits when its pipe closes), or -1.
int
fragment(int *keep)
{
  int cmd[2][2], ack[2], pid[2];
  char c;

  if (pipe(ack) < 0)
    return -1;
  for (int k = 0; k < 2; k++) {
    if (pipe(cmd[k]) < 0)
      return -1;
    if ((pid[k] = fork()) == 0) {
      sbrk(-(sbrk(0) - heap0)); // give back the inherited superpages
      close(cmd[k][1]);
      while (read(cmd[k][0], &c, 1) == 1) {
        if (c == 'q')
          exit(0);
        c = sbrk(BATCH) == SBRK_ERROR ? 'n' : 'y';
        write(ack[1], &c, 1);
      }
      exit(0);
    }
    close(cmd[k][0]);
  }
  // take turns until no superpage is left.
  for (int k = 0; freesuper() > 0; k = 1 - k) {
    write(cmd[k][1], "a", 1);
    if (read(ack[0], &c, 1) != 1 || c != 'y')
      break;
  }
  write(cmd[1][1], "q", 1);
  wait(0);
  close(cmd[1][1]);
  close(ack[0]);
  close(ack[1]);
  *keep = cmd[0][1];
  return pid[0];
}

// with no superpage free, fork must still copy a superpage, into
// 512 pages.
void
demotefork(void)
{
  int keep, pid;

  if ((pid = fragment(&keep)) < 0) {
    result("fork without free superpages", 0);
    return;
  }
  int n = freesuper();
  int npages = countfree();
  printf("supertest: fragmented: %d free superpages, %d free pages\n", n,
         npages);
  int ok = forkcheck(0);
  close(keep); // the other process exits
  wait(0);
  result("fork without free superpages", n == 0 && ok);
}

// shrink into the middle of the second superpage: it must be split,
// and the bytes below the new break kept.
void
shrinktest(void)
{
  uint64 cut = 3 * MB - 2 * PGSIZE; // new end, inside superpage 2
  int ok;

  if (sbrk(-(4 * MB - cut)) == SBRK_ERROR) {
    result("shrink splits", 0);
    return;
  }
  ok = pglevel(base) == 1 && pglevel(base + 2 * MB) == 0 &&
       pglevel(base + cut - 1) == 0 && pglevel(base + cut) == -1 &&
       checkpattern(0, cut);
  // grow back: the new pages must be zero.
  if (sbrk(4 * MB - cut) == SBRK_ERROR)
    ok = 0;
  for (uint64 i = cut; ok && i < 4 * MB; i++)
    if (base[i] != 0)
      ok = 0;
  result("shrink splits", ok);
}

// growth that is not 2-megabyte aligned, and lazy growth, use
// 4096-byte pages.
void
smalltest(void)
{
  char *top = sbrk(0), *p;
  int ok;

  sbrk(SUPERPGROUNDUP((uint64)top) - (uint64)top + PGSIZE);
  p = sbrk(2 * MB); // starts one page past a boundary
  ok = p != SBRK_ERROR && pglevel(p) == 0 && pglevel(p + 2 * MB - 1) == 0;
  sbrk(-(sbrk(0) - top));
  result("unaligned growth uses pages", ok);

  sbrk(SUPERPGROUNDUP((uint64)top) - (uint64)top);
  p = sbrklazy(2 * MB); // 2-megabyte aligned, but lazy
  ok = p != SBRK_ERROR && pglevel(p) == -1;
  for (uint64 i = 0; i < 2 * MB; i += PGSIZE)
    p[i] = 1;
  ok = ok && pglevel(p) == 0 && pglevel(p + 2 * MB - 1) == 0;
  sbrk(-(sbrk(0) - top));
  result("lazy growth uses pages", ok);
}

int
main(int argc, char *argv[])
{
  int super0 = freesuper();
  int free0 = countfree();

  // run the tests in a child, so that every page they use,
  // page-table pages included, is free again when it exits.
  if (fork() == 0) {
    supertest();
    bytetest();
    syscalltest();
    result("fork copies superpages", forkcheck(1));
    demotefork();
    shrinktest();
    smalltest();
    exit(failed);
  }
  failed = !childok();

  int super1 = freesuper();
  int free1 = countfree();
  printf("supertest: free pages %d before, %d after\n", free0, free1);
  printf("supertest: free superpages %d before, %d after\n", super0, super1);
  result("no leaks", free0 == free1 && super0 == super1);
  printf("supertest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
