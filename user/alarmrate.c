// alarmrate INTERVAL SECONDS NPROC: NPROC processes each burn user
// CPU time for SECONDS of wall-clock time with sigalarm(INTERVAL)
// on, and report how often their handlers ran.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

volatile int calls;

void
tick()
{
  calls++;
  sigreturn();
}

int
main(int argc, char *argv[])
{
  int interval, ticks, nproc, pid, st;

  if (argc != 4) {
    fprintf(2, "usage: alarmrate interval seconds nproc\n");
    exit(1);
  }
  interval = atoi(argv[1]);
  ticks = atoi(argv[2]) * 10; // uptime() counts 10 ticks a second
  nproc = atoi(argv[3]);
  if (interval < 1 || ticks < 1 || nproc < 1) {
    fprintf(2, "alarmrate: bad arguments\n");
    exit(1);
  }

  for (int i = 0; i < nproc; i++) {
    if (fork() == 0) {
      int t0 = uptime();
      sigalarm(interval, tick);
      while (uptime() - t0 < ticks)
        for (volatile int j = 0; j < 100000; j++)
          ;
      sigalarm(0, 0);
      exit(calls);
    }
  }
  // the children report through their exit status, so their
  // lines cannot interleave.
  for (int i = 0; i < nproc; i++) {
    pid = wait(&st);
    printf("alarmrate: pid %d: %d handler calls\n", pid, st);
  }
  printf("alarmrate: a process with a hart to itself for %d ticks "
         "would make about %d\n",
         ticks, ticks / interval);
  exit(0);
}
