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
#define MAXSLEEP 8  // sleep-locks one process can hold at once
#define NTRACE   5  // return addresses kept per acquisition
#define MAXEDGE  256

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
    __atomic_store_n(&nclass, c + 1, __ATOMIC_RELEASE); // for lockdep_print
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

// The sleep-locks each process holds, by proc[] slot. A process
// can sleep holding them and wake up on another hart, so they
// belong to the process. Only the process changes its own list.
static struct ldheld sheld[NPROC][MAXSLEEP];
static int nsheld[NPROC];
extern struct proc proc[NPROC];

int lockdep_on = 1;
static int busy[NCPU]; // this hart is printing a report

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

// An edge a -> b: a lock of class b was acquired while a lock
// of class a was held. Kept with where both were acquired.
struct ldedge {
  int a, b;
  int hart, pid;      // who did it first
  char name[16];      // and the process's name
  uint64 apc[NTRACE]; // where the held lock was acquired
  uint64 bpc[NTRACE]; // where the new one was
};

static uint64 after[MAXCLASS]; // bit b of after[a]: edge a -> b
static struct ldedge edges[MAXEDGE];
static int nedges;

// Record the edge h's class -> c. Caller holds ldlock.
static void
newedge(struct ldheld *h, int c, uint64 *pc)
{
  struct proc *p = mycpu()->proc;
  struct ldedge *e;

  if (nedges == MAXEDGE)
    panic("lockdep: too many edges");
  e = &edges[nedges++];
  e->a = h->cls;
  e->b = c;
  e->hart = cpuid();
  e->pid = p ? p->pid : 0;
  safestrcpy(e->name, p ? p->name : "", sizeof(e->name));
  memmove(e->apc, h->pc, sizeof(e->apc));
  memmove(e->bpc, pc, sizeof(e->bpc));
  __atomic_or_fetch(&after[h->cls], 1UL << c, __ATOMIC_RELEASE);
}

// Is there a path of edges from class a to class b? Depth-first
// search; on success prev[] holds the path, backwards from b.
// Caller holds ldlock.
static int prev[MAXCLASS];

static int
reach(int a, int b)
{
  int stack[MAXCLASS], n = 0, x, y;
  uint64 seen = 1UL << a;

  if (a == b)
    return 1;
  stack[n++] = a;
  while (n > 0) {
    x = stack[--n];
    for (y = 1; y < nclass; y++) {
      if ((after[x] & (1UL << y)) == 0 || (seen & (1UL << y)) != 0)
        continue;
      seen |= 1UL << y;
      prev[y] = x;
      if (y == b)
        return 1;
      stack[n++] = y;
    }
  }
  return 0;
}

static void
who(int hart, int pid, char *name)
{
  if (pid == 0)
    printk("hart %d, no process:\n", hart);
  else if (name[0] == 0)
    printk("hart %d, pid %d:\n", hart, pid);
  else
    printk("hart %d, pid %d (%s):\n", hart, pid, name);
}

static void
where(char *what, int c, uint64 *pc)
{
  printk("  %s %s at", what, cls[c].name);
  for (int i = 0; i < NTRACE && pc[i]; i++)
    printk(" 0x%lx", pc[i]);
  printk("\n");
}

// Acquiring class c while holding h would close a cycle: the path
// in prev[] already leads from c to h's class. Print both orders.
static void
report(struct ldheld *h, int c, uint64 *pc)
{
  struct proc *p = mycpu()->proc;
  int path[MAXCLASS], n = 0, i, k;
  struct ldedge *e;

  for (k = h->cls; k != c; k = prev[k])
    path[n++] = k;
  path[n++] = c; // path[n-1] -> ... -> path[0] is c -> ... -> h's class

  printk("lockdep: acquiring %s while holding %s may deadlock\n",
         cls[c].name, cls[h->cls].name);
  who(cpuid(), p ? p->pid : 0, p ? p->name : "");
  where("holds", h->cls, h->pc);
  where("wants", c, pc);
  if (n == 1)
    printk("both locks are of class %s\n", cls[c].name);
  else
    printk("but this order has been seen before:\n");
  for (i = n - 1; i > 0; i--) {
    for (e = edges; e->a != path[i] || e->b != path[i - 1]; e++)
      ;
    who(e->hart, e->pid, e->name);
    where("holds", e->a, e->apc);
    where("wants", e->b, e->bpc);
  }
}

// Learn that class c is acquired while h is held. Edges are only
// ever added, so a bit seen set without the lock can be trusted:
// the common case costs one load and no lock. A new edge is
// checked for a cycle before it is added.
static void
order(struct ldheld *h, int c, uint64 *pc)
{
  int id = cpuid();

  if (__atomic_load_n(&after[h->cls], __ATOMIC_RELAXED) & (1UL << c))
    return;
  ldlk();
  if ((after[h->cls] & (1UL << c)) == 0) {
    if (reach(c, h->cls)) {
      busy[id] = 1; // printk() acquires pr.lock: don't check it
      report(h, c, pc);
      printk("lockdep: turning off the checker\n");
      lockdep_on = 0;
      busy[id] = 0;
    } else {
      newedge(h, c, pc);
    }
  }
  ldunlk();
}

// Order class c after every lock this hart holds and, if a
// process is running, every sleep-lock it holds.
static void
orderall(int c, uint64 *pc)
{
  int id = cpuid(), i, s;
  struct proc *p = mycpu()->proc;

  for (i = 0; i < nheld[id] && lockdep_on; i++)
    order(&held[id][i], c, pc);
  if (p) {
    s = p - proc;
    for (i = 0; i < nsheld[s] && lockdep_on; i++)
      order(&sheld[s][i], c, pc);
  }
}

static void
push(struct ldheld *h, void *lk, int c, uint64 *pc)
{
  h->lk = lk;
  h->cls = c;
  memmove(h->pc, pc, sizeof(h->pc));
}

// Called by acquire() before it spins, with interrupts off.
void
lockdep_acquire(struct spinlock *lk)
{
  int id = cpuid();
  uint64 pc[NTRACE];

  if (!lockdep_on || busy[id])
    return;
  if (lk->cls == 0)
    panic("lockdep: lock without a class");
  trace((uint64)__builtin_frame_address(0), pc);
  orderall(lk->cls, pc);
  if (!lockdep_on)
    return;
  if (nheld[id] == MAXHELD)
    panic("lockdep: too many locks held");
  push(&held[id][nheld[id]++], lk, lk->cls, pc);
}

// Called by release(), with interrupts off. The lock was acquired
// on this hart (holding() checked that), but maybe by another
// thread: p->lock crosses swtch(). So search, don't just pop.
void
lockdep_release(struct spinlock *lk)
{
  int id = cpuid(), i;

  if (!lockdep_on || busy[id])
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

// Called by acquiresleep() before it waits.
void
lockdep_acquiresleep(struct sleeplock *lk)
{
  uint64 pc[NTRACE];
  struct proc *p;
  int s;

  push_off(); // stay on this hart while using its list
  p = mycpu()->proc;
  if (lockdep_on && !busy[cpuid()]) {
    if (lk->cls == 0 || p == 0)
      panic("lockdep: acquiresleep");
    trace((uint64)__builtin_frame_address(0), pc);
    orderall(lk->cls, pc);
    s = p - proc;
    if (nsheld[s] == MAXSLEEP)
      panic("lockdep: too many sleep-locks held");
    if (lockdep_on)
      push(&sheld[s][nsheld[s]++], lk, lk->cls, pc);
  }
  pop_off();
}

// Called by releasesleep(). Sleep-locks are released by the
// process that holds them, whatever hart it is on now.
void
lockdep_releasesleep(struct sleeplock *lk)
{
  struct proc *p;
  int s, i;

  push_off();
  p = mycpu()->proc;
  if (lockdep_on && !busy[cpuid()]) {
    if (p == 0)
      panic("lockdep: releasesleep");
    s = p - proc;
    for (i = nsheld[s] - 1; i >= 0; i--)
      if (sheld[s][i].lk == lk)
        break;
    if (i < 0)
      panic("lockdep: release of a sleep-lock this process does not hold");
    for (; i + 1 < nsheld[s]; i++)
      sheld[s][i] = sheld[s][i + 1];
    nsheld[s]--;
  }
  pop_off();
}

// Print the order learned so far (Ctrl-P). Without ldlock:
// printk() acquires pr.lock, and the checker may need ldlock for
// that. Classes and edges are only ever added.
void
lockdep_print(void)
{
  int a, b, n = __atomic_load_n(&nclass, __ATOMIC_ACQUIRE);
  uint64 m;

  for (a = 1; a < n; a++) {
    m = __atomic_load_n(&after[a], __ATOMIC_ACQUIRE);
    if (m == 0)
      continue;
    printk("lockdep: %s ->", cls[a].name);
    for (b = 1; b < n; b++)
      if (m & (1UL << b))
        printk(" %s", cls[b].name);
    printk("\n");
  }
}
