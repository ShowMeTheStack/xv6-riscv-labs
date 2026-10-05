// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// one free list per CPU, each with its own lock.
struct kmem {
  struct spinlock lock;
  struct run *freelist;
  char name[8]; // "kmem0", "kmem1", ...
};

struct kmem kmem[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    safestrcpy(kmem[i].name, "kmem0", sizeof(kmem[i].name));
    kmem[i].name[4] += i;
    initlock(&kmem[i].lock, kmem[i].name);
  }
  // kinit runs on the boot CPU, so kfree puts every page on
  // that CPU's list; the others take from it when they need to.
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int id;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  // put the page on this CPU's list. interrupts stay off
  // from cpuid() to release, so the thread cannot move to
  // another CPU in between.
  push_off();
  id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r = 0;
  int id;

  push_off();
  id = cpuid();
  // this CPU's list first, then the other CPUs' lists.
  // only one kmem lock is held at a time.
  for (int i = 0; i < NCPU && r == 0; i++) {
    struct kmem *km = &kmem[(id + i) % NCPU];
    acquire(&km->lock);
    r = km->freelist;
    if (r)
      km->freelist = r->next;
    release(&km->lock);
  }
  pop_off();

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
