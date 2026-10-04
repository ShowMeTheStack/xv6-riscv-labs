// Tests for mmap and munmap. Prints one line per check;
// "mmaptest NAME" runs only that check.

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int failed;
char buf[PGSIZE]; // the user stack is one page: keep big buffers off it

void
fail(char *test, char *what)
{
  printf("mmaptest: %s: FAIL (%s)\n", test, what);
  failed = 1;
}

// The byte at offset i of every test file.
char
fbyte(uint i)
{
  return (i % 251) ^ (i / PGSIZE);
}

// Create name with size bytes of fbyte().
void
makefile(char *name, uint size)
{
  char buf[512];
  uint i, j, n;
  int fd;

  if ((fd = open(name, O_CREATE | O_TRUNC | O_RDWR)) < 0) {
    printf("mmaptest: cannot create %s\n", name);
    exit(1);
  }
  for (i = 0; i < size; i += n) {
    n = size - i < sizeof(buf) ? size - i : sizeof(buf);
    for (j = 0; j < n; j++)
      buf[j] = fbyte(i + j);
    if (write(fd, buf, n) != n) {
      printf("mmaptest: cannot write %s\n", name);
      exit(1);
    }
  }
  close(fd);
}

// Read n bytes at offset off of name into buf.
int
readat(char *name, uint off, char *buf, int n)
{
  static char skip[512];
  int fd, k;

  if ((fd = open(name, O_RDONLY)) < 0)
    return -1;
  while (off > 0) {
    k = off < sizeof(skip) ? off : sizeof(skip);
    if (read(fd, skip, k) != k) {
      close(fd);
      return -1;
    }
    off -= k;
  }
  k = read(fd, buf, n);
  close(fd);
  return k;
}

int
filesize(char *name)
{
  struct stat st;

  if (stat(name, &st) < 0)
    return -1;
  return st.size;
}

// Free pages, counted the way usertests counts them.
int
countfree()
{
  int n = 0;
  uint64 sz0 = (uint64)sbrk(0);

  while (sbrk(PGSIZE) != SBRK_ERROR)
    n++;
  sbrk(-((uint64)sbrk(0) - sz0));
  return n;
}

// Run f in a child; return the child's exit status.
int
inchild(void (*f)(char *), char *arg)
{
  int pid, xs;

  if ((pid = fork()) < 0) {
    printf("mmaptest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    f(arg);
    exit(0);
  }
  if (wait(&xs) != pid)
    return -100;
  return xs;
}

void
store(char *a)
{
  *(volatile char *)a = 1;
}

void
load(char *a)
{
  printf("mmaptest: loaded %d: should have been killed\n",
         *(volatile char *)a);
}

// Map a file read-only, close it, read it through memory.
void
readtest(void)
{
  char *t = "read";
  uint size = 2 * PGSIZE + 1000, i;
  int fd, f0, f1, bad = 0;
  char *m;

  makefile("mm.read", size);
  if ((fd = open("mm.read", O_RDONLY)) < 0) {
    fail(t, "open");
    return;
  }
  m = mmap(0, 3 * PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd); // the mapping keeps its own reference
  if (m == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  if ((uint64)m % PGSIZE != 0 || (uint64)m < (uint64)sbrk(0))
    fail(t, "address");

  countfree(); // warm-up: the heap's page-table pages
  f0 = countfree();
  if (m[PGSIZE + 5] != fbyte(PGSIZE + 5))
    fail(t, "page 1");
  f1 = countfree();
  printf("mmaptest: read: touching 1 of 3 mapped pages took %d free pages\n",
         f0 - f1);
  if (f0 - f1 != 1)
    fail(t, "lazy");

  for (i = 0; i < 3 * PGSIZE; i++)
    if (m[i] != (i < size ? fbyte(i) : 0))
      bad++;
  if (bad)
    fail(t, "contents");
  if (munmap(m, 3 * PGSIZE) != 0)
    fail(t, "munmap");
  if (inchild(load, m) != -1)
    fail(t, "still mapped after munmap");
  unlink("mm.read");
}

// Stores through a shared mapping reach the file at munmap; the
// file never grows.
void
sharedtest(void)
{
  char *t = "shared";
  uint size = 2 * PGSIZE + 100, i;
  int fd, bad = 0;
  char *m;

  makefile("mm.shared", size);
  if ((fd = open("mm.shared", O_RDWR)) < 0) {
    fail(t, "open");
    return;
  }
  m = mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  strcpy(m + 10, "hello");    // page 0
  m[2 * PGSIZE + 50] = 'X';   // page 2, inside the file
  m[2 * PGSIZE + 3000] = 'Y'; // page 2, past the end of the file
  if (munmap(m, 3 * PGSIZE) != 0) {
    fail(t, "munmap");
    return;
  }

  if (filesize("mm.shared") != size)
    fail(t, "the file changed size");
  if (readat("mm.shared", 0, buf, PGSIZE) != PGSIZE ||
      strcmp(buf + 10, "hello") != 0)
    fail(t, "page 0 not written back");
  for (i = 0; i < PGSIZE; i++)
    if (i < 10 || i > 15)
      bad += buf[i] != fbyte(i);
  if (readat("mm.shared", PGSIZE, buf, PGSIZE) != PGSIZE)
    fail(t, "read page 1");
  for (i = 0; i < PGSIZE; i++)
    bad += buf[i] != fbyte(PGSIZE + i);
  if (readat("mm.shared", 2 * PGSIZE, buf, PGSIZE) != 100 || buf[50] != 'X')
    fail(t, "page 2 not written back");
  for (i = 0; i < 100; i++)
    if (i != 50)
      bad += buf[i] != fbyte(2 * PGSIZE + i);
  if (bad)
    fail(t, "other bytes changed");
  unlink("mm.shared");
}

// Private stores never reach the file; prot is enforced.
void
privatetest(void)
{
  char *t = "private";
  int fd, pfd[2], i, bad = 0;
  char *m, *r;

  makefile("mm.priv", PGSIZE);
  if ((fd = open("mm.priv", O_RDONLY)) < 0) {
    fail(t, "open");
    return;
  }
  if (mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) !=
      MAP_FAILED)
    fail(t, "shared writable map of a read-only file");
  m = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  r = mmap(0, PGSIZE, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED || r == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  for (i = 0; i < PGSIZE; i++)
    m[i] = 'P';
  if (m[100] != 'P' || r[100] != fbyte(100))
    fail(t, "contents");
  if (inchild(store, r) != -1)
    fail(t, "store to a read-only mapping");
  if ((fd = open("mm.priv", O_RDONLY)) < 0 || read(fd, r, 10) != -1)
    fail(t, "read() into a read-only mapping");
  close(fd);
  if (munmap(m, PGSIZE) != 0 || munmap(r, PGSIZE) != 0)
    fail(t, "munmap");
  if (readat("mm.priv", 0, buf, PGSIZE) != PGSIZE)
    fail(t, "read");
  for (i = 0; i < PGSIZE; i++)
    bad += buf[i] != fbyte(i);
  if (bad)
    fail(t, "a private store reached the file");

  pipe(pfd);
  if (mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, pfd[0], 0) != MAP_FAILED)
    fail(t, "mapped a pipe");
  close(pfd[0]);
  close(pfd[1]);
  unlink("mm.priv");
}

// munmap of a part at the start or at the end of a mapping.
void
partialtest(void)
{
  char *t = "partial";
  int fd, i;
  char *m;

  makefile("mm.part", 4 * PGSIZE);
  if ((fd = open("mm.part", O_RDWR)) < 0) {
    fail(t, "open");
    return;
  }
  m = mmap(0, 4 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  for (i = 0; i < 4; i++)
    m[i * PGSIZE] = 'A' + i;
  if (munmap(m + PGSIZE, PGSIZE) != -1)
    fail(t, "unmapped a hole in the middle");
  if (munmap(m, PGSIZE) != 0) // the first page
    fail(t, "munmap start");
  if (munmap(m + 3 * PGSIZE, PGSIZE) != 0) // the last page
    fail(t, "munmap end");
  if (inchild(load, m) != -1 || inchild(load, m + 3 * PGSIZE) != -1)
    fail(t, "still mapped after munmap");
  if (m[PGSIZE] != 'B' || m[2 * PGSIZE] != 'C')
    fail(t, "the rest lost its contents");
  m[2 * PGSIZE + 1] = 'c';
  if (munmap(m + PGSIZE, 2 * PGSIZE) != 0)
    fail(t, "munmap rest");
  for (i = 0; i < 4; i++)
    if (readat("mm.part", i * PGSIZE, buf, 2) != 2 || buf[0] != 'A' + i ||
        buf[1] != (i == 2 ? 'c' : fbyte(i * PGSIZE + 1)))
      fail(t, "not written back");
  unlink("mm.part");
}

char *forkmap;

void
forkchild(char *arg)
{
  int i;

  // page 1: the parent never touched it.
  for (i = 0; i < PGSIZE; i++)
    if (forkmap[PGSIZE + i] != fbyte(PGSIZE + i))
      exit(1);
  forkmap[PGSIZE] = 'C';
  exit(0); // no munmap: exit writes page 1 back
}

// A child has its parent's mappings.
void
forktest(void)
{
  char *t = "fork";

  makefile("mm.fork", 2 * PGSIZE);
  int fd = open("mm.fork", O_RDWR);
  forkmap = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (forkmap == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  forkmap[0] = 'P';
  if (inchild(forkchild, 0) != 0)
    fail(t, "child");
  if (forkmap[PGSIZE] != 'C') // loaded now, after the child's exit
    fail(t, "the child's store did not reach the file");
  if (munmap(forkmap, 2 * PGSIZE) != 0)
    fail(t, "munmap");
  if (readat("mm.fork", 0, buf, 1) != 1 || buf[0] != 'P' ||
      readat("mm.fork", PGSIZE, buf, 1) != 1 || buf[0] != 'C')
    fail(t, "file");
  unlink("mm.fork");
}

void
exitchild(char *how)
{
  int fd = open("mm.exit", O_RDWR);
  char *m = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  char *argv[] = {"mmaptest", "nop", 0};

  close(fd);
  if (m == MAP_FAILED)
    exit(1);
  if (how[0] == 'x') {
    m[0] = 'x';
    exit(0);
  }
  m[PGSIZE] = 'e';
  exec("mmaptest", argv);
  exit(1);
}

// exit and exec write dirty shared pages back.
void
exittest(void)
{
  char *t = "exit";

  makefile("mm.exit", 2 * PGSIZE);
  if (inchild(exitchild, "x") != 0 || inchild(exitchild, "e") != 0)
    fail(t, "child");
  if (readat("mm.exit", 0, buf, 1) != 1 || buf[0] != 'x')
    fail(t, "exit did not write back");
  if (readat("mm.exit", PGSIZE, buf, 1) != 1 || buf[0] != 'e')
    fail(t, "exec did not write back");
  unlink("mm.exit");
}

// The table fills up; nothing leaks.
void
manytest(void)
{
  char *t = "many";
  char *m[NVMA + 1];
  int fd, i, f0, f1;

  makefile("mm.many", PGSIZE);
  countfree();
  f0 = countfree();
  if ((fd = open("mm.many", O_RDONLY)) < 0) {
    fail(t, "open");
    return;
  }
  for (i = 0; i < NVMA; i++) {
    m[i] = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m[i] == MAP_FAILED || m[i][7] != fbyte(7)) {
      fail(t, "mmap");
      return;
    }
  }
  if (mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED)
    fail(t, "more than NVMA mappings");
  close(fd);
  for (i = 0; i < NVMA; i++)
    if (munmap(m[i], PGSIZE) != 0)
      fail(t, "munmap");

  // each round takes a file and a page; if munmap failed to give
  // them back, open() would run out of files after NFILE rounds.
  for (i = 0; i < 3 * NFILE; i++) {
    if ((fd = open("mm.many", O_RDONLY)) < 0) {
      fail(t, "open failed: files leak");
      return;
    }
    m[0] = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m[0] == MAP_FAILED || m[0][9] != fbyte(9) ||
        munmap(m[0], PGSIZE) != 0) {
      fail(t, "round");
      return;
    }
  }
  f1 = countfree();
  if (f1 != f0) {
    printf("mmaptest: many: %d free pages before, %d after\n", f0, f1);
    fail(t, "pages leak");
  }

  // a mapping keeps an unlinked file alive.
  fd = open("mm.many", O_RDONLY);
  m[0] = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  unlink("mm.many");
  if (m[0] == MAP_FAILED || m[0][11] != fbyte(11))
    fail(t, "unlinked file");
  munmap(m[0], PGSIZE);
}

// System calls that copy into or out of unloaded mapped pages
// while they hold spinlocks.
void
lockedtest(void)
{
  char *t = "locked";
  int fd, pfd[2], i, bad = 0;
  char *m;

  makefile("mm.lock", 3 * PGSIZE);
  fd = open("mm.lock", O_RDWR);
  m = mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  // pipewrite copies page 0 in, piperead copies into page 1, both
  // holding the pipe's spinlock.
  pipe(pfd);
  if (write(pfd[1], m, 8) != 8 || read(pfd[0], m + PGSIZE, 8) != 8)
    fail(t, "pipe");
  close(pfd[0]);
  close(pfd[1]);
  // kwait copies the status into page 2 holding two spinlocks.
  if (fork() == 0)
    exit(7);
  wait((int *)(m + 2 * PGSIZE));
  if (*(int *)(m + 2 * PGSIZE) != 7)
    fail(t, "wait");
  if (munmap(m, 3 * PGSIZE) != 0) {
    fail(t, "munmap");
    return;
  }
  // what the kernel wrote into pages 1 and 2 went back to the file:
  // copyout marked them dirty.
  readat("mm.lock", PGSIZE, buf, 8);
  for (i = 0; i < 8; i++)
    bad += buf[i] != fbyte(i);
  readat("mm.lock", 2 * PGSIZE, buf, 4);
  bad += *(int *)buf != 7;
  if (bad)
    fail(t, "file contents");
  unlink("mm.lock");
}

// read() and write() of a file, with the buffer in an unloaded page
// of a mapping of that same file.
void
selftest(void)
{
  char *t = "self";
  int fd, fd2, i, bad = 0;
  char *m;

  makefile("mm.self", 3 * PGSIZE);
  fd = open("mm.self", O_RDWR);
  m = mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) {
    fail(t, "mmap");
    return;
  }
  // fileread holds the file's inode lock while it copies into page 1.
  fd2 = open("mm.self", O_RDONLY);
  if (read(fd2, m + PGSIZE, PGSIZE) != PGSIZE)
    fail(t, "read into the file's own mapping");
  close(fd2);
  // filewrite holds the inode lock and a transaction while it
  // copies from page 2; the bytes go to the start of the file.
  if (write(fd, m + 2 * PGSIZE, 16) != 16)
    fail(t, "write from the file's own mapping");
  close(fd);
  if (munmap(m, 3 * PGSIZE) != 0) {
    fail(t, "munmap");
    return;
  }
  // page 1 (written by read) went back; page 2 was only read, and
  // page 0 was never loaded: neither was written back.
  readat("mm.self", 0, buf, PGSIZE);
  for (i = 0; i < PGSIZE; i++)
    bad += buf[i] != (i < 16 ? fbyte(2 * PGSIZE + i) : fbyte(i));
  readat("mm.self", PGSIZE, buf, PGSIZE);
  for (i = 0; i < PGSIZE; i++)
    bad += buf[i] != fbyte(i);
  readat("mm.self", 2 * PGSIZE, buf, PGSIZE);
  for (i = 0; i < PGSIZE; i++)
    bad += buf[i] != fbyte(2 * PGSIZE + i);
  if (bad)
    fail(t, "file contents");
  unlink("mm.self");
}

#define NCHILD 3
#define PERCHILD 4
#define ROUNDS 30

// Each child, ROUNDS times: map the whole file shared, check every
// page, fill its own pages, unmap.
void
stresschild(char *arg)
{
  int c = arg[0] - '0', r, i, k, fd;
  int *m;

  for (r = 0; r < ROUNDS; r++) {
    fd = open("mm.stress", O_RDWR);
    m = mmap(0, NCHILD * PERCHILD * PGSIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
      exit(1);
    for (k = 0; k < NCHILD * PERCHILD; k++) {
      int *pg = m + k * PGSIZE / sizeof(int);
      for (i = 1; i < PGSIZE / sizeof(int); i++)
        if (pg[i] != pg[0])
          exit(2); // a page must never be half written
    }
    for (k = c * PERCHILD; k < (c + 1) * PERCHILD; k++)
      for (i = 0; i < PGSIZE / sizeof(int); i++)
        m[k * PGSIZE / sizeof(int) + i] = (c + 1) * 1000 + r;
    if (munmap(m, NCHILD * PERCHILD * PGSIZE) != 0)
      exit(3);
  }
  exit(0);
}

void
stresstest(void)
{
  char *t = "stress";
  int fd, c, k, xs, f0, f1, bad = 0;
  int *ibuf = (int *)buf;
  char arg[2];

  fd = open("mm.stress", O_CREATE | O_TRUNC | O_RDWR);
  memset(buf, 0, sizeof(buf));
  for (k = 0; k < NCHILD * PERCHILD; k++)
    write(fd, buf, PGSIZE);
  close(fd);
  countfree();
  f0 = countfree();
  for (c = 0; c < NCHILD; c++) {
    if (fork() == 0) {
      arg[0] = '0' + c;
      arg[1] = 0;
      stresschild(arg);
    }
  }
  for (c = 0; c < NCHILD; c++) {
    wait(&xs);
    if (xs != 0) {
      printf("mmaptest: stress: a child exited with %d\n", xs);
      bad++;
    }
  }
  fd = open("mm.stress", O_RDONLY);
  for (k = 0; k < NCHILD * PERCHILD; k++) {
    read(fd, buf, PGSIZE);
    for (c = 0; c < PGSIZE / sizeof(int); c++)
      bad += ibuf[c] != (k / PERCHILD + 1) * 1000 + ROUNDS - 1;
  }
  close(fd);
  if (bad)
    fail(t, "file contents");
  f1 = countfree();
  if (f1 != f0) {
    printf("mmaptest: stress: %d free pages before, %d after\n", f0, f1);
    fail(t, "pages leak");
  }
  unlink("mm.stress");
}

struct test {
  void (*f)(void);
  char *name;
} tests[] = {
  {readtest, "read"},       {sharedtest, "shared"}, {privatetest, "private"},
  {partialtest, "partial"}, {forktest, "fork"},     {exittest, "exit"},
  {manytest, "many"},       {lockedtest, "locked"}, {selftest, "self"},
  {stresstest, "stress"},
};

int
main(int argc, char *argv[])
{
  struct test *t;
  int ran = 0;

  if (argc > 1 && strcmp(argv[1], "nop") == 0)
    exit(0); // exittest's exec
  for (t = tests; t < &tests[sizeof(tests) / sizeof(tests[0])]; t++) {
    if (argc > 1 && strcmp(argv[1], t->name) != 0)
      continue;
    int before = failed;
    failed = 0;
    t->f();
    if (!failed)
      printf("mmaptest: %s: OK\n", t->name);
    failed |= before;
    ran++;
  }
  if (ran == 0) {
    printf("mmaptest: no test %s\n", argv[1]);
    exit(1);
  }
  if (argc == 1)
    printf(failed ? "mmaptest: SOME FAILED\n" : "mmaptest: ALL OK\n");
  exit(failed);
}
