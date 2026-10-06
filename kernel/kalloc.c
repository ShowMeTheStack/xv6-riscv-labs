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

#define STEAL 64 // most pages taken from another CPU's list at once

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// one free list per CPU, each with its own lock.
struct kmem {
  struct spinlock lock;
  struct run *freelist;
  int nfree;    // pages on freelist
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
  kmem[id].nfree++;
  release(&kmem[id].lock);
  pop_off();
}

// Take up to STEAL pages from another CPU's list: return the
// first, and put the rest on CPU id's list. Holds only one
// kmem lock at a time, so two stealing CPUs cannot deadlock.
// Caller must have interrupts off.
static struct run *
steal(int id)
{
  struct run *first, *last;
  int n;

  for (int i = 1; i < NCPU; i++) {
    struct kmem *km = &kmem[(id + i) % NCPU];
    acquire(&km->lock);
    first = last = km->freelist;
    n = 0;
    if (first) {
      // cut the first STEAL pages (or all) off km's list.
      for (n = 1; n < STEAL && last->next; n++)
        last = last->next;
      km->freelist = last->next;
      km->nfree -= n;
    }
    release(&km->lock);
    if (n == 0)
      continue;
    if (n > 1) {
      // keep the first page; the rest go on this CPU's list.
      acquire(&kmem[id].lock);
      last->next = kmem[id].freelist;
      kmem[id].freelist = first->next;
      kmem[id].nfree += n - 1;
      release(&kmem[id].lock);
    }
    return first;
  }
  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id;

  push_off();
  id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if (r) {
    kmem[id].freelist = r->next;
    kmem[id].nfree--;
  }
  release(&kmem[id].lock);
  if (r == 0)
    r = steal(id);
  pop_off();

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Print how many free pages each CPU's list holds. Called by
// procdump (^P). Like procdump it takes no lock, so that it
// works on a stuck machine; the counts may be a moment old.
void
kmemdump(void)
{
  printk("free pages:");
  for (int i = 0; i < NCPU; i++)
    printk(" %s %d", kmem[i].name, kmem[i].nfree);
  printk("\n");
}
