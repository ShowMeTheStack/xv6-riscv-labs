// A sampling profiler: on each timer interrupt, record
// where the interrupted program was.

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "prof.h"
#include "defs.h"

// profile(lo, hi, shift): start sampling the calling process.
uint64
sys_profile(void)
{
  return -1;
}

// profget(buf): copy the calling process's histogram to buf.
uint64
sys_profget(void)
{
  return -1;
}
