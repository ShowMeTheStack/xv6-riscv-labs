// User threads on clone() and join(), and a mutex on
// futex_wait() and futex_wake().

#include "kernel/types.h"
#include "user/user.h"

#define TSTACK 8192 // bytes of stack for each thread
#define MAXT   8    // the kernel's NTHREAD

// what a new thread finds at the top of its stack.
struct tstart {
  int (*fn)(void *);
  void *arg;
};

// thread stacks, reused after join. no guard pages.
static struct {
  int tid; // the thread using it, or 0 if free
  char *stack;
} stacks[MAXT];
static struct mutex stacklock;

// every thread starts here, on its new stack: call the
// thread's function and exit with what it returns.
static void
start(void *a)
{
  struct tstart *s = a;
  exit(s->fn(s->arg));
}

// Start fn(arg) in a new thread. Return its id, or -1.
int
thread_create(int (*fn)(void *), void *arg)
{
  struct tstart *s;
  int i, tid = -1;

  mutex_lock(&stacklock);
  for (i = 0; i < MAXT; i++)
    if (stacks[i].tid == 0)
      break;
  if (i < MAXT && stacks[i].stack == 0) {
    stacks[i].stack = sbrk(TSTACK); // sbrk is thread-safe
    if (stacks[i].stack == SBRK_ERROR)
      stacks[i].stack = 0;
  }
  if (i < MAXT && stacks[i].stack != 0) {
    // the top 16 bytes hold fn and arg; sp starts below them.
    s = (struct tstart *)(stacks[i].stack + TSTACK) - 1;
    s->fn = fn;
    s->arg = arg;
    tid = clone(start, s, s);
    if (tid > 0)
      stacks[i].tid = tid;
  }
  mutex_unlock(&stacklock);
  return tid;
}

// Wait for a thread this thread created to exit. Return its
// id, and its exit status in *status, or -1 if there is none.
int
thread_join(int *status)
{
  int i, tid;

  tid = join(status);
  if (tid > 0) {
    mutex_lock(&stacklock);
    for (i = 0; i < MAXT; i++)
      if (stacks[i].tid == tid)
        stacks[i].tid = 0;
    mutex_unlock(&stacklock);
  }
  return tid;
}

// m->v is 0 when the mutex is free, 1 when it is held,
// and 2 when it is held and threads may be asleep on it.
void
mutex_lock(struct mutex *m)
{
  int c = 0;

  // free: take it with one atomic instruction, no system call.
  if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE,
                                  __ATOMIC_RELAXED))
    return;

  // held: mark it 2 ("someone waits") and sleep until it is
  // free. the exchange both takes the mutex, if it got free,
  // and makes sure the holder's unlock will wake us.
  if (c != 2)
    c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    futex_wait(&m->v, 2);
    c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
  }
}

void
mutex_unlock(struct mutex *m)
{
  // nobody waits (1): done. maybe somebody (2): wake one.
  if (__atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE) == 2)
    futex_wake(&m->v, 1);
}
