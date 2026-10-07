// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
  lk->cls = lockdep_class(name, 1);
}

void
acquiresleep(struct sleeplock *lk)
{
  acquiresleep_nested(lk, 0);
}

// Acquire lk, maybe while holding another lock of its class.
// sub names lk's role there (see sleeplock.h). The checker gives
// each role a class of its own, so "a directory, then a file in
// it" is an order it can check, not inode nested in inode.
void
acquiresleep_nested(struct sleeplock *lk, int sub)
{
  lockdep_acquiresleep(lk, sub); // check the order before waiting
  acquire(&lk->lk);
  while (lk->locked) {
    sleep_prepare(lk);
    release(&lk->lk);
    sleep();
    acquire(&lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  lockdep_releasesleep(lk);
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;

  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}
