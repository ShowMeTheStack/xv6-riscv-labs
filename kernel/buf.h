struct buf {
  int valid; // has data been read from disk?
  int disk;  // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint lastuse;     // ticks when refcnt last dropped to 0
  struct buf *next; // next buffer in the same hash bucket
  uchar data[BSIZE];
};
