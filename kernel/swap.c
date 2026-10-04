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

// swap I/O moves one page at a time. holding swapio.lock also
// keeps a page from being read back in while it is still being
// written out.
struct {
  struct sleeplock lock;
  struct buf b; // only used to wait for the disk; b.data is unused
} swapio;

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
  initsleeplock(&swapio.lock, "swapio");
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

// copy the page at physical address pa to slot s (write = 1),
// or slot s to pa (write = 0). sleeps until the disk is done.
// the caller holds swapio.lock.
static void
swaprw(int s, void *pa, int write)
{
  if (!holdingsleep(&swapio.lock))
    panic("swaprw");
  virtio_disk_rwpage(&swapio.b, SLOTBLOCK(s), pa, write);
}

// the page at va is in swap: read it into a fresh page and map
// it again. returns the physical address, or 0 if out of memory.
// sleeps, so the caller must not hold a spinlock.
uint64
swapin(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  char *mem;
  int s;

  if ((mem = kalloc()) == 0)
    return 0;
  pte = walk(pagetable, va, 0);
  s = PTE2SLOT(*pte);

  acquiresleep(&swapio.lock);
  swaprw(s, mem, 0);
  releasesleep(&swapio.lock);

  *pte = PA2PTE(mem) | (*pte & (PTE_R | PTE_W | PTE_X | PTE_U)) | PTE_V;
  ftset(mem, myproc(), va);
  swapfree(s);
  return (uint64)mem;
}
