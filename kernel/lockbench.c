// lockbench: several processes acquire and release one
// spinlock in a loop, to measure how fairly it is shared.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "lockbench.h"

static struct spinlock benchlock;

// runlock protects running: callers between the start line
// and the end of their run. reset reinitializes benchlock,
// so it must find running == 0. (a zeroed spinlock is a
// valid free lock, so runlock needs no initlock.)
static struct spinlock runlock;
static int running;

// written with benchlock held, except by reset; read
// without it only by the spin on arrived and after the run.
static struct {
  int total;    // stop after this many acquisitions in all
  int count;    // acquisitions so far
  int arrived;  // callers at the start line
  int inside;   // a hart is in the critical section
  int overlaps; // times a hart found another one inside
  int last;     // hart of the previous acquisition
  int run;      // acquisitions in a row by that hart
  int maxrun;   // longest such run
} bench;

// reset the benchmark, unless a run is in progress.
static int
benchreset(int total)
{
  acquire(&runlock);
  if (running > 0) {
    release(&runlock);
    return -1;
  }
  initlock(&benchlock, "bench");
  bench.total = total;
  bench.count = 0;
  bench.arrived = 0;
  bench.inside = 0;
  bench.overlaps = 0;
  bench.last = -1;
  bench.run = 0;
  bench.maxrun = 0;
  release(&runlock);
  return 0;
}

// the critical section: check that nobody else is
// inside, and keep score.
static void
critical(struct lbstat *st, uint64 w)
{
  int me = cpuid();

  if (__atomic_load_n(&bench.inside, __ATOMIC_RELAXED))
    bench.overlaps++;
  __atomic_store_n(&bench.inside, 1, __ATOMIC_RELAXED);

  if (me == bench.last) {
    bench.run++;
  } else {
    bench.last = me;
    bench.run = 1;
  }
  if (bench.run > bench.maxrun)
    bench.maxrun = bench.run;

  bench.count++;
  st->n++;
  st->hart[me]++;
  st->waited += w;
  if (w > st->maxwait)
    st->maxwait = w;

  __atomic_store_n(&bench.inside, 0, __ATOMIC_RELAXED);
}

// int lockbench(int nproc, int total, struct lbstat *st)
// nproc 0: reset, the run will make total acquisitions;
// fails (-1) while a run is in progress.
// otherwise: wait until nproc callers have arrived, then
// acquire and release benchlock until total is reached.
uint64
sys_lockbench(void)
{
  int nproc, total;
  uint64 addr, start, t0, w;
  struct lbstat st;
  struct proc *p = myproc();

  argint(0, &nproc);
  argint(1, &total);
  argaddr(2, &addr);
  if (nproc <= 0)
    return benchreset(total);

  memset(&st, 0, sizeof(st));
  acquire(&runlock);
  running++;
  release(&runlock);

  // the start line. spin with interrupts on: a caller
  // that has not arrived yet may need this hart.
  acquire(&benchlock);
  bench.arrived++;
  release(&benchlock);
  while (__atomic_load_n(&bench.arrived, __ATOMIC_RELAXED) < nproc)
    ;
  start = r_time();

  for (;;) {
    push_off(); // so that only the lock is timed
    t0 = r_time();
    acquire(&benchlock);
    w = r_time() - t0;
    if (bench.count >= bench.total) {
      release(&benchlock);
      pop_off();
      break;
    }
    critical(&st, w);
    release(&benchlock);
    pop_off();
  }

  st.elapsed = r_time() - start;

  // every acquisition is done; these are final.
  st.count = bench.count;
  st.overlaps = bench.overlaps;
  st.maxrun = bench.maxrun;
  acquire(&runlock);
  running--;
  release(&runlock);
  if (copyout(p->pagetable, p->sz, addr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return 0;
}
