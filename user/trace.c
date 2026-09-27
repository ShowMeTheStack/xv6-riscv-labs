#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// trace MASK command [args...]
// run command with system-call tracing set to MASK.
int
main(int argc, char *argv[])
{
  if (argc < 3 || argv[1][0] < '0' || argv[1][0] > '9') {
    fprintf(2, "usage: trace mask command [args...]\n");
    exit(1);
  }

  trace(atoi(argv[1]));

  exec(argv[2], &argv[2]);
  fprintf(2, "trace: exec %s failed\n", argv[2]);
  exit(1);
}
