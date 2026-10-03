//
// mmap and munmap: mappings of files into a process's address
// space. mmap only records a mapping in p->vma[]; its pages are
// read from the file when they are first touched.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// The mapping of p that contains va, or 0.
struct vma *
vmalookup(struct proc *p, uint64 va)
{
  struct vma *v;

  for (v = p->vma; v < &p->vma[NVMA]; v++)
    if (v->len != 0 && va >= v->start && va < v->start + v->len)
      return v;
  return 0;
}

// Choose where a new mapping of len bytes goes: the highest free
// range below TRAPFRAME, and never below the heap. Returns its
// start, or 0 if there is no room.
static uint64
vmaplace(struct proc *p, uint64 len)
{
  uint64 end = TRAPFRAME, a;
  struct vma *v;

  for (;;) {
    if (len > end || end - len < PGROUNDUP(p->sz))
      return 0;
    a = end - len;
    for (v = p->vma; v < &p->vma[NVMA]; v++)
      if (v->len != 0 && v->start < end && a < v->start + v->len)
        break;
    if (v == &p->vma[NVMA])
      return a; // [a, end) overlaps no mapping
    end = v->start; // try again just below the mapping in the way
  }
}

// void *mmap(void *addr, uint64 len, int prot, int flags, int fd,
//            uint64 off)
// addr must be 0: the kernel chooses the address.
uint64
sys_mmap(void)
{
  uint64 addr, len, off, start;
  int prot, flags, fd;
  struct proc *p = myproc();
  struct file *f;
  struct vma *v;

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &off);

  if (addr != 0 || len == 0 || len > TRAPFRAME || off % PGSIZE != 0 ||
      off + len < off)
    return -1;
  if (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
    return -1;
  if (flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  if (fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return -1;
  if (f->type != FD_INODE || !f->readable)
    return -1; // pages are read from an inode: no pipes, no devices
  if (flags == MAP_SHARED && (prot & PROT_WRITE) && !f->writable)
    return -1; // stores would reach a file opened read-only

  for (v = p->vma; v < &p->vma[NVMA]; v++)
    if (v->len == 0)
      break;
  if (v == &p->vma[NVMA])
    return -1; // no free slot
  len = PGROUNDUP(len);
  if ((start = vmaplace(p, len)) == 0)
    return -1;

  v->start = start;
  v->len = len;
  v->prot = prot;
  v->flags = flags;
  v->f = filedup(f); // the mapping outlives close(fd)
  v->off = off;
  return start;
}
