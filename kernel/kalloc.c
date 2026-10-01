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

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// Reference counts, one per physical page from KERNBASE to
// PHYSTOP: how many page tables map the page (a page that
// kalloc returned and nobody has shared yet has count 1).
// kfree drops one reference and frees the page at zero.
#define PA2REF(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[(PHYSTOP - KERNBASE) / PGSIZE];
} kref;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    kref.count[PA2REF(p)] = 1; // kfree drops this to 0
    kfree(p);
  }
}

// Drop one reference to the page of physical memory pointed
// at by pa, which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// The page is freed when its last reference is dropped.
void
kfree(void *pa)
{
  struct run *r;
  int n;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kref.lock);
  if (kref.count[PA2REF(pa)] < 1)
    panic("kfree: refcount");
  n = --kref.count[PA2REF(pa)];
  release(&kref.lock);
  if (n > 0)
    return; // another page table still maps it

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    memset((char *)r, 5, PGSIZE); // fill with junk
    acquire(&kref.lock);
    kref.count[PA2REF(r)] = 1;
    release(&kref.lock);
  }
  return (void *)r;
}

// Add a reference to page pa: one more page table maps it.
void
krefinc(uint64 pa)
{
  acquire(&kref.lock);
  if (kref.count[PA2REF(pa)] < 1)
    panic("krefinc");
  kref.count[PA2REF(pa)]++;
  release(&kref.lock);
}

// How many page tables map page pa.
int
krefcount(uint64 pa)
{
  int n;

  acquire(&kref.lock);
  n = kref.count[PA2REF(pa)];
  release(&kref.lock);
  return n;
}
