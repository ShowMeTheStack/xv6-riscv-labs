// Test swapping: use more memory than the machine has, check
// that every page comes back with its contents, use pages in
// swap as system call buffers, fork a process that is partly in
// swap, and run several big processes at once.

#include "kernel/types.h"
#include "kernel/swapstat.h"
#include "user/user.h"

#define PG 4096

int npages = 36000; // more than the 32,768 pages of RAM

// the value of word w of page i in round r.
uint64
val(uint64 i, uint64 w, uint64 r)
{
  return (i << 32) ^ (w << 20) ^ (r * 0x9e3779b97f4a7c15ULL);
}

void
fill(char *base, int n, int r)
{
  for (int i = 0; i < n; i++) {
    uint64 *p = (uint64 *)(base + (uint64)i * PG);
    for (int w = 0; w < PG / 8; w++)
      p[w] = val(i, w, r);
  }
}

// check pages [0, n), the last one first if backward.
// returns the number of pages with a wrong word.
int
check(char *base, int n, int r, int backward)
{
  int bad = 0;
  for (int k = 0; k < n; k++) {
    int i = backward ? n - 1 - k : k;
    uint64 *p = (uint64 *)(base + (uint64)i * PG);
    for (int w = 0; w < PG / 8; w++) {
      if (p[w] != val(i, w, r)) {
        if (bad < 3)
          printf("swaptest: page %d word %d is %lx\n", i, w, p[w]);
        bad++;
        break;
      }
    }
  }
  return bad;
}

struct swapstat
sst(void)
{
  struct swapstat st;
  swapstat(&st);
  return st;
}

// wait for one child; 1 if it exited with status 0.
int
childok(void)
{
  int xs = 1;
  if (wait(&xs) < 0)
    return 0;
  return xs == 0;
}

char *
grow(int n)
{
  char *p = sbrklazy(n * PG);
  if (p == SBRK_ERROR) {
    printf("swaptest: sbrklazy(%d pages) failed\n", n);
    exit(1);
  }
  return p;
}

// write npages pages, then check them, newest first.
void
bigtest(void)
{
  struct swapstat a = sst(), b, c;
  char *base = grow(npages);
  fill(base, npages, 1);
  b = sst();
  printf("swaptest: big: wrote %d pages: %d out, %d in\n", npages,
         b.pageouts - a.pageouts, b.pageins - a.pageins);
  int bad = check(base, npages, 1, 1);
  c = sst();
  printf("swaptest: big: checked %d pages, %d wrong: %d out, %d in\n",
         npages, bad, c.pageouts - b.pageouts, c.pageins - b.pageins);
  exit(bad != 0);
}

// the kernel reads and writes pages that are in swap: copy page 2i
// to page 2i+1 through a pipe, 512 bytes (the pipe's size) at a
// time, until 4 source pages and 4 destination pages were found
// in swap (the first write() or read() of the page read it in).
void
syscalltest(void)
{
  int fds[2], bad = 0, nsrc = 0, ndst = 0, i;
  char *base = grow(npages);
  fill(base, npages, 2);
  if (pipe(fds) < 0)
    exit(1);
  for (i = 0; i < npages / 2 && (nsrc < 4 || ndst < 4); i++) {
    char *src = base + (uint64)(2 * i) * PG;
    char *dst = src + PG;
    for (int off = 0; off < PG; off += 512) {
      struct swapstat a = sst();
      if (write(fds[1], src + off, 512) != 512) {
        printf("swaptest: write failed\n");
        exit(1);
      }
      struct swapstat b = sst();
      if (read(fds[0], dst + off, 512) != 512) {
        printf("swaptest: read failed\n");
        exit(1);
      }
      struct swapstat c = sst();
      if (off == 0) {
        nsrc += b.pageins > a.pageins;
        ndst += c.pageins > b.pageins;
      }
    }
    if (memcmp(src, dst, PG) != 0)
      bad++;
  }
  printf("swaptest: syscalls: copied %d pages, %d wrong; %d sources and %d "
         "destinations were in swap\n",
         i, bad, nsrc, ndst);
  exit(bad != 0 || nsrc < 4 || ndst < 4);
}

// fill 2000 pages, push them out by using a lot of memory, give
// that memory back, then fork. both processes check all pages,
// and the child's writes must not reach the parent.
void
forktest(void)
{
  int m = 2000, h = 37000;
  char *base = grow(m);
  fill(base, m, 3);
  char *hog = grow(h);
  for (int i = 0; i < h; i++)
    hog[(uint64)i * PG] = 1;
  sbrk(-h * PG);
  struct swapstat a = sst();
  printf("swaptest: fork: %d slots in use at fork\n", a.slotsused);
  int pid = fork();
  if (pid < 0) {
    printf("swaptest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    int bad = check(base, m, 3, 0);
    fill(base, m / 2, 4); // must not show in the parent
    exit(bad != 0);
  }
  int cok = childok();
  int bad = check(base, m, 3, 0);
  struct swapstat b = sst();
  printf("swaptest: fork: child %s, parent %d wrong, %d pages read in\n",
         cok ? "ok" : "FAILED", bad, b.pageins - a.pageins);
  exit(!cok || bad != 0 || a.slotsused == 0);
}

// three processes of 12,500 pages each: all three fill their
// pages before any of them checks, so together they need more
// memory than there is and evict each other's pages.
void
procstest(void)
{
  int n = 12500, ready[2], go[2], ok = 1;
  char c;
  struct swapstat a = sst();
  if (pipe(ready) < 0 || pipe(go) < 0)
    exit(1);
  for (int k = 0; k < 3; k++) {
    int pid = fork();
    if (pid < 0) {
      printf("swaptest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      close(ready[0]);
      close(go[1]);
      char *base = grow(n);
      fill(base, n, 10 + k);
      write(ready[1], "r", 1);
      close(ready[1]);
      read(go[0], &c, 1); // wait until all three have filled
      exit(check(base, n, 10 + k, 1) != 0);
    }
  }
  // a child that was killed never writes; its exit closes its end,
  // so read returns 0 instead of waiting forever.
  close(ready[1]);
  for (int k = 0; k < 3; k++)
    if (read(ready[0], &c, 1) != 1)
      break;
  write(go[1], "ggg", 3);
  for (int k = 0; k < 3; k++)
    ok &= childok();
  struct swapstat b = sst();
  printf("swaptest: procs: %d out, %d in\n", b.pageouts - a.pageouts,
         b.pageins - a.pageins);
  exit(!ok || b.pageouts == a.pageouts);
}

// run f in a child and print OK or FAIL.
int
run(void (*f)(void), char *name)
{
  int pid = fork();
  if (pid < 0) {
    printf("swaptest: fork failed\n");
    exit(1);
  }
  if (pid == 0)
    f();
  int ok = childok();
  printf("swaptest: %s: %s\n", name, ok ? "OK" : "FAIL");
  return ok;
}

int
main(int argc, char *argv[])
{
  int ok = 1;
  if (argc > 1)
    npages = atoi(argv[1]);
  struct swapstat a = sst();
  ok &= run(bigtest, "big");
  ok &= run(syscalltest, "syscalls");
  ok &= run(forktest, "fork");
  ok &= run(procstest, "procs");
  struct swapstat b = sst();
  // a page moving between memory and swap changes both counts.
  printf("swaptest: free pages + free slots: %d before, %d after\n",
         a.freepages + a.nslots - a.slotsused,
         b.freepages + b.nslots - b.slotsused);
  if (a.freepages - a.slotsused != b.freepages - b.slotsused) {
    printf("swaptest: leak: FAIL\n");
    ok = 0;
  } else {
    printf("swaptest: leak: OK\n");
  }
  printf("swaptest: %d pages written out, %d read in\n",
         b.pageouts - a.pageouts, b.pageins - a.pageins);
  printf("swaptest: %s\n", ok ? "ALL OK" : "SOME TESTS FAILED");
  exit(!ok);
}
