// cputest: check the kernel's CPU time accounting.
//
//   cputest              run the checks
//   cputest spin N       burn CPU in user mode for N*1,000,000 loop turns
//   cputest par N        three processes at once, each doing spin N
//   cputest sys N        make N*1000 getpid() system calls
//   cputest sleep N      sleep for N timer ticks
//
// The last four are workloads for the time command.

#include "kernel/types.h"
#include "kernel/cputime.h"
#include "user/user.h"

int failed;

void
check(char *what, int ok)
{
  printf("cputest: %s: %s\n", what, ok ? "OK" : "FAIL");
  if (!ok)
    failed++;
}

void
spin(int n)
{
  volatile int x = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < 1000000; j++)
      x++;
}

void
syscalls(int n)
{
  for (int i = 0; i < n * 1000; i++)
    getpid();
}

// burn user time until ms milliseconds of real time have passed.
void
spinfor(int ms)
{
  struct cputime t0, t;
  volatile int x = 0;

  cputime(&t0);
  do {
    for (int j = 0; j < 100000; j++)
      x++;
    cputime(&t);
  } while (t.real - t0.real < ms * 1000);
}

// print a line of measurements, in milliseconds.
void
show(char *what, struct cputime *a, struct cputime *b)
{
  printf("cputest: %s: real %lu ms, user %lu ms, sys %lu ms\n", what,
         (b->real - a->real) / 1000, (b->utime - a->utime) / 1000,
         (b->stime - a->stime) / 1000);
}

// call cputime() for ms milliseconds of real time; return how many
// times the kernel time went backwards (at most 100).
int
backwards(int ms)
{
  struct cputime t0, prev, t;
  int n = 0;

  cputime(&t0);
  prev = t0;
  do {
    cputime(&t);
    if (t.stime < prev.stime && n < 100)
      n++;
    prev = t;
  } while (t.real - t0.real < ms * 1000);
  return n;
}

// a child that spins for ms milliseconds, writes its own final
// utime and stime to fd, and exits.
void
spinchild(int ms, int fd)
{
  struct cputime t;

  spinfor(ms);
  cputime(&t);
  write(fd, &t.utime, sizeof(t.utime));
  write(fd, &t.stime, sizeof(t.stime));
  exit(0);
}

int
main(int argc, char *argv[])
{
  struct cputime a, b;
  int fds[2], pid, xs;
  uint64 cu, cs;

  if (argc == 3 && strcmp(argv[1], "spin") == 0) {
    spin(atoi(argv[2]));
    exit(0);
  }
  if (argc == 3 && strcmp(argv[1], "par") == 0) {
    for (int i = 0; i < 3; i++)
      if (fork() == 0) {
        spin(atoi(argv[2]));
        exit(0);
      }
    for (int i = 0; i < 3; i++)
      wait(&xs);
    exit(0);
  }
  if (argc == 3 && strcmp(argv[1], "sys") == 0) {
    syscalls(atoi(argv[2]));
    exit(0);
  }
  if (argc == 3 && strcmp(argv[1], "sleep") == 0) {
    pause(atoi(argv[2]));
    exit(0);
  }
  if (argc != 1) {
    fprintf(2, "usage: cputest [spin N | par N | sys N | sleep N]\n");
    exit(1);
  }

  // 1. A CPU-bound loop is user time.
  cputime(&a);
  spinfor(300);
  cputime(&b);
  show("spin", &a, &b);
  check("a CPU-bound loop is mostly user time",
        (b.utime - a.utime) * 2 >= b.real - a.real &&
          (b.stime - a.stime) * 10 < b.real - a.real);

  // 2. System calls are kernel time.
  cputime(&a);
  syscalls(20);
  cputime(&b);
  show("20000 getpids", &a, &b);
  check("system calls add kernel time", b.stime > a.stime);

  // 3. Sleeping is not CPU time, and times are in microseconds.
  cputime(&a);
  pause(5);
  cputime(&b);
  show("pause(5)", &a, &b);
  check("pause(5) takes about half a second of real time",
        b.real - a.real >= 300000 && b.real - a.real <= 1500000);
  check("sleeping adds almost no CPU time",
        (b.utime - a.utime) + (b.stime - a.stime) < (b.real - a.real) / 20);

  // 4. A child's time reaches its parent when, and only when, the
  // parent reaps it.
  cputime(&a);
  pipe(fds);
  if ((pid = fork()) == 0)
    spinchild(200, fds[1]);
  read(fds[0], &cu, sizeof(cu));
  read(fds[0], &cs, sizeof(cs));
  cputime(&b);
  check("a child's time is not counted before wait", b.cutime == a.cutime);
  wait(&xs);
  cputime(&b);
  printf("cputest: child said user %lu ms, sys %lu ms; parent got user %lu "
         "ms, sys %lu ms\n",
         cu / 1000, cs / 1000, (b.cutime - a.cutime) / 1000,
         (b.cstime - a.cstime) / 1000);
  check("wait adds the child's time",
        b.cutime - a.cutime >= cu && b.cutime - a.cutime < cu + 10000 &&
          b.cstime - a.cstime >= cs && b.cstime - a.cstime < cs + 10000);
  close(fds[0]);
  close(fds[1]);

  // 5. A new process starts from zero, even in a reused slot (the
  // child above has just been freed).
  pipe(fds);
  if ((pid = fork()) == 0) {
    cputime(&a);
    write(fds[1], &a, sizeof(a));
    exit(0);
  }
  read(fds[0], &a, sizeof(a));
  wait(&xs);
  printf("cputest: new child: user %lu us, sys %lu us\n", a.utime, a.stime);
  check("a new process starts with almost no CPU time",
        a.utime + a.stime < 10000 && a.cutime == 0 && a.cstime == 0);
  close(fds[0]);
  close(fds[1]);

  // 6. Grandchildren count once their parent has reaped them.
  cputime(&a);
  pipe(fds);
  if ((pid = fork()) == 0) {
    if (fork() == 0)
      spinchild(200, fds[1]);
    wait(&xs);
    exit(0);
  }
  read(fds[0], &cu, sizeof(cu));
  read(fds[0], &cs, sizeof(cs));
  wait(&xs);
  cputime(&b);
  check("a grandchild's time arrives through its parent",
        b.cutime - a.cutime >= cu);
  close(fds[0]);
  close(fds[1]);

  // 7. Three processes spinning on three harts use about three
  // times as much CPU time as real time.
  cputime(&a);
  for (int i = 0; i < 3; i++)
    if (fork() == 0) {
      spinfor(500);
      exit(0);
    }
  for (int i = 0; i < 3; i++)
    wait(&xs);
  cputime(&b);
  printf("cputest: 3 spinning children: real %lu ms, user %lu ms, sys %lu ms\n",
         (b.real - a.real) / 1000, (b.cutime - a.cutime) / 1000,
         (b.cstime - a.cstime) / 1000);
  check("on 3 harts, 3 spinners use more CPU time than real time",
        (b.cutime - a.cutime) * 2 >= (b.real - a.real) * 3);

  // 8. Kernel time never goes backwards, even when processes are
  // preempted in the middle of cputime(): four processes on three
  // harts, so timer interrupts make them yield to each other.
  int back = 0;
  for (int i = 0; i < 4; i++)
    if (fork() == 0)
      exit(backwards(2000));
  for (int i = 0; i < 4; i++) {
    wait(&xs);
    back += xs;
  }
  printf("cputest: 4 processes calling cputime() for 2 s: sys went "
         "backwards %d times\n",
         back);
  check("kernel time never goes backwards", back == 0);

  if (failed)
    printf("cputest: %d FAILED\n", failed);
  else
    printf("cputest: all 10 checks OK\n");
  exit(failed != 0);
}
