// time command args...: run a command, then print how long it took
// (real) and how much CPU time it used in user mode and in the kernel.

#include "kernel/types.h"
#include "kernel/cputime.h"
#include "user/user.h"

// print microseconds as seconds with three decimals.
static void
show(char *what, uint64 us)
{
  uint64 ms = us / 1000;
  fprintf(2, "%s %lu.%lu%lu%lu\n", what, ms / 1000, ms / 100 % 10,
          ms / 10 % 10, ms % 10);
}

int
main(int argc, char *argv[])
{
  struct cputime t0, t1;
  int pid, status;

  if (argc < 2) {
    fprintf(2, "usage: time command [args...]\n");
    exit(1);
  }

  cputime(&t0);
  pid = fork();
  if (pid < 0) {
    fprintf(2, "time: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    exec(argv[1], argv + 1);
    fprintf(2, "time: exec %s failed\n", argv[1]);
    exit(1);
  }
  wait(&status);
  cputime(&t1);

  // the child is reaped, so its times are in our children's totals.
  show("real", t1.real - t0.real);
  show("user", t1.cutime - t0.cutime);
  show("sys ", t1.cstime - t0.cstime);
  exit(status);
}
