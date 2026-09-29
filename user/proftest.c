// proftest: checks the profile and profget system calls.

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/prof.h"
#include "user/user.h"

#define PGSIZE 4096

static struct prof pr;
static int failed;

static void
check(char *what, int ok)
{
  printf("proftest: %s: %s\n", what, ok ? "OK" : "FAIL");
  if (!ok)
    failed++;
}

// spin in user mode for n clock ticks.
__attribute__((noinline)) uint
hot(int n)
{
  uint x = 1;
  int t0 = uptime();

  while (uptime() - t0 < n)
    for (int i = 0; i < 1000000; i++)
      x = x * 1103515245 + 12345;
  return x;
}

int hotsamples(void);
int hotprofile(char *);
int countfree(void);
int main(int, char **);

// where hot() ends: the compiler need not place functions in
// source order, so take the nearest function above it.
uint64
hotend(void)
{
  uint64 f[] = {(uint64)check, (uint64)hotend,     (uint64)hotsamples,
                (uint64)hotprofile, (uint64)countfree, (uint64)main};
  uint64 end = PGSIZE;

  for (int i = 0; i < sizeof(f) / sizeof(f[0]); i++)
    if (f[i] > (uint64)hot && f[i] < end)
      end = f[i];
  return end;
}

// how many samples in pr landed inside hot()?
int
hotsamples(void)
{
  int n = 0;
  uint64 end = hotend();

  for (int i = 0; i < pr.nbucket; i++) {
    uint64 a = pr.lo + ((uint64)i << pr.shift);
    if (a >= (uint64)hot && a < end)
      n += pr.bucket[i];
  }
  return n;
}

// is pr a good profile of a run of hot()?
int
hotprofile(char *who)
{
  int h = hotsamples();
  int in = pr.nsample - pr.nout;

  printf("proftest: %s: %d samples, %d in range, %d in hot()\n", who,
         pr.nsample, in, h);
  return pr.nsample >= 10 && h * 10 >= in * 9;
}

// count free pages, by allocating them all.
int
countfree(void)
{
  int n = 0;
  char *sz0 = sbrk(0);

  while (sbrk(PGSIZE) != SBRK_ERROR)
    n++;
  sbrk(-(sbrk(0) - sz0));
  return n;
}

void
badargs(void)
{
  int ok = profget(&pr) < 0;
  ok = ok && profile(100, 100, 2) < 0;           // empty range
  ok = ok && profile(0, 0x100000, 2) < 0;        // too many buckets
  ok = ok && profile(0, PGSIZE, -1) < 0;         // bad shift
  ok = ok && profile(0, PGSIZE, 3) == 0;         // 512 buckets
  ok = ok && profget(&pr) == 0 && pr.nbucket == 512;
  ok = ok && profile(0, 0, 0) == 0 && profget(&pr) < 0;
  check("arguments, and profile(0, 0, 0) stops", ok);
}

void
hotloop(void)
{
  if (profile(0, PGSIZE, 3) < 0 || hot(30) == 0 || profget(&pr) < 0) {
    check("a hot loop", 0);
    return;
  }
  profile(0, 0, 0);
  check("most samples in the hot loop", hotprofile("hot loop"));
}

void
forkexec(void)
{
  int xs = 1;

  profile(0, PGSIZE, 3);
  if (fork() == 0)
    exit(profget(&pr) < 0 ? 0 : 1);
  wait(&xs);
  check("fork does not inherit the profile", xs == 0);

  xs = 1;
  if (fork() == 0) {
    char *argv[] = {"proftest", "exec", 0};
    profile(0, PGSIZE, 3);
    exec("proftest", argv);
    exit(1);
  }
  wait(&xs);
  check("exec keeps the profile", xs == 0);
  profile(0, 0, 0);
}

void
exitdump(void)
{
  int fd, n, xs = 1;

  unlink("prof.out");
  if (fork() == 0) {
    profile(0, PGSIZE, 3);
    hot(10);
    exit(0); // no profile(0, 0, 0): kexit writes prof.out
  }
  wait(&xs);
  memset(&pr, 0, sizeof(pr));
  if ((fd = open("prof.out", O_RDONLY)) < 0) {
    check("exit writes prof.out", 0);
    return;
  }
  n = read(fd, &pr, sizeof(pr));
  close(fd);
  unlink("prof.out");
  check("exit writes prof.out",
        xs == 0 && n == sizeof(pr) && pr.lo == 0 && pr.hi == PGSIZE &&
          pr.shift == 3 && hotprofile("prof.out"));
}

void
leaks(void)
{
  int free0 = countfree();

  for (int i = 0; i < 20; i++) {
    if (fork() == 0) {
      profile(0, PGSIZE, 3);
      exit(0);
    }
    wait(0);
  }
  unlink("prof.out");
  int free1 = countfree();
  printf("proftest: free pages %d before, %d after\n", free0, free1);
  check("no leaks", free0 == free1);
}

int
main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "exec") == 0) {
    // run by forkexec, which called profile(0, 4096, 3) before exec.
    exit(profget(&pr) == 0 && pr.lo == 0 && pr.hi == PGSIZE ? 0 : 1);
  }
  if (hotend() >= PGSIZE) {
    printf("proftest: hot() is not where the test expects it\n");
    exit(1);
  }
  badargs();
  hotloop();
  forkexec();
  exitdump();
  leaks();
  if (failed == 0)
    printf("proftest: ALL OK\n");
  else
    printf("proftest: %d FAILED\n", failed);
  exit(0);
}
