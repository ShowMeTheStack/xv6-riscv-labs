// nice: run a command with a given number of tickets.
//   nice tickets command [args...]
// (Unlike Unix nice, the number is a ticket count: more is faster.)

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 3) {
    fprintf(2, "usage: nice tickets command [args...]\n");
    exit(1);
  }
  if (settickets(atoi(argv[1])) < 0) {
    fprintf(2, "nice: bad ticket count %s\n", argv[1]);
    exit(1);
  }
  exec(argv[2], argv + 2);
  fprintf(2, "nice: exec %s failed\n", argv[2]);
  exit(1);
}
