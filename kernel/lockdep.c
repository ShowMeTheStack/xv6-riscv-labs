// Lockdep-lite: a run-time lock-order checker.
//
// Every lock belongs to a class: the name it was given in
// initlock() or initsleeplock(). All 64 p->locks are one class,
// "proc"; every pipe's lock is "pipe". The checker learns, while
// the kernel runs, which classes are acquired while which others
// are held, and complains about the first acquisition that would
// make that order inconsistent, before any deadlock happens.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"

#define MAXCLASS 64 // lock classes; class 0 means "none"
#define MAXHELD  8  // spinlocks one hart can hold at once
#define NTRACE   5  // return addresses kept per acquisition

struct ldclass {
  char *name;
  int sleep; // 1 for a sleep-lock class
};

static struct ldclass cls[MAXCLASS];
static int nclass = 1;

// The checker's own lock. It cannot be a struct spinlock:
// acquire() calls the checker, which would call acquire()
// again, forever. Callers must have interrupts off, so that
// an interrupt handler on this hart cannot spin on it while
// it is held underneath.
static uint ldlock;

static void
ldlk(void)
{
  while (__atomic_exchange_n(&ldlock, 1, __ATOMIC_ACQUIRE) != 0)
    ;
}

static void
ldunlk(void)
{
  __atomic_store_n(&ldlock, 0, __ATOMIC_RELEASE);
}

// Return the class for locks named name (sleep: a sleep-lock),
// making a new one the first time the name is seen.
int
lockdep_class(char *name, int sleep)
{
  int c;

  push_off();
  ldlk();
  for (c = 1; c < nclass; c++)
    if (cls[c].sleep == sleep && strncmp(cls[c].name, name, 32) == 0)
      break;
  if (c == nclass) {
    if (nclass == MAXCLASS)
      panic("lockdep: too many classes");
    cls[c].name = name;
    cls[c].sleep = sleep;
    nclass++;
  }
  ldunlk();
  pop_off();
  return c;
}

// One acquisition: which lock, its class, and where it happened.
struct ldheld {
  void *lk;
  int cls;
  uint64 pc[NTRACE];
};

// The spinlocks each hart holds, oldest first. Per hart, not per
// process: the scheduler acquires a p->lock and the process it
// switches to releases it, on the same hart.
static struct ldheld held[NCPU][MAXHELD];
static int nheld[NCPU];

int lockdep_on = 1;

// Fill pc[] with return addresses: the caller of acquire() and
// the callers above it. fp is the checker's own frame pointer
// (s0). In each frame, ra is saved at fp-8 and the caller's fp at
// fp-16. Stop at the edge of the stack page.
static void
trace(uint64 fp, uint64 *pc)
{
  uint64 lo = PGROUNDDOWN(fp), hi = lo + PGSIZE;
  int i = 0;

  fp = *(uint64 *)(fp - 16); // the frame of acquire() itself
  while (i < NTRACE && fp >= lo + 16 && fp <= hi) {
    pc[i++] = *(uint64 *)(fp - 8);
    fp = *(uint64 *)(fp - 16);
  }
  while (i < NTRACE)
    pc[i++] = 0;
}

// Called by acquire() before it spins, with interrupts off.
void
lockdep_acquire(struct spinlock *lk)
{
  int id = cpuid();
  struct ldheld *h;

  if (!lockdep_on)
    return;
  if (lk->cls == 0)
    panic("lockdep: lock without a class");
  if (nheld[id] == MAXHELD)
    panic("lockdep: too many locks held");
  h = &held[id][nheld[id]++];
  h->lk = lk;
  h->cls = lk->cls;
  trace((uint64)__builtin_frame_address(0), h->pc);
}

// Called by release(), with interrupts off. The lock was acquired
// on this hart (holding() checked that), but maybe by another
// thread: p->lock crosses swtch(). So search, don't just pop.
void
lockdep_release(struct spinlock *lk)
{
  int id = cpuid(), i;

  if (!lockdep_on)
    return;
  for (i = nheld[id] - 1; i >= 0; i--)
    if (held[id][i].lk == lk)
      break;
  if (i < 0)
    panic("lockdep: release of a lock this hart does not hold");
  for (; i + 1 < nheld[id]; i++)
    held[id][i] = held[id][i + 1];
  nheld[id]--;
}
