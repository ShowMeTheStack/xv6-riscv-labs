#include "kernel/types.h"
#include "user/user.h"

// pause ticks: sleep in the kernel for that many clock ticks
// (a tick is 1/10 s). Handy for trying Ctrl-C and &.
int
main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(2, "usage: pause ticks\n");
    exit(1);
  }
  pause(atoi(argv[1]));
  exit(0);
}
