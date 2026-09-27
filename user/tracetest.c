// Tests for the trace system call.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/syscall.h"
#include "user/user.h"

#define MASK (1 << SYS_uptime)

int fails;

void
check(int ok, char *what)
{
  printf("tracetest: %s: %s\n", what, ok ? "OK" : "FAIL");
  if (!ok)
    fails++;
}

// fork a child that runs f() and exits with its result;
// return the child's exit status.
int
inchild(int (*f)(void))
{
  int pid, status;

  pid = fork();
  if (pid < 0) {
    printf("tracetest: fork failed\n");
    exit(1);
  }
  if (pid == 0)
    exit(f());
  if (wait(&status) != pid)
    return -1;
  return status;
}

// child: report whether the mask arrived, then clear it.
int
childmask(void)
{
  return trace(0) == MASK ? 0 : 1;
}

// child: exec tracetest again, which checks its own mask.
int
execmask(void)
{
  char *argv[] = {"tracetest", "exec", 0};
  exec("/tracetest", argv);
  return 2;
}

int
main(int argc, char *argv[])
{
  int pid, child;

  // run by execmask(): did the mask survive exec?
  if (argc == 2 && strcmp(argv[1], "exec") == 0)
    exit(trace(0) == MASK ? 0 : 1);

  trace(0);
  trace(MASK);
  check(trace(MASK) == MASK, "trace returns the previous mask");
  check(inchild(childmask) == 0, "fork copies the mask to the child");
  check(trace(MASK) == MASK, "the child's trace(0) leaves the parent alone");
  check(inchild(execmask) == 0, "exec keeps the mask");
  trace(0);

  // only getpid is traced between the markers: the parent's and
  // the child's getpid print one line each, nothing else prints.
  pid = getpid();
  printf("tracetest: begin\n");
  trace(1 << SYS_getpid);
  getpid();
  uptime();
  child = fork();
  if (child == 0) {
    getpid();
    exit(0);
  }
  wait(0);
  trace(0);
  printf("tracetest: end\n");
  printf("tracetest: expected between the markers, and nothing else:\n");
  printf("%d: syscall getpid -> %d\n", pid, pid);
  printf("%d: syscall getpid -> %d\n", child, child);

  if (fails == 0)
    printf("tracetest: 4 checks OK; now compare the lines between the "
           "markers by eye\n");
  else
    printf("tracetest: %d FAILED\n", fails);
  exit(fails != 0);
}
