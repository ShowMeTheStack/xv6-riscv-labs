// Swapping: user pages written out to the swap area at the end
// of the disk, and read back in when they are used again.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "proc.h"
#include "defs.h"

// a slot holds one page: PGSIZE / BSIZE blocks of the swap area.
#define SLOTBLOCK(s) (SWAPSTART + (s) * (PGSIZE / BSIZE))

// the swap slots. a slot is free when no PTE refers to it;
// after fork, a parent and a child can both refer to one.
struct {
  struct spinlock lock;
  uchar ref[NSWAP]; // number of PTEs that hold each slot
  int nused;        // slots with ref > 0
} slots;

// the frame table: for every physical page that is mapped as a
// user page, the process and the virtual address that map it.
// eviction needs this reverse map to find the PTE of a page.
#define NFRAME     ((PHYSTOP - KERNBASE) / PGSIZE)
#define FRAME(pa)  (((uint64)(pa) - KERNBASE) / PGSIZE)
#define FRAMEPA(i) (KERNBASE + (uint64)(i) * PGSIZE)

struct {
  struct spinlock lock;
  struct frame {
    struct proc *proc; // 0 if not a user page
    uint64 va;
  } f[NFRAME];
} ft;

void
swapinit(void)
{
  initlock(&slots.lock, "slots");
  initlock(&ft.lock, "ftable");
}

// record that p maps the user page pa at va.
void
ftset(void *pa, struct proc *p, uint64 va)
{
  acquire(&ft.lock);
  ft.f[FRAME(pa)].proc = p;
  ft.f[FRAME(pa)].va = va;
  release(&ft.lock);
}

// pa is no longer mapped as a user page.
void
ftclear(void *pa)
{
  acquire(&ft.lock);
  ft.f[FRAME(pa)].proc = 0;
  release(&ft.lock);
}

// allocate a free slot. returns -1 if the swap area is full.
int
swapalloc(void)
{
  acquire(&slots.lock);
  for (int s = 0; s < NSWAP; s++) {
    if (slots.ref[s] == 0) {
      slots.ref[s] = 1;
      slots.nused++;
      release(&slots.lock);
      return s;
    }
  }
  release(&slots.lock);
  return -1;
}

// one more PTE refers to slot s (fork copied a swapped-out PTE).
void
swapdup(int s)
{
  acquire(&slots.lock);
  if (slots.ref[s] < 1 || slots.ref[s] == 255)
    panic("swapdup");
  slots.ref[s]++;
  release(&slots.lock);
}

// a PTE no longer refers to slot s.
void
swapfree(int s)
{
  acquire(&slots.lock);
  if (slots.ref[s] < 1)
    panic("swapfree");
  if (--slots.ref[s] == 0)
    slots.nused--;
  release(&slots.lock);
}
