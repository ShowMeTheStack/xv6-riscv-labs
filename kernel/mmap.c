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

// The lowest address of any mapping of p, or TRAPFRAME if it has
// none. The heap must stay below it.
uint64
vmabottom(struct proc *p)
{
  uint64 bottom = TRAPFRAME;
  struct vma *v;

  for (v = p->vma; v < &p->vma[NVMA]; v++)
    if (v->len != 0 && v->start < bottom)
      bottom = v->start;
  return bottom;
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

// Load the page at va of mapping v from the file and map it.
// Called by vmfault. Locks the file's inode and may wait for the
// disk, so the caller must hold no spinlock, and must not hold
// that inode's lock. Returns the page's physical address, or 0 if
// the access is not allowed, the page is already mapped, or
// memory is short.
uint64
mmapfault(struct proc *p, struct vma *v, uint64 va, int read)
{
  struct inode *ip = v->f->ip;
  uint64 off;
  uint n;
  char *mem;
  int perm = PTE_R | PTE_U;

  if (!read && (v->prot & PROT_WRITE) == 0)
    return 0; // a store to a read-only mapping
  va = PGROUNDDOWN(va);
  if (ismapped(p->pagetable, va))
    return 0; // mapped, so the fault was about permissions

  if ((mem = kalloc()) == 0)
    return 0;
  memset(mem, 0, PGSIZE); // past the end of the file: zeros
  off = v->off + (va - v->start);
  ilock(ip);
  if (off < ip->size) {
    n = PGSIZE;
    if (ip->size - off < PGSIZE)
      n = ip->size - off;
    if (readi(ip, 0, (uint64)mem, off, n) != n) {
      iunlock(ip);
      kfree(mem);
      return 0;
    }
  }
  iunlock(ip);

  if (v->prot & PROT_WRITE)
    perm |= PTE_W;
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return 0;
  }
  return (uint64)mem;
}

// Load the pages of p's mappings in [va, va+n) that are not loaded
// yet. A system call that copies to or from user memory while it
// holds locks calls this first, holding none: loading a mapped page
// sleeps, and locks the mapped file's inode. Returns -1 if a page
// can't be loaded.
int
vmaprefault(struct proc *p, uint64 va, uint64 n)
{
  uint64 a, lo, hi;
  struct vma *v;

  if (va + n < va)
    return -1;
  for (v = p->vma; v < &p->vma[NVMA]; v++) {
    if (v->len == 0)
      continue;
    lo = PGROUNDDOWN(va) > v->start ? PGROUNDDOWN(va) : v->start;
    hi = va + n < v->start + v->len ? va + n : v->start + v->len;
    for (a = lo; a < hi; a += PGSIZE)
      if (!ismapped(p->pagetable, a) && mmapfault(p, v, a, 1) == 0)
        return -1;
  }
  return 0;
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

// Write the page at va of shared mapping v, whose contents are at
// physical address pa, back to the file. Only the part inside the
// file is written: a mapping never makes its file longer.
static void
vmawrite(struct vma *v, uint64 va, uint64 pa)
{
  struct inode *ip = v->f->ip;
  uint64 off = v->off + (va - v->start);
  uint n;

  // one page is 4 blocks, all already in the file, plus the
  // inode: 5 log blocks, within the MAXOPBLOCKS of one transaction.
  begin_op();
  ilock(ip);
  if (off < ip->size) {
    n = PGSIZE;
    if (ip->size - off < PGSIZE)
      n = ip->size - off;
    writei(ip, 0, pa, off, n);
  }
  iunlock(ip);
  end_op();
}

// Remove [addr, addr+len) from mapping v, in the address space of
// pagetable: write back the dirty pages of a shared mapping, free
// the loaded pages and shrink the record. addr and len are
// page-aligned, and the range is the whole of v, or a part of it
// that starts at its start or ends at its end. The last page gone
// drops the mapping's reference to the file.
void
vmaunmap(pagetable_t pagetable, struct vma *v, uint64 addr, uint64 len)
{
  uint64 a;
  pte_t *pte;

  if (v->flags == MAP_SHARED && (v->prot & PROT_WRITE)) {
    for (a = addr; a < addr + len; a += PGSIZE) {
      pte = walk(pagetable, a, 0);
      if (pte && (*pte & PTE_V) && (*pte & PTE_D))
        vmawrite(v, a, PTE2PA(*pte)); // written since it was loaded
    }
  }
  uvmunmap(pagetable, addr, len / PGSIZE, 1);
  if (addr == v->start) {
    v->start += len;
    v->off += len;
  }
  v->len -= len;
  if (v->len == 0) {
    fileclose(v->f);
    v->f = 0;
  }
}

// Remove all of p's mappings from pagetable: at exit, and in exec
// for the old image. Their pages lie above p->sz, where uvmfree
// does not look, and freewalk panics on any page still mapped.
void
vmaunmapall(struct proc *p, pagetable_t pagetable)
{
  struct vma *v;

  for (v = p->vma; v < &p->vma[NVMA]; v++)
    if (v->len != 0)
      vmaunmap(pagetable, v, v->start, v->len);
}

// int munmap(void *addr, uint64 len)
// Unmaps a whole mapping, or a part at its start or at its end.
uint64
sys_munmap(void)
{
  uint64 addr, len;
  struct proc *p = myproc();
  struct vma *v;

  argaddr(0, &addr);
  argaddr(1, &len);
  if (addr % PGSIZE != 0 || len == 0 || len > TRAPFRAME)
    return -1;
  len = PGROUNDUP(len);
  if ((v = vmalookup(p, addr)) == 0)
    return -1;
  if (addr + len > v->start + v->len)
    return -1; // runs past the end of the mapping
  if (addr != v->start && addr + len != v->start + v->len)
    return -1; // would leave a hole in the middle
  vmaunmap(p->pagetable, v, addr, len);
  return 0;
}
