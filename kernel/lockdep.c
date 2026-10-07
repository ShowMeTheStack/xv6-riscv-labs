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
#include "lockdep.h"

#define MAXCLASS 64 // lock classes; class 0 means "none"
#define MAXHELD  8  // spinlocks one hart can hold at once
#define MAXSLEEP 8  // sleep-locks one process can hold at once
#define NTRACE   5  // return addresses kept per acquisition
#define NSUB     4  // subclasses per class (acquiresleep_nested)
#define MAXEDGE  256

#define USE_IRQ   1 // taken in an interrupt handler
#define USE_IRQON 2 // held with interrupts on

struct ldclass {
  char *name;
  int sleep;                // 1 for a sleep-lock class
  int use;                  // USE_IRQ, USE_IRQON
  uint64 usepc[2][NTRACE];  // where each use was first seen
  int sub[NSUB];            // the class of each subclass, if made
  char subname[24];         // "inode/1", for a subclass
  int test;                 // a self-test class (see selftest)
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

// Return the class for role sub of class c (sub 0 is c itself):
// a class of its own, so that the order between roles can be
// learned and checked like any other order.
static int
subclass(int c, int sub)
{
  int s, n;

  if (sub == 0)
    return c;
  if (sub < 0 || sub >= NSUB)
    panic("lockdep: bad subclass");
  if ((s = __atomic_load_n(&cls[c].sub[sub], __ATOMIC_ACQUIRE)) != 0)
    return s;
  ldlk();
  if ((s = cls[c].sub[sub]) == 0) {
    if (nclass == MAXCLASS)
      panic("lockdep: too many classes");
    s = nclass;
    n = strlen(safestrcpy(cls[s].subname, cls[c].name, 20));
    cls[s].subname[n] = '/';
    cls[s].subname[n + 1] = '0' + sub;
    cls[s].name = cls[s].subname;
    cls[s].sleep = cls[c].sleep;
    cls[s].test = cls[c].test;
    __atomic_store_n(&nclass, s + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&cls[c].sub[sub], s, __ATOMIC_RELEASE);
  }
  ldunlk();
  return s;
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
static int busy[NCPU];  // this hart is printing a report
static int inirq[NCPU]; // this hart is running an interrupt handler
static uint64 nchecks[NCPU];      // acquisitions checked, per hart
static uint64 nslow;              // checks that took ldlock
static int nreports, ntestreports;

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

// After a report, printed by this hart with ldlock held: panic,
// or (LOCKDEP_PANIC 0) carry on without the checker. Once the
// graph has a cycle, more reports would mostly be echoes of it.
static void
stop(void)
{
  nreports++;
  if (LOCKDEP_PANIC) {
    ldunlk();
    panic("lockdep");
  }
  printk("lockdep: turning off the checker\n");
  lockdep_on = 0;
  busy[cpuid()] = 0;
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
  nslow++;
  if ((after[h->cls] & (1UL << c)) == 0) {
    if (reach(c, h->cls)) {
      busy[id] = 1; // printk() acquires pr.lock: don't check it
      report(h, c, pc);
      if (cls[c].test) { // a self-test: count it and go on
        ntestreports++;
        busy[id] = 0;
      } else {
        stop();
      }
    } else {
      newedge(h, c, pc);
    }
  }
  ldunlk();
}

// Order class c after every lock this hart holds and, if a
// process is running, every sleep-lock it holds. Not in an
// interrupt handler: it runs on top of whatever process it
// interrupted, but does not act for it.
static void
orderall(int c, uint64 *pc)
{
  int id = cpuid(), i, s;
  struct proc *p = mycpu()->proc;

  for (i = 0; i < nheld[id] && lockdep_on; i++)
    order(&held[id][i], c, pc);
  if (p && inirq[id] == 0) {
    s = p - proc;
    for (i = 0; i < nsheld[s] && lockdep_on; i++)
      order(&sheld[s][i], c, pc);
  }
}

// Class c is used in a way how. A lock that is taken in an
// interrupt handler (USE_IRQ) must never be held with interrupts
// on (USE_IRQON): the handler could interrupt the holder on its
// own hart and spin forever on the lock underneath it.
static void
use(int c, int how, uint64 *pc)
{
  int id = cpuid();

  if (cls[c].use & how)
    return;
  ldlk();
  if ((cls[c].use & how) == 0) {
    cls[c].use |= how;
    memmove(cls[c].usepc[how - 1], pc, sizeof(cls[c].usepc[0]));
    if (cls[c].use == (USE_IRQ | USE_IRQON)) {
      busy[id] = 1;
      printk("lockdep: %s is taken in interrupt handlers and held "
             "with interrupts on\n", cls[c].name);
      where("in a handler:", c, cls[c].usepc[USE_IRQ - 1]);
      where("interrupts on:", c, cls[c].usepc[USE_IRQON - 1]);
      stop();
    }
  }
  ldunlk();
}

static void
push(struct ldheld *h, void *lk, int c, uint64 *pc)
{
  h->lk = lk;
  h->cls = c;
  memmove(h->pc, pc, sizeof(h->pc));
}

// Called by acquire() before it spins, with interrupts off, or
// by tryacquire() (try = 1) once it has the lock. A lock that is
// never waited for cannot be one of the waits in a deadlock, so a
// try records no order: it is only added to the held list.
void
lockdep_acquire(struct spinlock *lk, int try)
{
  int id = cpuid();
  uint64 pc[NTRACE];

  if (!lockdep_on || busy[id])
    return;
  if (lk->cls == 0)
    panic("lockdep: lock without a class");
  nchecks[id]++;
  trace((uint64)__builtin_frame_address(0), pc);
  if (inirq[id])
    use(lk->cls, USE_IRQ, pc);
  if (intr_get()) // never: acquire() turned them off first
    use(lk->cls, USE_IRQON, pc);
  if (!try)
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

// Called by acquiresleep_nested() before it waits, or by
// tryacquiresleep() (try = 1) once it has the lock.
void
lockdep_acquiresleep(struct sleeplock *lk, int sub, int try)
{
  uint64 pc[NTRACE];
  struct proc *p;
  int s, c;

  push_off(); // stay on this hart while using its list
  p = mycpu()->proc;
  if (lockdep_on && !busy[cpuid()]) {
    if (lk->cls == 0 || p == 0)
      panic("lockdep: acquiresleep");
    c = subclass(lk->cls, sub);
    nchecks[cpuid()]++;
    trace((uint64)__builtin_frame_address(0), pc);
    // a sleep-lock's holder runs with interrupts on, and sleeps
    use(c, inirq[cpuid()] ? USE_IRQ : USE_IRQON, pc);
    if (!try)
      orderall(c, pc);
    s = p - proc;
    if (nsheld[s] == MAXSLEEP)
      panic("lockdep: too many sleep-locks held");
    if (lockdep_on)
      push(&sheld[s][nsheld[s]++], lk, c, pc);
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

// devintr() calls these around each interrupt handler, with
// interrupts off.
void
lockdep_irq_enter(void)
{
  inirq[cpuid()]++;
}

void
lockdep_irq_exit(void)
{
  inirq[cpuid()]--;
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

// Self-tests: lock orders, right and wrong, on locks of their own,
// whose classes are marked "test". A report about a test class is
// printed and counted, but neither panics nor stops the checker.
static struct spinlock ta, tb, tc, ts1, ts2;
static struct sleeplock tsa, tsb;
static uint testbusy;

// Forget every order that involves a test class.
static void
forget(void)
{
  int a, b, i, j;

  ldlk();
  for (a = 1; a < nclass; a++)
    for (b = 1; b < nclass; b++)
      if (cls[a].test || cls[b].test)
        after[a] &= ~(1UL << b);
  for (i = j = 0; i < nedges; i++)
    if (!cls[edges[i].a].test && !cls[edges[i].b].test)
      edges[j++] = edges[i];
  nedges = j;
  ldunlk();
}

// Acquire a, then b inside it; release both.
static void
nest(struct spinlock *a, struct spinlock *b)
{
  acquire(a);
  acquire(b);
  release(b);
  release(a);
}

// The same for sleep-locks, each in a role.
static void
nestsleep(struct sleeplock *a, int asub, struct sleeplock *b, int bsub)
{
  acquiresleep_nested(a, asub);
  acquiresleep_nested(b, bsub);
  releasesleep(b);
  releasesleep(a);
}

static int
selftest(int n)
{
  int before;

  if (ta.cls == 0) {
    initlock(&ta, "test A");
    initlock(&tb, "test B");
    initlock(&tc, "test C");
    initlock(&ts1, "test S");
    initlock(&ts2, "test S");
    initsleeplock(&tsa, "test T");
    initsleeplock(&tsb, "test T");
    cls[ta.cls].test = cls[tb.cls].test = cls[tc.cls].test = 1;
    cls[ts1.cls].test = cls[tsa.cls].test = 1;
  }
  push_off();
  forget();
  pop_off();
  before = ntestreports;
  switch (n) {
  case 1: // A then B; later B then A
    nest(&ta, &tb);
    nest(&tb, &ta);
    break;
  case 2: // A then B, B then C; later C then A
    nest(&ta, &tb);
    nest(&tb, &tc);
    nest(&tc, &ta);
    break;
  case 3: // two locks of one class, one inside the other
    nest(&ts1, &ts2);
    break;
  case 4: // the same with sleep-locks, the inner one as SUB_CHILD;
          // later the same two locks the other way round
    nestsleep(&tsa, 0, &tsb, SUB_CHILD);
    nestsleep(&tsb, 0, &tsa, SUB_CHILD);
    break;
  case 5: // role 0 then SUB_CHILD; later SUB_CHILD then role 0
    nestsleep(&tsa, 0, &tsb, SUB_CHILD);
    nestsleep(&tsb, SUB_CHILD, &tsa, 0);
    break;
  case 6: // A then B; later B then a trylock of A
    nest(&ta, &tb);
    acquire(&tb);
    if (tryacquire(&ta))
      release(&ta);
    release(&tb);
    break;
  default:
    return -1;
  }
  return ntestreports - before;
}

// lockdep(0, st): copy the checker's statistics to st.
// lockdep(n, 0): run self-test n and return how many reports it
// caused; -1 if there is no such test, the checker is off, or
// another self-test is running.
uint64
sys_lockdep(void)
{
  int cmd, i, r;
  uint64 addr;
  struct ldstat st;

  argint(0, &cmd);
  argaddr(1, &addr);
  if (cmd == 0) {
    memset(&st, 0, sizeof(st));
    push_off();
    ldlk();
    st.on = lockdep_on;
    for (i = 1; i < nclass; i++)
      st.classes += !cls[i].test;
    for (i = 0; i < nedges; i++)
      st.edges += !cls[edges[i].a].test && !cls[edges[i].b].test;
    st.reports = nreports;
    for (i = 0; i < NCPU; i++)
      st.checks += nchecks[i];
    st.slow = nslow;
    ldunlk();
    pop_off();
    return copyout(myproc()->pagetable, myproc()->sz, addr, (char *)&st,
                   sizeof(st));
  }
  if (!lockdep_on || __atomic_exchange_n(&testbusy, 1, __ATOMIC_ACQUIRE))
    return -1;
  r = selftest(cmd);
  __atomic_store_n(&testbusy, 0, __ATOMIC_RELEASE);
  return r;
}
