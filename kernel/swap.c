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

// keep at least this many pages free for allocations that cannot
// evict.
#define FREELOW 32

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
  int hand; // the clock hand: next frame to look at
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

// go round the frame table, clock-wise, for a user page to evict.
// a page whose PTE has A set was used since the hand last passed:
// clear A and give it a second chance. only pages of a process
// that is not running on any hart (it flushed its TLB when it
// entered the kernel) or of the caller itself are taken, and
// never a pinned page. on success the PTE says "in slot s" and
// the page is the caller's to write out and free.
static void *
clock(int s)
{
  struct proc *me = myproc();
  struct proc *p;
  pte_t *pte;
  uint64 va;
  int i;

  for (int n = 0; n < 2 * NFRAME; n++) {
    acquire(&ft.lock);
    i = ft.hand;
    ft.hand = (ft.hand + 1) % NFRAME;
    p = ft.f[i].proc;
    va = ft.f[i].va;
    release(&ft.lock);
    if (p == 0)
      continue;

    // p->lock before ft.lock: freeproc frees pages holding p->lock.
    acquire(&p->lock);
    if (p->state == SLEEPING || p->state == RUNNABLE || p == me) {
      acquire(&ft.lock);
      pte = 0;
      if (ft.f[i].proc == p && ft.f[i].va == va)
        pte = walk(p->pagetable, va, 0);
      // during exec the frame may be in the new page table, not
      // in p->pagetable yet: then the PTE maps another page. the
      // stack guard page (no PTE_U) is never used: not worth a write.
      if (pte && (*pte & PTE_V) && (*pte & PTE_U) &&
          PTE2PA(*pte) == FRAMEPA(i) && !(*pte & PTE_PIN)) {
        if (*pte & PTE_A) {
          *pte &= ~PTE_A; // second chance
        } else {
          *pte = SLOT2PTE(s) | (*pte & (PTE_R | PTE_W | PTE_X | PTE_U)) |
                 PTE_SWAP;
          ft.f[i].proc = 0;
          release(&ft.lock);
          release(&p->lock);
          return (void *)FRAMEPA(i);
        }
      }
      release(&ft.lock);
    }
    release(&p->lock);
  }
  return 0;
}

// write one user page out to swap and free it. returns 0 if the
// swap area is full or no page can be evicted. sleeps, so the
// caller must not hold a spinlock.
int
evict(void)
{
  void *pa;
  int s;

  acquiresleep(&swapio.lock);
  if ((s = swapalloc()) < 0) {
    releasesleep(&swapio.lock);
    return 0;
  }
  if ((pa = clock(s)) == 0) {
    swapfree(s);
    releasesleep(&swapio.lock);
    return 0;
  }
  swaprw(s, pa, 1);
  releasesleep(&swapio.lock);
  kfree(pa);
  return 1;
}

// allocate a page for user memory. when few pages are free, first
// evict pages to swap until FREELOW are free again, so that
// allocations that cannot wait for the disk (page-table pages,
// fork, pipes: all under spinlocks) still find memory. evicting
// sleeps, so a caller that holds a spinlock (a copy under pi->lock
// that touches a lazy page) only gets what is free, as before.
void *
ualloc(void)
{
  int locked;

  push_off();
  locked = mycpu()->noff > 1; // a spinlock is held besides our push_off
  pop_off();
  while (!locked && kfreecount() < FREELOW && evict())
    ;
  return kalloc();
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

  if ((mem = ualloc()) == 0)
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
