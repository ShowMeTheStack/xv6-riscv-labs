// A sampling profiler: on each timer interrupt, record
// where the interrupted program was.
//
// a user profile (lo < KERNBASE) belongs to one process and is
// fed by usertrap, on the hart running that process, so it
// needs no lock. a kernel profile (lo >= KERNBASE) is fed by
// kerneltrap on every hart, so it has a lock, and there is at
// most one at a time.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "stat.h"
#include "prof.h"
#include "defs.h"

struct spinlock kproflock;
struct prof *kprof; // the kernel profile, or 0; kproflock

void
profinit(void)
{
  initlock(&kproflock, "kprof");
}

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

// called by kerneltrap for each timer interrupt, on every hart:
// the kernel was interrupted at pc.
void
kprofsample(uint64 pc)
{
  acquire(&kproflock);
  if (kprof)
    profsample(kprof, pc);
  release(&kproflock);
}

// the process's histogram will no longer be fed:
// make sure no hart is still adding to it.
static void
profdetach(struct proc *p)
{
  acquire(&kproflock);
  if (kprof == p->prof)
    kprof = 0;
  release(&kproflock);
}

_Static_assert(sizeof(struct prof) == PGSIZE, "struct prof must fill a page");

// profile(lo, hi, shift): start sampling into a new, empty
// histogram whose buckets cover [lo, hi), 1 << shift bytes
// each: the calling process's user pcs, or, if lo >= KERNBASE,
// the kernel's pcs on all harts. profile(0, 0, 0) stops
// sampling. either way, the old histogram is discarded.
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

  if (p->prof) {
    profdetach(p);
    kfree((void *)p->prof);
    p->prof = 0;
  }

  if (pr && pr->lo >= KERNBASE) {
    acquire(&kproflock);
    if (kprof) {
      // another process is profiling the kernel.
      release(&kproflock);
      kfree((void *)pr);
      return -1;
    }
    kprof = pr;
    release(&kproflock);
  }
  p->prof = pr;
  return 0;
}

// profget(buf): copy the calling process's histogram,
// a struct prof, to buf.
uint64
sys_profget(void)
{
  uint64 buf;
  int r;
  struct proc *p = myproc();

  argaddr(0, &buf);
  if (p->prof == 0)
    return -1;
  // other harts may be adding to a kernel profile.
  acquire(&kproflock);
  r = copyout(p->pagetable, p->sz, buf, (char *)p->prof,
              sizeof(struct prof));
  release(&kproflock);
  return r;
}

// called by kexit: write the histogram to the file prof.out
// in the process's current directory.
void
profdump(struct proc *p)
{
  struct inode *ip;

  profdetach(p);
  begin_op();
  if ((ip = create("prof.out", T_FILE, 0, 0)) != 0) {
    writei(ip, 0, (uint64)p->prof, 0, sizeof(struct prof));
    iunlockput(ip);
  }
  end_op();
}
