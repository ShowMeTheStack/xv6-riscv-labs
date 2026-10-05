//
// tests for the physical page allocator under load from
// several harts. each check prints "kalloctest: <name>: OK"
// or "... FAIL". the timing line is information, not a check.
//

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define NCHILD 3    // one per hart
#define ROUNDS 2000 // grow/shrink rounds per child
#define NPAGES 64   // pages per round

int failed;

void
result(char *name, int ok)
{
  printf("kalloctest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
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

// grow by NPAGES pages, write and check every page, shrink
// again; ROUNDS times. exits 0 if every page held its value.
void
churn(int id)
{
  for (int r = 0; r < ROUNDS; r++) {
    char *p = sbrk(NPAGES * PGSIZE);
    if (p == SBRK_ERROR)
      exit(1);
    for (int i = 0; i < NPAGES; i++)
      p[i * PGSIZE] = id + r + i;
    for (int i = 0; i < NPAGES; i++)
      if (p[i * PGSIZE] != (char)(id + r + i))
        exit(1);
    sbrk(-(NPAGES * PGSIZE));
  }
  exit(0);
}

// NCHILD processes allocate and free pages at the same time.
void
churntest(void)
{
  int ok = 1, xstatus;
  int t0 = uptime();

  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("kalloctest: fork failed\n");
      exit(1);
    }
    if (pid == 0)
      churn(i);
  }
  for (int i = 0; i < NCHILD; i++)
    if (wait(&xstatus) < 0 || xstatus != 0)
      ok = 0;
  printf("kalloctest: churn: %d processes x %d rounds x %d pages, %d ticks\n",
         NCHILD, ROUNDS, NPAGES, uptime() - t0);
  result("churn", ok);
}

// NCHILD processes each take pages until none is left, all at
// the same time, and keep them until every one has run out.
// returns how many pages they got in all, or -1.
int
exhaust(void)
{
  int done[2], go[2], xstatus, total = 0;
  char c;

  if (pipe(done) < 0 || pipe(go) < 0)
    return -1;
  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();
    if (pid < 0)
      return -1;
    if (pid == 0) {
      int n = 0;
      char *p;
      close(done[0]);
      close(go[1]);
      while ((p = sbrk(PGSIZE)) != SBRK_ERROR) {
        *p = 1;
        n++;
      }
      write(done[1], "x", 1);
      read(go[0], &c, 1); // returns 0 when the parent closes go[1]
      exit(n);
    }
  }
  close(done[1]);
  close(go[0]);
  for (int i = 0; i < NCHILD; i++)
    if (read(done[0], &c, 1) != 1)
      return -1;
  close(go[1]);
  close(done[0]);
  for (int i = 0; i < NCHILD; i++) {
    if (wait(&xstatus) < 0)
      return -1;
    total += xstatus;
  }
  return total;
}

int
main(int argc, char *argv[])
{
  int free0, free1, free2, got;

  free0 = countfree();
  churntest();

  // the freed pages may now be spread over several lists;
  // one process must still be able to get every one.
  free1 = countfree();
  printf("kalloctest: one process got %d free pages, %d at start\n", free1, free0);
  result("gather", free1 == free0);

  got = exhaust();
  // the children's own page-table and fork pages come out of
  // the same memory, so they get a little less than free0.
  printf("kalloctest: exhaust: %d processes got %d of %d free pages\n", NCHILD, got, free0);
  result("exhaust", got > free0 - 256);

  free2 = countfree();
  printf("kalloctest: free pages %d before, %d after\n", free0, free2);
  result("no leaks", free2 == free0);

  if (failed)
    printf("kalloctest: SOME TESTS FAILED\n");
  else
    printf("kalloctest: ALL OK\n");
  exit(failed);
}
