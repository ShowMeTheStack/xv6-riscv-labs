// alarmtest: tests for sigalarm and sigreturn.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

volatile int count; // calls of periodic

// the first function in this file: in this build it sits at
// user address 0, so a kernel that treats a handler of 0 as
// "no handler" never calls it.
void
periodic()
{
  count++;
  sigreturn();
}

volatile int depth, maxdepth, slowcalls;

// a handler that runs for 3 ticks or more, longer than its interval.
void
slow()
{
  int t0;

  depth++;
  if (depth > maxdepth)
    maxdepth = depth;
  if (depth > 1)
    printf("alarmtest: slow entered while already running\n");
  t0 = uptime();
  while (uptime() - t0 < 3)
    for (volatile int i = 0; i < 100000; i++)
      ;
  slowcalls++;
  depth--;
  sigreturn();
}

// burn user CPU time until *c reaches n or t ticks pass.
// returns 1 if *c reached n.
int
waitfor(volatile int *c, int n, int t)
{
  int t0 = uptime();

  while (*c < n) {
    if (uptime() - t0 >= t)
      return 0;
    for (volatile int i = 0; i < 100000; i++)
      ;
  }
  return 1;
}

int
check(char *what, int ok)
{
  printf("alarmtest: %s: %s\n", what, ok ? "OK" : "FAIL");
  return ok;
}

// the handler runs every 2 ticks; sigalarm(0, 0) stops it.
int
test_called(void)
{
  int ok, n;

  printf("alarmtest: periodic is at address %p\n", periodic);
  count = 0;
  sigalarm(2, periodic);
  ok = waitfor(&count, 5, 300);
  sigalarm(0, 0);
  n = count;
  waitfor(&count, n + 1, 10);
  printf("alarmtest: %d calls; %d more in 10 ticks after sigalarm(0, 0)\n", n,
         count - n);
  return check("handler called, then stopped", ok && count == n) +
         check("sigreturn outside a handler fails", sigreturn() == -1);
}

// load register r with eight copies of the byte 0xb.
#define FILL(r, b) "li " #r ", 0x" #b #b #b #b #b #b #b #b "\n"
// regs[i] = the register (i is its number, in decimal)
#define SAVE(r, i) "sd " #r ", " #i "*8(a0)\n"

uint64 regs[32];

// fill 26 registers, spin with the counter in a0, save them all.
void
fillspin(void)
{
  asm volatile(FILL(ra, 01) FILL(t0, 05) FILL(t1, 06) FILL(t2, 07)
               FILL(s1, 09) FILL(a1, 11) FILL(a2, 12) FILL(a3, 13)
               FILL(a4, 14) FILL(a5, 15) FILL(a6, 16) FILL(a7, 17)
               FILL(s2, 18) FILL(s3, 19) FILL(s4, 20) FILL(s5, 21)
               FILL(s6, 22) FILL(s7, 23) FILL(s8, 24) FILL(s9, 25)
               FILL(s10, 26) FILL(s11, 27) FILL(t3, 28) FILL(t4, 29)
               FILL(t5, 30) FILL(t6, 31)
               "li a0, 20000000\n"
               "1: addi a0, a0, -1\n"
               "bgtz a0, 1b\n"
               "la a0, regs\n"
               SAVE(ra, 1) SAVE(t0, 5) SAVE(t1, 6) SAVE(t2, 7)
               SAVE(s1, 9) SAVE(a1, 11) SAVE(a2, 12) SAVE(a3, 13)
               SAVE(a4, 14) SAVE(a5, 15) SAVE(a6, 16) SAVE(a7, 17)
               SAVE(s2, 18) SAVE(s3, 19) SAVE(s4, 20) SAVE(s5, 21)
               SAVE(s6, 22) SAVE(s7, 23) SAVE(s8, 24) SAVE(s9, 25)
               SAVE(s10, 26) SAVE(s11, 27) SAVE(t3, 28) SAVE(t4, 29)
               SAVE(t5, 30) SAVE(t6, 31)
               :
               :
               : "ra", "t0", "t1", "t2", "s1", "a0", "a1", "a2", "a3", "a4",
                 "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8",
                 "s9", "s10", "s11", "t3", "t4", "t5", "t6", "memory");
}

// the registers fillspin checks, by number. FILL writes the number's
// decimal digits as hex: x11 holds 0x1111111111111111.
int regno[] = {1,  5,  6,  7,  9,  11, 12, 13, 14, 15, 16, 17, 18,
               19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

// the handler interrupts fillspin; every register must survive.
int
test_registers(void)
{
  int c0, hits = 0, bad = 0, t0 = uptime();

  count = 0;
  sigalarm(1, periodic);
  while (hits < 10 && uptime() - t0 < 300) {
    c0 = count;
    fillspin();
    if (count != c0)
      hits++;
    for (int i = 0; i < sizeof(regno) / sizeof(regno[0]); i++) {
      int n = regno[i];
      uint64 want = 0x0101010101010101ULL * (n / 10 * 16 + n % 10);
      if (regs[n] != want) {
        printf("alarmtest: x%d is 0x%lx, not 0x%lx\n", n, regs[n], want);
        bad++;
      }
    }
  }
  sigalarm(0, 0);
  printf("alarmtest: the handler ran during %d fillspin runs (%d calls)\n",
         hits, count);
  return check("registers preserved", hits == 10 && bad == 0);
}

// a handler that runs longer than the interval is not entered again.
int
test_reentry(void)
{
  int ok;

  depth = maxdepth = slowcalls = 0;
  sigalarm(1, slow);
  ok = waitfor(&slowcalls, 3, 300);
  sigalarm(0, 0);
  printf("alarmtest: slow ran %d times, nested at most %d deep\n", slowcalls,
         maxdepth);
  return check("no re-entry", ok && maxdepth == 1);
}

// spin with a0 holding 0x0a0a0a0a0a0a0a0a; return a0.
uint64
a0spin(void)
{
  uint64 a0;

  asm volatile("li a0, 0x0a0a0a0a0a0a0a0a\n"
               "li t0, 20000000\n"
               "1: addi t0, t0, -1\n"
               "bgtz t0, 1b\n"
               "mv %0, a0\n"
               : "=r"(a0)
               :
               : "a0", "t0");
  return a0;
}

// sigreturn is a system call, and syscall() stores its return
// value in a0: the interrupted code's a0 must still survive.
int
test_a0(void)
{
  int c0, hits = 0, bad = 0, t0 = uptime();
  uint64 a0;

  count = 0;
  sigalarm(1, periodic);
  while (hits < 10 && uptime() - t0 < 300) {
    c0 = count;
    a0 = a0spin();
    if (count != c0)
      hits++;
    if (a0 != 0x0a0a0a0a0a0a0a0aULL) {
      printf("alarmtest: a0 is 0x%lx after sigreturn\n", a0);
      bad++;
    }
  }
  sigalarm(0, 0);
  printf("alarmtest: the handler ran during %d a0spin runs (%d calls)\n", hits,
         count);
  return check("sigreturn restores a0", hits == 10 && bad == 0);
}

// a forked child, and an exec'd program, start with no alarm.
int
test_forkexec(void)
{
  int pid, st1, st2;
  char *argv[] = {"alarmtest", "exec", 0};

  sigalarm(1, periodic);
  pid = fork();
  if (pid == 0) {
    count = 0;
    waitfor(&count, 1, 10);
    exit(count);
  }
  wait(&st1);
  pid = fork();
  if (pid == 0) {
    // turn the alarm on in this process, then replace its program.
    sigalarm(1, periodic);
    exec("alarmtest", argv);
    exit(-1);
  }
  wait(&st2);
  sigalarm(0, 0);
  printf("alarmtest: handler calls in the forked child: %d, after exec: %d\n",
         st1, st2);
  return check("fork and exec start with the alarm off", st1 == 0 && st2 == 0);
}

int
main(int argc, char *argv[])
{
  int ok = 0;

  if (argc == 2 && strcmp(argv[1], "exec") == 0) {
    // the exec'd half of test_forkexec: periodic is at the same
    // address in this new image, so a surviving alarm would call it.
    waitfor(&count, 1, 10);
    exit(count);
  }
  ok += test_called();
  ok += test_registers();
  ok += test_reentry();
  ok += test_a0();
  ok += test_forkexec();
  if (ok == 6)
    printf("alarmtest: 6 of 6 checks OK\n");
  else
    printf("alarmtest: %d of 6 checks FAILED\n", 6 - ok);
  exit(0);
}
