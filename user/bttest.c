// test kbacktrace(): the kernel prints three backtraces, one from
// a system call, one across a trap, one on a scheduler stack.

#include "kernel/types.h"
#include "user/user.h"

int failed = 0;

void
check(int where, int got, int want)
{
  printf("bttest: kbacktrace(%d) returned %d: %s\n", where, got,
         got == want ? "OK" : "FAIL");
  if (got != want)
    failed++;
}

char *what[] = {
  "from a system call, on this process's kernel stack",
  "from a timer interrupt during a system call",
  "from a timer interrupt on a scheduler stack",
};

int
main(int argc, char *argv[])
{
  int where, n = 0;

  // "bttest 2" runs only that case, to look at one stack at a time.
  for (where = 0; where < 3; where++) {
    if (argc > 1 && atoi(argv[1]) != where)
      continue;
    printf("bttest: %s:\n", what[where]);
    check(where, kbacktrace(where), 0);
    n++;
  }
  if (argc == 1) {
    check(3, kbacktrace(3), -1);
    n++;
  }

  if (failed) {
    printf("bttest: %d FAILED\n", failed);
    exit(1);
  }
  printf("bttest: %d checks OK; the addresses can only be checked by "
         "symbolizing them on the host with addr2line\n", n);
  exit(0);
}
