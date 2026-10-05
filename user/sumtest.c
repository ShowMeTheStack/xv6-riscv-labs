// sumtest: checks the kernel's direct copies to and from user
// memory (copyin, copyout and copyinstr with sstatus.SUM).
//
//   sumtest        run the checks
//   sumtest time   count copies per 20 ticks

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "user/user.h"

char data[100] = "initialized data, copied by the kernel";

static int
fail(char *what)
{
  printf("sumtest: %s\n", what);
  return 1;
}

// pass n bytes from src through a pipe into dst.
// returns read()'s result.
static int
viapipe(char *dst, char *src, int n)
{
  int fds[2], r;

  if (pipe(fds) < 0)
    return -1;
  if (write(fds[1], src, n) != n) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  r = read(fds[0], dst, n);
  close(fds[0]);
  close(fds[1]);
  return r;
}

// the kernel copies from and to the stack, data and heap.
int
copytest(void)
{
  char stack[100];
  struct stat st;
  char *heap = sbrk(2 * PGSIZE);
  char *edge = heap + PGSIZE - 3; // 6 bytes across a page boundary
  int fd;

  if (heap == SBRK_ERROR)
    return fail("sbrk failed");
  if (viapipe(stack, data, 100) != 100 || memcmp(stack, data, 100) != 0)
    return fail("data -> pipe -> stack");
  if (viapipe(heap, stack, 100) != 100 || memcmp(heap, data, 100) != 0)
    return fail("stack -> pipe -> heap");
  if (viapipe(edge, "abcdef", 6) != 6 || memcmp(edge, "abcdef", 6) != 0)
    return fail("pipe -> across a page boundary");

  fd = open("sumfile", O_CREATE | O_RDWR | O_TRUNC);
  if (fd < 0 || write(fd, edge, 6) != 6)
    return fail("write from across a page boundary");
  close(fd);
  fd = open("sumfile", O_RDONLY);
  if (fd < 0 || fstat(fd, &st) < 0 || st.size != 6)
    return fail("fstat");
  if (read(fd, stack, 6) != 6 || memcmp(stack, "abcdef", 6) != 0)
    return fail("file -> stack");
  close(fd);
  unlink("sumfile");
  sbrk(-2 * PGSIZE);
  return 0;
}

// pages reserved with sbrklazy: the kernel's own access takes
// the page fault.
int
lazytest(void)
{
  char *p = sbrklazy(3 * PGSIZE);
  char buf[8];
  struct stat st;
  int fd;

  if (p == SBRK_ERROR)
    return fail("sbrklazy failed");
  // copyout into a page nobody has touched
  if (viapipe(p + PGSIZE + 10, "hello", 6) != 6 ||
      strcmp(p + PGSIZE + 10, "hello") != 0)
    return fail("read() into an untouched lazy page");
  // copyin from a page nobody has touched: zeros
  memset(buf, 'x', sizeof(buf));
  if (viapipe(buf, p + 2 * PGSIZE, 8) != 8 ||
      memcmp(buf, "\0\0\0\0\0\0\0\0", 8) != 0)
    return fail("write() from an untouched lazy page");
  sbrklazy(-3 * PGSIZE);
  // copyinstr from an untouched page: the name "", which
  // open() takes to mean the current directory
  p = sbrklazy(PGSIZE);
  fd = open(p + 100, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) < 0 || st.type != T_DIR)
    return fail("open() of a name in an untouched lazy page");
  close(fd);
  sbrklazy(-PGSIZE);
  return 0;
}

// every one of these addresses must be refused, in both
// directions, and the kernel must survive.
int
badtest(void)
{
  char c;
  uint64 stackpage = PGROUNDDOWN((uint64)&c);
  uint64 end = (uint64)sbrk(0);
  struct {
    uint64 addr;
    char *what;
  } bad[] = {
    {0x80000000L, "kernel text (KERNBASE)"},
    {PHYSTOP - PGSIZE, "kernel RAM (PHYSTOP - PGSIZE)"},
    {0x0c000000L, "the PLIC"},
    {0x10000000L, "the UART"},
    {stackpage - PGSIZE, "the stack guard page"},
    {end, "the first byte beyond sbrk(0)"},
    {TRAPFRAME, "the trapframe"},
    {0xffffffffffffffffL, "the last address"},
  };
  int fd, fds[2], n, bads = 0;
  char buf[16];

  for (int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *a = (char *)bad[i].addr;

    // copyout
    fd = open("README", O_RDONLY);
    if ((n = read(fd, a, 16)) > 0) {
      printf("sumtest: read(fd, %p) into %s returned %d\n", a, bad[i].what, n);
      bads++;
    }
    close(fd);
    // copyin
    if (pipe(fds) < 0)
      return fail("pipe failed");
    if ((n = write(fds[1], a, 16)) > 0) {
      read(fds[0], buf, sizeof(buf));
      printf("sumtest: write(pipe, %p) from %s returned %d\n", a, bad[i].what,
             n);
      bads++;
    }
    close(fds[0]);
    close(fds[1]);
    // copyinstr
    if ((fd = open(a, O_RDONLY)) >= 0) {
      printf("sumtest: open(%p) of %s returned %d\n", a, bad[i].what, fd);
      close(fd);
      bads++;
    }
  }

  // a range that starts below sbrk(0) and ends beyond it: the
  // kernel copies the 1 valid byte, then stops
  if (pipe(fds) < 0)
    return fail("pipe failed");
  if ((n = write(fds[1], (char *)end - 1, 16)) > 1) {
    printf("sumtest: write(pipe, sbrk(0) - 1, 16) returned %d\n", n);
    bads++;
  }
  close(fds[0]);
  close(fds[1]);

  // text is readable but not writable
  fd = open("README", O_RDONLY);
  if ((n = read(fd, (char *)fail, 16)) > 0) {
    printf("sumtest: read() into the code of fail() returned %d\n", n);
    bads++;
  }
  close(fd);
  if (viapipe(buf, (char *)fail, 16) != 16 || memcmp(buf, fail, 16) != 0) {
    printf("sumtest: write() from the code of fail() failed\n");
    bads++;
  }
  return bads != 0;
}

// after sbrk(-n), the kernel must not reach the old pages.
int
shrinktest(void)
{
  char *a = sbrk(2 * PGSIZE);

  if (a == SBRK_ERROR)
    return fail("sbrk failed");
  if (viapipe(a + PGSIZE, "1234567", 8) != 8 || strcmp(a + PGSIZE, "1234567"))
    return fail("read() into new sbrk memory");
  sbrk(-2 * PGSIZE);
  if (viapipe(a + PGSIZE, "1234567", 8) > 0)
    return fail("read() into memory given back with sbrk(-n) succeeded");
  return 0;
}

// user memory ends at USERTOP, and the kernel can copy right
// up to it.
int
limittest(void)
{
  uint64 end = (uint64)sbrk(0);
  int n = USERTOP - end;
  char *last = (char *)USERTOP - 8;

  int r = 0;

  if (sbrklazy(n) == SBRK_ERROR)
    return fail("sbrklazy up to USERTOP failed");
  if (sbrklazy(1) != SBRK_ERROR || sbrk(1) != SBRK_ERROR)
    r = fail("sbrk beyond USERTOP succeeded");
  last[-1] = 'x'; // a lazy page fault from user mode
  if (viapipe(last, "1234567", 8) != 8 || strcmp(last, "1234567") != 0)
    r = fail("read() into the last 8 bytes below USERTOP");
  // only the 4 bytes below USERTOP can be copied
  if (viapipe(last + 4, "1234567", 8) != 4)
    r = fail("read() across USERTOP did not stop at USERTOP");
  sbrk(-n);
  return r;
}

// a forked child's kernel page table maps what it inherited.
int
forktest(void)
{
  char *heap = sbrk(PGSIZE);
  char stack[16];
  int pid, xstatus;

  strcpy(heap, "parent");
  strcpy(stack, "parent");
  if ((pid = fork()) < 0)
    return fail("fork failed");
  if (pid == 0) {
    if (viapipe(heap, "child", 6) != 6 || strcmp(heap, "child") != 0)
      exit(1);
    if (viapipe(stack, "child", 6) != 6 || strcmp(stack, "child") != 0)
      exit(1);
    exit(0);
  }
  if (wait(&xstatus) != pid || xstatus != 0)
    return fail("the child could not read() into inherited memory");
  if (strcmp(heap, "parent") != 0 || strcmp(stack, "parent") != 0)
    return fail("the child's copies changed the parent's memory");
  sbrk(-PGSIZE);
  return 0;
}

// count operations in 20 ticks (about 2 seconds).
static int
count(int (*op)(void))
{
  int n = 0, t0;

  t0 = uptime();
  while (uptime() == t0)
    ;
  t0 = uptime();
  while (uptime() - t0 < 20) {
    for (int i = 0; i < 10; i++)
      op();
    n += 10;
  }
  return n;
}

int timefd, timefds[2];
char timebuf[8192];

static int
opfstat(void)
{
  struct stat st;
  return fstat(timefd, &st);
}

static int
oppipe(void)
{
  write(timefds[1], timebuf, 512);
  return read(timefds[0], timebuf, 512);
}

static int
opread(void)
{
  int fd = open("sumtime", O_RDONLY);
  int n = read(fd, timebuf, sizeof(timebuf));
  close(fd);
  return n;
}

void
timing(void)
{
  int fd = open("sumtime", O_CREATE | O_RDWR | O_TRUNC);

  write(fd, timebuf, sizeof(timebuf));
  close(fd);
  timefd = open("sumtime", O_RDONLY);
  pipe(timefds);
  printf("sumtest: fstat (copyout 24 bytes): %d in 20 ticks\n",
         count(opfstat));
  printf("sumtest: pipe write+read of 512 bytes: %d in 20 ticks\n",
         count(oppipe));
  printf("sumtest: open+read 8192 bytes+close: %d in 20 ticks\n",
         count(opread));
  unlink("sumtime");
}

int
main(int argc, char *argv[])
{
  struct {
    int (*f)(void);
    char *name;
  } tests[] = {
    {copytest, "copy"},     {lazytest, "lazy"},   {badtest, "bad pointers"},
    {shrinktest, "shrink"}, {limittest, "limit"}, {forktest, "fork"},
  };
  int failed = 0;

  if (argc > 1 && strcmp(argv[1], "time") == 0) {
    timing();
    exit(0);
  }
  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int r = tests[i].f();
    printf("sumtest: %s: %s\n", tests[i].name, r ? "FAIL" : "OK");
    failed |= r;
  }
  printf("sumtest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
