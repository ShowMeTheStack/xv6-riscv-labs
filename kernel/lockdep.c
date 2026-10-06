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
