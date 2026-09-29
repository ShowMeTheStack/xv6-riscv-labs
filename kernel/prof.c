// A sampling profiler: on each timer interrupt, record
// where the interrupted program was.

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "stat.h"
#include "prof.h"
#include "defs.h"

// count one sample: the interrupted program was at pc.
void
profsample(struct prof *pr, uint64 pc)
{
  pr->nsample++;
  if (pc < pr->lo || pc >= pr->hi) {
    pr->nout++;
    return;
  }
  pr->bucket[(pc - pr->lo) >> pr->shift]++;
}

_Static_assert(sizeof(struct prof) == PGSIZE, "struct prof must fill a page");

// profile(lo, hi, shift): start sampling the calling process
// into a new, empty histogram whose buckets cover [lo, hi),
// 1 << shift bytes each. profile(0, 0, 0) stops sampling.
// either way, the old histogram is discarded.
uint64
sys_profile(void)
{
  uint64 lo, hi;
  int shift;
  struct prof *pr = 0;
  struct proc *p = myproc();

  argaddr(0, &lo);
  argaddr(1, &hi);
  argint(2, &shift);

  if (lo != 0 || hi != 0) {
    if (lo >= hi || shift < 0 || shift > 31 ||
        ((hi - lo - 1) >> shift) >= PROFBUCKETS)
      return -1;
    if ((pr = (struct prof *)kalloc()) == 0)
      return -1;
    memset(pr, 0, PGSIZE);
    pr->lo = lo;
    pr->hi = hi;
    pr->shift = shift;
    pr->nbucket = ((hi - lo - 1) >> shift) + 1;
  }

  if (p->prof)
    kfree((void *)p->prof);
  p->prof = pr;
  return 0;
}

// profget(buf): copy the calling process's histogram,
// a struct prof, to buf.
uint64
sys_profget(void)
{
  uint64 buf;
  struct proc *p = myproc();

  argaddr(0, &buf);
  if (p->prof == 0)
    return -1;
  return copyout(p->pagetable, p->sz, buf, (char *)p->prof,
                 sizeof(struct prof));
}

// called by kexit: write the histogram to the file prof.out
// in the process's current directory.
void
profdump(struct proc *p)
{
  struct inode *ip;

  begin_op();
  if ((ip = create("prof.out", T_FILE, 0, 0)) != 0) {
    writei(ip, 0, (uint64)p->prof, 0, sizeof(struct prof));
    iunlockput(ip);
  }
  end_op();
}
