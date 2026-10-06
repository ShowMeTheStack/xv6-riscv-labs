// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// The ticket counters start 256 below the point where they
// wrap around to 0, so every lock wraps soon after boot, and a
// comparison that cannot cope with the wrap fails at once,
// not after 2^32 acquisitions.
#define TICKET0 0xffffff00

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->next = TICKET0;
  lk->serving = TICKET0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  uint my;

  push_off(); // disable interrupts to avoid deadlock.
  if (holding(lk))
    panic("acquire");

  // Take a ticket. On RISC-V, __atomic_fetch_add turns into
  // one atomic add that returns the old value:
  //   a5 = 1
  //   s1 = &lk->next
  //   amoadd.w a4, a5, (s1)
  // so every caller gets a different number, even if
  // several harts ask at the same moment.
  my = __atomic_fetch_add(&lk->next, 1, __ATOMIC_RELAXED);

  // Wait for our turn. __ATOMIC_ACQUIRE keeps the critical
  // section's loads and stores after the load that sees our
  // number; on RISC-V it is a load and then a fence:
  //   lw a5, 0(a5)
  //   fence r,rw
  // Compare with ==, so that the counters may wrap.
  while (__atomic_load_n(&lk->serving, __ATOMIC_ACQUIRE) != my)
    ;

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Serve the next ticket. Only the holder writes serving,
  // so a plain load and a single store are enough; the
  // __ATOMIC_RELEASE store makes the critical section's
  // loads and stores visible to other CPUs before it:
  //   a4 = &lk->serving
  //   fence rw,w
  //   sw a5, 0(a4)
  __atomic_store_n(&lk->serving, lk->serving + 1, __ATOMIC_RELEASE);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->next != lk->serving && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  uint64 flags = rc_sstatus(SSTATUS_SIE);
  int old = !!(flags & SSTATUS_SIE);

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}
