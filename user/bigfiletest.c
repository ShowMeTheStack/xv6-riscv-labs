// Test files larger than the old 268-block limit.
//
//   bigfiletest [n]   write an n-block file (default 6580), read it
//                     back, count the blocks it used, unlink it, and
//                     check that every block came back.
//   bigfiletest -w n  only write an n-block bigtest.dat and keep it.
//   bigfiletest -r    only read bigtest.dat back and check it.
//
// (usertests creates and removes a file called "bigfile", so this
// program cannot have that name.)

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define NAME "bigtest.dat"

char buf[BSIZE];

// Fill buf with what block n of the file should hold: its own
// block number, then a pattern that differs from block to block.
void
fill(int n)
{
  int i;

  for (i = 0; i < BSIZE; i++)
    buf[i] = n + i;
  ((int *)buf)[0] = n;
}

// How many indirect blocks a file of n blocks needs.
int
nindirect(int n)
{
  if (n <= NDIRECT)
    return 0;
  if (n <= NDIRECT + NINDIRECT)
    return 1;
  n -= NDIRECT + NINDIRECT;
  return 2 + (n + NINDIRECT - 1) / NINDIRECT;
}

// Append n blocks to fd. Returns how many were written.
int
writeblocks(int fd, int n)
{
  int i;

  for (i = 0; i < n; i++) {
    fill(i);
    if (write(fd, buf, BSIZE) != BSIZE)
      break;
  }
  return i;
}

// Read NAME back. Returns its number of blocks, or -1 if a block
// does not hold what writeblocks() put there.
int
checkfile(void)
{
  char want[BSIZE];
  int fd, i, r;

  if ((fd = open(NAME, O_RDONLY)) < 0)
    return -1;
  for (i = 0;; i++) {
    r = read(fd, want, BSIZE);
    if (r == 0)
      break;
    fill(i);
    if (r != BSIZE || memcmp(want, buf, BSIZE) != 0) {
      printf("bigfiletest: block %d is wrong\n", i);
      close(fd);
      return -1;
    }
  }
  close(fd);
  return i;
}

void
check(char *what, int ok)
{
  printf("bigfiletest: %s: %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) {
    printf("bigfiletest: SOME TESTS FAILED\n");
    exit(1);
  }
}

int
main(int argc, char *argv[])
{
  int fd, n, f0, f1, f2, used;
  struct stat st;

  if (argc == 2 && strcmp(argv[1], "-r") == 0) {
    n = checkfile();
    printf("bigfiletest: %s has %d good blocks, %d blocks free\n", NAME,
           n, freeblocks());
    exit(n < 0);
  }
  if (argc == 3 && strcmp(argv[1], "-w") == 0) {
    n = atoi(argv[2]);
    unlink(NAME);
    if ((fd = open(NAME, O_CREATE | O_WRONLY)) < 0)
      exit(1);
    printf("bigfiletest: wrote %d blocks\n", writeblocks(fd, n));
    close(fd);
    exit(0);
  }

  n = argc > 1 ? atoi(argv[1]) : 6580;
  if (n < 1 || n > MAXFILE) {
    printf("usage: bigfiletest [1..%d] | -w n | -r\n", (int)MAXFILE);
    exit(1);
  }
  printf("bigfiletest: %d blocks (old limit 268, new limit %d)\n", n,
         (int)MAXFILE);

  unlink(NAME);
  fd = open(NAME, O_CREATE | O_WRONLY);
  check("create", fd >= 0);
  f0 = freeblocks(); // after create: a new directory block is not ours
  check("write", writeblocks(fd, n) == n);
  if (n == MAXFILE)
    check("write past MAXFILE fails", write(fd, buf, BSIZE) < 0);
  close(fd);

  check("size", stat(NAME, &st) == 0 && st.size == (uint64)n * BSIZE);
  f1 = freeblocks();
  used = f0 - f1;
  printf("bigfiletest: blocks used %d = %d data + %d indirect\n", used,
         n, nindirect(n));
  check("block count", used == n + nindirect(n));
  check("read back", checkfile() == n);

  check("unlink", unlink(NAME) == 0);
  f2 = freeblocks();
  printf("bigfiletest: free blocks %d before, %d after\n", f0, f2);
  check("all blocks freed", f2 == f0);
  printf("bigfiletest: ALL OK\n");
  exit(0);
}
