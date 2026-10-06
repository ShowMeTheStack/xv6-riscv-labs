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
struct bucket {
  struct buf *first; // list of buffers, through buf.next
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Every buffer starts with blockno 0, so in bucket 0.
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.bucket[0].first;
    bcache.bucket[0].first = b;
  }
}

// Return the buffer holding block (dev, blockno) if bucket bk
// has one, or 0. The caller holds the lock that protects bk.
static struct buf *
bfind(struct bucket *bk, uint dev, uint blockno)
{
  struct buf *b;

  for (b = bk->first; b != 0; b = b->next)
    if (b->dev == dev && b->blockno == blockno)
      return b;
  return 0;
}

// Remove b from bucket bk's list.
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
  int i, vi;

  acquire(&bcache.lock);

  // Is the block already cached?
  if ((b = bfind(bk, dev, blockno)) != 0) {
    b->refcnt++;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer,
  // in whichever bucket it is.
  victim = 0;
  vi = 0;
  for (i = 0; i < NBUCKET; i++) {
    for (b = bcache.bucket[i].first; b != 0; b = b->next) {
      if (b->refcnt == 0 && (victim == 0 || b->lastuse < victim->lastuse)) {
        victim = b;
        vi = i;
      }
    }
  }
  if (victim == 0)
    panic("bget: no buffers");

  // Move it to the bucket of the block it will hold.
  bunlink(&bcache.bucket[vi], victim);
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  victim->next = bk->first;
  bk->first = victim;
  release(&bcache.lock);
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
  uint now;

  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&tickslock);
  now = ticks;
  release(&tickslock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->lastuse = now;
  }
  release(&bcache.lock);
}

void
bpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b)
{
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}
