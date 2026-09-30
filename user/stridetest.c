// stridetest: is the CPU shared in proportion to tickets?
//
//   stridetest              run the checks
//   stridetest t1 t2 ...    measure only: one spinning process per
//                           ticket count; prints shares, checks nothing
//
// A spinning process asks the kernel for its CPU time (cputime) when
// it first sees the clock (uptime) inside the measurement window and
// again when the window has ended. Its share of the CPU time of all
// spinners is its share of the CPU; the checks use that.
//
// It also counts "chunks" of a busy loop finished inside the window.
// They are printed for information only: on QEMU a chunk takes more
// or less time depending on how fast the host runs each hart.

#include "kernel/types.h"
#include "user/user.h"

#define CHUNK   50000 // loop iterations between two looks at the clock
#define MAXSPIN 8

struct result {
  int i;          // which spinner
  int chunks;     // chunks finished inside the window
  uint64 cpu;     // CPU time used inside the window (time CSR units)
};

int wstart, wend;   // the measurement window, in ticks

// Spin until tick `wend`. Measure the CPU time used and count the
// chunks finished inside [wstart, wend). Send the result to fd and
// exit.
void
spin(int i, int fd)
{
  struct result r = {i, 0, 0};
  volatile int x = 0;
  uint64 t0 = 0;
  int now, inside = 0;

  for (;;) {
    for (int k = 0; k < CHUNK; k++)
      x++;
    now = uptime();
    if (now >= wend)
      break;
    if (now >= wstart) {
      if (!inside) {
        inside = 1;
        t0 = cputime();
      }
      r.chunks++;
    }
  }
  if (inside)
    r.cpu = cputime() - t0;
  write(fd, &r, sizeof(r));
  exit(0);
}

int
xfork(void)
{
  int pid = fork();
  if (pid < 0) {
    printf("stridetest: fork failed\n");
    exit(1);
  }
  return pid;
}

// Run n spinners with the given tickets. The window starts `warm`
// ticks from now and lasts `len` ticks. Spinner `sleeper` (or -1)
// sleeps until the window starts; spinner `newcomer` (or -1) is not
// forked until then. Fill in res[].
//
// Both really sleep, in read() on a pipe. (pause() would not do:
// it wakes up at every tick to look at the clock.) An alarm process
// writes to the pipe when the window starts.
void
measure(int n, int *tickets, int warm, int len, int sleeper, int newcomer,
        struct result *res)
{
  int out[2], go[2];
  struct result r;
  char c;

  if (pipe(out) < 0 || pipe(go) < 0) {
    printf("stridetest: pipe failed\n");
    exit(1);
  }
  wstart = uptime() + warm;
  wend = wstart + len;
  for (int i = 0; i < n; i++) {
    if (i == newcomer)
      continue;
    if (xfork() == 0) {
      settickets(tickets[i]);
      if (i == sleeper)
        read(go[0], &c, 1);
      spin(i, out[1]);
    }
  }
  if (sleeper >= 0 || newcomer >= 0) {
    if (xfork() == 0) {
      pause(wstart - uptime());
      write(go[1], "g", 1);
      exit(0);
    }
  }
  if (newcomer >= 0) {
    read(go[0], &c, 1);
    if (xfork() == 0) {
      settickets(tickets[newcomer]);
      spin(newcomer, out[1]);
    }
  }
  close(out[1]);
  for (int i = 0; i < n; i++) {
    if (read(out[0], &r, sizeof(r)) != sizeof(r)) {
      printf("stridetest: short read\n");
      exit(1);
    }
    res[r.i] = r;
  }
  close(out[0]);
  close(go[0]);
  close(go[1]);
  while (wait(0) > 0)
    ;
}

// settickets checks its argument and returns the old value;
// fork and exec keep the tickets.
int
ticketstest(void)
{
  int ok = 1, old, st;
  char *argv[] = {"stridetest", "-t", 0};

  if (settickets(0) != -1 || settickets(-3) != -1 || settickets(101) != -1) {
    printf("stridetest: tickets: a bad count was accepted\n");
    ok = 0;
  }
  old = settickets(7);
  if (settickets(7) != 7) {
    printf("stridetest: tickets: settickets did not return the old count\n");
    ok = 0;
  }
  if (fork() == 0)
    exit(settickets(1));
  wait(&st);
  if (st != 7) {
    printf("stridetest: tickets: fork child had %d tickets, not 7\n", st);
    ok = 0;
  }
  if (fork() == 0) {
    exec("/stridetest", argv);
    exit(-1);
  }
  wait(&st);
  if (st != 7) {
    printf("stridetest: tickets: after exec %d tickets, not 7\n", st);
    ok = 0;
  }
  settickets(old);
  printf("stridetest: tickets: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

// Per mille of `part` in `total`.
int
permille(uint64 part, uint64 total)
{
  return total ? part * 1000 / total : 0;
}

// Six processes with 1, 1, 2, 2, 3 and 3 tickets. However many harts
// (up to 4), each process wants more CPU than its share, so the
// shares should be 1/6, 2/6 and 3/6 for the three ticket counts.
int
sharetest(void)
{
  int tickets[6] = {1, 1, 2, 2, 3, 3};
  struct result res[6];
  uint64 cpu = 0, chunks = 0;
  int ok = 1;

  measure(6, tickets, 5, 50, -1, -1, res);
  for (int i = 0; i < 6; i++) {
    cpu += res[i].cpu;
    chunks += res[i].chunks;
  }
  printf("stridetest: share: 6 processes with 1 1 2 2 3 3 tickets, 50 ticks\n");
  for (int t = 1; t <= 3; t++) {
    struct result *a = &res[2 * t - 2], *b = &res[2 * t - 1];
    int share = permille(a->cpu + b->cpu, cpu);
    int ideal = (t * 1000 + 3) / 6; // rounded
    printf("stridetest: share: tickets %d: cpu %d per mille (ideal %d); "
           "chunks %d per mille\n",
           t, share, ideal, permille(a->chunks + b->chunks, chunks));
    // within 20% of the ideal share
    if (5 * (share - ideal) > ideal || 5 * (ideal - share) > ideal)
      ok = 0;
  }
  printf("stridetest: share: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

// Six processes with equal tickets; process 0 either sleeps through
// the first 30 ticks (sleeper) or is only created after them
// (newcomer). In the next 30 ticks it should get about as much CPU
// as each of the others, not a burst to make up for lost time.
int
latetest(char *what, int sleeper, int newcomer)
{
  int tickets[6] = {5, 5, 5, 5, 5, 5};
  struct result res[6];
  uint64 others = 0;
  int ratio, ok;

  measure(6, tickets, 30, 30, sleeper, newcomer, res);
  for (int i = 1; i < 6; i++)
    others += res[i].cpu;
  ratio = others ? res[0].cpu * 5 * 100 / others : 0;
  ok = ratio >= 67 && ratio <= 150;
  printf("stridetest: %s: cpu %d ms, the others %d ms on average "
         "(ratio %d/100)\n",
         what, (int)(res[0].cpu / 10000), (int)(others / 5 / 10000), ratio);
  printf("stridetest: %s: %s\n", what, ok ? "OK" : "FAIL");
  return ok;
}

int
main(int argc, char *argv[])
{
  int tickets[MAXSPIN], n, sum = 0, ok = 1;
  struct result res[MAXSPIN];
  uint64 cpu = 0, chunks = 0;

  if (argc == 2 && strcmp(argv[1], "-t") == 0)
    exit(settickets(1)); // the exec part of ticketstest

  if (argc > 1) {
    n = argc - 1;
    if (n > MAXSPIN)
      n = MAXSPIN;
    for (int i = 0; i < n; i++) {
      tickets[i] = atoi(argv[i + 1]);
      sum += tickets[i];
    }
    measure(n, tickets, 5, 50, -1, -1, res);
    for (int i = 0; i < n; i++) {
      cpu += res[i].cpu;
      chunks += res[i].chunks;
    }
    for (int i = 0; i < n; i++)
      printf("stridetest: %d tickets: cpu %d ms, %d per mille "
             "(%d if shared by tickets); chunks %d per mille\n",
             tickets[i], (int)(res[i].cpu / 10000), permille(res[i].cpu, cpu),
             tickets[i] * 1000 / sum, permille(res[i].chunks, chunks));
    printf("stridetest: measured only, nothing checked\n");
    exit(0);
  }

  ok &= ticketstest();
  ok &= sharetest();
  ok &= latetest("sleeper", 0, -1);
  ok &= latetest("newcomer", -1, 0);
  printf("stridetest: %s\n", ok ? "ALL OK" : "SOME FAILED");
  exit(0);
}
