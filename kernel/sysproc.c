#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "cputime.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// fill in a struct cputime at the user address in argument 0.
// the counters are in time-CSR ticks; report microseconds.
uint64
sys_cputime(void)
{
  uint64 addr;
  struct cputime ct;
  struct proc *p = myproc();
  uint64 tpus = TIMEBASE / 1000000; // ticks per microsecond

  argaddr(0, &addr);

  // the clock is running: include this system call so far. a timer
  // interrupt here could yield, and sched() would change stime and
  // tstamp between our reads; so read them with interrupts off.
  push_off();
  uint64 now = r_time();
  ct.stime = (p->stime + (now - p->tstamp)) / tpus;
  pop_off();

  ct.real = now / tpus;
  ct.utime = p->utime / tpus;
  ct.cutime = p->cutime / tpus;
  ct.cstime = p->cstime / tpus;
  if (copyout(p->pagetable, p->sz, addr, (char *)&ct, sizeof(ct)) < 0)
    return -1;
  return 0;
}
