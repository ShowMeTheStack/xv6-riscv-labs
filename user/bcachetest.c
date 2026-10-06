// bcachetest: stress the buffer cache from three processes at once.
//
// Every check reads back what was written and compares it byte by
// byte, so a block that was lost, written twice or mixed up with
// another block shows up as FAIL.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"

#define NCHILD 3 // one per hart
#define NBLK 40  // blocks per big file: more than the 30 buffers
#define NSMALL 20

static int rounds = 4;
static char buf[BSIZE];

// a block's contents name the file (who) and the block number.
static void
fill(int who, int blk)
{
  ((int *)buf)[0] = who;
  ((int *)buf)[1] = blk;
  for (int i = 8; i < BSIZE; i++)
    buf[i] = who + blk + i;
}

static int
check(int who, int blk)
{
  if (((int *)buf)[0] != who || ((int *)buf)[1] != blk)
    return 0;
  for (int i = 8; i < BSIZE; i++)
    if (buf[i] != (char)(who + blk + i))
      return 0;
  return 1;
}

static int
writefile(char *name, int who, int nblk)
{
  int fd = open(name, O_CREATE | O_TRUNC | O_RDWR);
  if (fd < 0)
    return 0;
  for (int b = 0; b < nblk; b++) {
    fill(who, b);
    if (write(fd, buf, BSIZE) != BSIZE) {
      close(fd);
      return 0;
    }
  }
  close(fd);
  return 1;
}

static int
readfile(char *name, int who, int nblk)
{
  struct stat st;
  int fd = open(name, O_RDONLY);
  if (fd < 0)
    return 0;
  if (fstat(fd, &st) < 0 || st.size != nblk * BSIZE) {
    printf("bcachetest: %s: size %d, expected %d\n", name, (int)st.size, nblk * BSIZE);
    close(fd);
    return 0;
  }
  for (int b = 0; b < nblk; b++) {
    if (read(fd, buf, BSIZE) != BSIZE || !check(who, b)) {
      // the first 8 bytes of every block name its file and block.
      printf("bcachetest: %s: block %d should be (%d, %d) but is (%d, %d)\n", name, b, who, b,
             ((int *)buf)[0], ((int *)buf)[1]);
      close(fd);
      return 0;
    }
  }
  close(fd);
  return 1;
}

// run f(child) in NCHILD processes at once; 1 if all of them succeed.
static int
parallel(int (*f)(int))
{
  int ok = 1, xs;
  for (int c = 0; c < NCHILD; c++) {
    int pid = fork();
    if (pid < 0) {
      printf("bcachetest: fork failed\n");
      return 0;
    }
    if (pid == 0)
      exit(f(c) ? 0 : 1);
  }
  for (int c = 0; c < NCHILD; c++)
    if (wait(&xs) < 0 || xs != 0)
      ok = 0;
  return ok;
}

// each child writes and rereads its own big file.
static int
private(int c)
{
  char name[] = "bcp0";
  name[3] += c;
  for (int r = 0; r < rounds; r++)
    if (!writefile(name, 100 * c + r, NBLK) || !readfile(name, 100 * c + r, NBLK))
      return 0;
  return unlink(name) == 0;
}

// all children read the same file at the same time.
static int
shared(int c)
{
  for (int r = 0; r < 2 * rounds; r++)
    if (!readfile("bcshared", 999, NBLK))
      return 0;
  return 1;
}

// each child creates, checks and deletes many small files, so the
// children allocate inodes and blocks from the same bitmap and inode
// blocks at the same time.
static int
small(int c)
{
  char name[] = "bcs0_00";
  name[3] += c;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < NSMALL; i++) {
      name[5] = '0' + i / 10;
      name[6] = '0' + i % 10;
      if (!writefile(name, 1000 * c + i, 2))
        return 0;
    }
    for (int i = 0; i < NSMALL; i++) {
      name[5] = '0' + i / 10;
      name[6] = '0' + i % 10;
      if (!readfile(name, 1000 * c + i, 2) || unlink(name) != 0)
        return 0;
    }
  }
  return 1;
}

// remove what together() leaves behind, also after a failed run,
// so that every run starts from the same state.
static void
cleanup(void)
{
  char f[] = "bct0/f";
  for (int c = 0; c < NCHILD; c++) {
    f[3] = '0' + c;
    unlink(f);
    f[4] = 0;
    unlink(f);
    f[4] = '/';
  }
}

// Each child appends one block per round to its own file, working in
// its own directory, so that no inode lock keeps them apart. Before
// each round the parent waits for a clock tick and reads a big file,
// which pushes the free-block bitmap out of the cache; then it lets
// the three children go at once. All three allocate a block, so all
// three need the bitmap block at about the same moment. At the end
// each child checks every byte of its file.
static int
together(void)
{
  int go[NCHILD][2], done[2], fd, ok = 1, xs;
  int n = 15 * rounds;
  char dir[] = "bct0", x;

  if (pipe(done) < 0)
    return 0;
  for (int c = 0; c < NCHILD; c++) {
    dir[3] = '0' + c;
    if (mkdir(dir) < 0 || pipe(go[c]) < 0)
      return 0;
  }
  for (int c = 0; c < NCHILD; c++) {
    if (fork() == 0) {
      dir[3] = '0' + c;
      if (chdir(dir) < 0 || (fd = open("f", O_CREATE | O_RDWR)) < 0)
        exit(1);
      for (int r = 0; r < n; r++) {
        if (read(go[c][0], &x, 1) != 1)
          exit(1);
        fill(c, r);
        if (write(fd, buf, BSIZE) != BSIZE)
          exit(1);
        write(done[1], "d", 1);
      }
      close(fd);
      exit(readfile("f", c, n) && unlink("f") == 0 ? 0 : 1);
    }
  }
  close(done[1]); // so that read(done[0]) returns 0 if all children die
  for (int r = 0; r < n; r++) {
    pause(1);
    if (!readfile("bcshared", 999, NBLK))
      ok = 0;
    for (int c = 0; c < NCHILD; c++)
      write(go[c][1], "g", 1);
    for (int c = 0; c < NCHILD; c++)
      read(done[0], &x, 1);
  }
  for (int c = 0; c < NCHILD; c++)
    if (wait(&xs) < 0 || xs != 0)
      ok = 0;
  cleanup();
  return ok;
}

static int
report(char *what, int ok)
{
  printf("bcachetest: %s: %s\n", what, ok ? "OK" : "FAIL");
  return ok;
}

int
main(int argc, char *argv[])
{
  int ok = 1;

  if (argc > 1)
    rounds = atoi(argv[1]);
  cleanup();
  ok &= report("private files", parallel(private));
  ok &= report("one shared file", writefile("bcshared", 999, NBLK) && parallel(shared));
  ok &= report("small files", parallel(small));
  ok &= report("allocating together", together());
  unlink("bcshared");
  printf("bcachetest: %s\n", ok ? "ALL OK" : "SOME TESTS FAILED");
  exit(ok ? 0 : 1);
}
