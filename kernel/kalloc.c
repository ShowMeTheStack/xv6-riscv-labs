// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages,
// and 2-megabyte superpages for user memory.

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

// RAM is divided into 2-megabyte-aligned chunks, each in one
// of three states.
#define NCHUNK    ((PHYSTOP - KERNBASE) / SUPERPGSIZE)
#define CHUNK(pa) (((uint64)(pa) - KERNBASE) / SUPERPGSIZE)
#define FREE      0 // whole and free
#define SUPER     1 // whole, in use as a superpage
#define SPLIT     2 // split into 512 pages, each free or in use

struct {
  struct spinlock lock;
  char state[NCHUNK];
  struct run *freelist[NCHUNK]; // a split chunk's free pages
  int nfree[NCHUNK];            // how many there are
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // the chunk that holds the kernel is split from the start;
  // every chunk above it starts whole and free.
  for (int c = 0; c < CHUNK(SUPERPGROUNDUP((uint64)end)); c++)
    kmem.state[c] = SPLIT;
  freerange(end, (void *)SUPERPGROUNDUP((uint64)end));
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
  int c = CHUNK(pa);

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  if (kmem.state[c] != SPLIT)
    panic("kfree: whole chunk");
  if (kmem.nfree[c] >= SUPERPGSIZE / PGSIZE)
    panic("kfree: double free"); // more free pages than the chunk has
  r->next = kmem.freelist[c];
  kmem.freelist[c] = r;
  kmem.nfree[c]++;
  release(&kmem.lock);
}

// Make free chunk c 512 free pages.
// Caller holds kmem.lock.
static void
split(int c)
{
  char *start = (char *)(KERNBASE + (uint64)c * SUPERPGSIZE);
  struct run *r;

  for (char *p = start + SUPERPGSIZE - PGSIZE; p >= start; p -= PGSIZE) {
    r = (struct run *)p;
    r->next = kmem.freelist[c];
    kmem.freelist[c] = r;
  }
  kmem.nfree[c] = SUPERPGSIZE / PGSIZE;
  kmem.state[c] = SPLIT;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r = 0;
  int c;

  acquire(&kmem.lock);
  // take a page from the lowest split chunk that has one,
  // so that the high chunks stay whole as long as possible.
  for (c = 0; c < NCHUNK; c++)
    if (kmem.state[c] == SPLIT && kmem.nfree[c] > 0)
      break;
  if (c == NCHUNK) {
    // none: split the lowest free chunk.
    for (c = 0; c < NCHUNK; c++)
      if (kmem.state[c] == FREE)
        break;
    if (c < NCHUNK)
      split(c);
  }
  if (c < NCHUNK) {
    r = kmem.freelist[c];
    kmem.freelist[c] = r->next;
    kmem.nfree[c]--;
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Free a superpage that superalloc() returned.
void
superfree(void *pa)
{
  int c = CHUNK(pa);

  if (((uint64)pa % SUPERPGSIZE) != 0 || (char *)pa < end ||
      (uint64)pa >= PHYSTOP)
    panic("superfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, SUPERPGSIZE);

  acquire(&kmem.lock);
  if (kmem.state[c] != SUPER)
    panic("superfree: not a superpage");
  kmem.state[c] = FREE;
  release(&kmem.lock);
}

// A superpage in use becomes 512 pages in use: from now
// on they are freed one at a time with kfree().
void
supersplit(void *pa)
{
  int c = CHUNK(pa);

  acquire(&kmem.lock);
  if (kmem.state[c] != SUPER)
    panic("supersplit");
  kmem.state[c] = SPLIT;
  kmem.freelist[c] = 0;
  kmem.nfree[c] = 0;
  release(&kmem.lock);
}

// Can chunk c become a superpage? Caller holds kmem.lock.
static int
whole(int c)
{
  // a split chunk whose 512 pages are all free counts too.
  return kmem.state[c] == FREE ||
         (kmem.state[c] == SPLIT && kmem.nfree[c] == SUPERPGSIZE / PGSIZE);
}

// Allocate one 2-megabyte, 2-megabyte-aligned chunk of
// physical memory, the highest free one.
// Returns 0 if no chunk is free.
void *
superalloc(void)
{
  int c;
  char *pa = 0;

  acquire(&kmem.lock);
  for (c = NCHUNK - 1; c >= 0; c--) {
    if (whole(c)) {
      kmem.state[c] = SUPER;
      kmem.freelist[c] = 0; // its pages are no longer free pages
      kmem.nfree[c] = 0;
      pa = (char *)(KERNBASE + (uint64)c * SUPERPGSIZE);
      break;
    }
  }
  release(&kmem.lock);

  if (pa)
    memset(pa, 5, SUPERPGSIZE); // fill with junk
  return (void *)pa;
}
