#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

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

// clone(fn, arg, stack): a new thread that shares this
// address space and starts at fn(arg) with sp = stack.
uint64
sys_clone(void)
{
  uint64 fn, arg, stack;
  argaddr(0, &fn);
  argaddr(1, &arg);
  argaddr(2, &stack);
  return kclone(fn, arg, stack);
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p, 0);
}

// join(&status): wait for a thread this thread created.
uint64
sys_join(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p, 1);
}

// futex_wait(addr, val): sleep if *addr == val.
uint64
sys_futex_wait(void)
{
  uint64 addr;
  int val;
  argaddr(0, &addr);
  argint(1, &val);
  return kfutex_wait(addr, val);
}

// futex_wake(addr, n): wake up to n futex_wait(addr)ers.
uint64
sys_futex_wake(void)
{
  uint64 addr;
  int n;
  argaddr(0, &addr);
  argint(1, &n);
  return kfutex_wake(addr, n);
}

uint64
sys_sbrk(void)
{
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);

  // reading the old size and setting the new one must be one
  // step, or two threads could both get the same memory.
  return growproc(n, t == SBRK_EAGER || n < 0);
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
