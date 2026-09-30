//
// futexes: sleep and wake on a word of user memory, the
// kernel half of a user-level mutex. the word itself is
// only ever changed by user code, with atomic instructions.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern struct proc proc[NPROC];

// The sleep channel for the futex word at user address addr:
// its physical address, which every thread of the process
// sees the same. 0 if addr is not a good, aligned address.
static void *
futexchan(uint64 addr)
{
  struct proc *p = myproc();
  int x;

  // copyin() also faults in a lazily allocated page.
  if (addr % sizeof(int) != 0 ||
      copyin(p->pagetable, p->vm->sz, (char *)&x, addr, sizeof(x)) < 0)
    return 0;
  return (void *)(walkaddr(p->pagetable, addr) + addr % PGSIZE);
}

// Sleep until futex_wake(addr), if *addr == val.
// Return 0 when woken, or at once if *addr != val.
int
kfutex_wait(uint64 addr, int val)
{
  struct proc *p = myproc();
  void *chan;
  int x, r;

  if ((chan = futexchan(addr)) == 0)
    return -1;

  // register first, then look at the word. a futex_wake()
  // that comes after the look clears p->chan, and sleep()
  // then returns at once: the wakeup cannot be lost.
  sleep_prepare(chan);
  if (copyin(p->pagetable, p->vm->sz, (char *)&x, addr, sizeof(x)) < 0)
    r = -1;
  else if (x != val)
    r = 0;
  else if (killed(p))
    r = -1;
  else {
    sleep();
    return 0;
  }

  // not sleeping after all: unregister, so that a
  // futex_wake() does not count this thread as woken.
  acquire(&p->lock);
  p->chan = 0;
  release(&p->lock);
  return r;
}

// Wake up to n threads sleeping in futex_wait(addr).
// Return how many were woken.
int
kfutex_wake(uint64 addr, int n)
{
  struct proc *p;
  void *chan;
  int woken = 0;

  if ((chan = futexchan(addr)) == 0)
    return -1;

  for (p = proc; p < &proc[NPROC] && woken < n; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // as in wakeup(): a thread still on its way to sleep()
      // sees p->chan == 0 and does not sleep.
      p->chan = 0;
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      woken++;
    }
    release(&p->lock);
  }
  return woken;
}
