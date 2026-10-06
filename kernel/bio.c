// Buffer cache.
//
// The buffer cache is a hash table of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13 // a prime, so block numbers spread over all buckets

// A block can only be cached in bucket[blockno % NBUCKET].
// A bucket's lock protects its list and the dev, blockno, refcnt
// and lastuse of every buffer on it.
struct bucket {
  struct spinlock lock;
  struct buf *first; // list of buffers, through buf.next
};

// Lock order: evict, then bucket locks in increasing index order.
struct {
  struct spinlock evict; // held by a miss while it recycles a buffer
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.evict, "bcache.evict");
  for (int i = 0; i < NBUCKET; i++)
    initlock(&bcache.bucket[i].lock, "bcache.bucket");

  // Every buffer starts with blockno 0, so in bucket 0.
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.bucket[0].first;
    bcache.bucket[0].first = b;
  }
}

// Return the buffer holding block (dev, blockno) if bucket bk
// has one, or 0. The caller holds bk's lock.
static struct buf *
bfind(struct bucket *bk, uint dev, uint blockno)
{
  struct buf *b;

  for (b = bk->first; b != 0; b = b->next)
    if (b->dev == dev && b->blockno == blockno)
      return b;
  return 0;
}

// Remove b from bucket bk's list. The caller holds bk's lock.
static void
bunlink(struct bucket *bk, struct buf *b)
{
  struct buf *p;

  if (bk->first == b) {
    bk->first = b->next;
    return;
  }
  for (p = bk->first; p->next != b; p = p->next)
    ;
  p->next = b->next;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct bucket *bk = &bcache.bucket[blockno % NBUCKET];
  struct buf *b, *victim;
  int i, vi, better;

  // Is the block already cached? Only its own bucket can hold it.
  acquire(&bk->lock);
  if ((b = bfind(bk, dev, blockno)) != 0) {
    b->refcnt++;
    release(&bk->lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bk->lock);

  // Not cached. Only one miss at a time may recycle a buffer.
  acquire(&bcache.evict);

  // Look again: while this hart held no lock, another hart may
  // have missed on the same block and cached it. Only a miss adds
  // a block to a bucket, so while evict is held, the answer stays
  // true.
  acquire(&bk->lock);
  if ((b = bfind(bk, dev, blockno)) != 0) {
    b->refcnt++;
    release(&bk->lock);
    release(&bcache.evict);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bk->lock);

  // Recycle the least recently used (LRU) unused buffer,
  // in whichever bucket it is. Keep holding the lock of the
  // bucket with the best buffer so far, so that no lookup can
  // take that buffer while the later buckets are searched.
  victim = 0;
  vi = -1;
  for (i = 0; i < NBUCKET; i++) {
    acquire(&bcache.bucket[i].lock);
    better = 0;
    for (b = bcache.bucket[i].first; b != 0; b = b->next) {
      if (b->refcnt == 0 && (victim == 0 || b->lastuse < victim->lastuse)) {
        victim = b;
        better = 1;
      }
    }
    if (better) {
      if (vi >= 0)
        release(&bcache.bucket[vi].lock);
      vi = i;
    } else {
      release(&bcache.bucket[i].lock);
    }
  }
  if (victim == 0)
    panic("bget: no buffers");

  // Take it out of its bucket. Now no lookup can find it, and
  // nobody holds it (refcnt is 0), so it is ours alone.
  bunlink(&bcache.bucket[vi], victim);
  release(&bcache.bucket[vi].lock);

  // Put it in the bucket of the block it will hold.
  acquire(&bk->lock);
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  victim->next = bk->first;
  bk->first = victim;
  release(&bk->lock);
  release(&bcache.evict);
  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
// Only the log calls bwrite.
void
bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// If no one else wants it, record when it was last used.
void
brelse(struct buf *b)
{
  struct bucket *bk;
  uint now;

  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // Read the clock without tickslock, which every brelse on
  // every hart would otherwise take. A value one tick old only
  // makes the LRU choice a little less exact. The atomic load is
  // a single 32-bit load that the compiler cannot split or repeat.
  now = __atomic_load_n(&ticks, __ATOMIC_RELAXED);

  // b cannot move to another bucket before the decrement:
  // a miss recycles only buffers with refcnt 0.
  bk = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bk->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->lastuse = now;
  }
  release(&bk->lock);
}

// The caller holds b (refcnt > 0), so b stays in its bucket.
void
bpin(struct buf *b)
{
  struct bucket *bk = &bcache.bucket[b->blockno % NBUCKET];

  acquire(&bk->lock);
  b->refcnt++;
  release(&bk->lock);
}

void
bunpin(struct buf *b)
{
  struct bucket *bk = &bcache.bucket[b->blockno % NBUCKET];

  acquire(&bk->lock);
  b->refcnt--;
  release(&bk->lock);
}
