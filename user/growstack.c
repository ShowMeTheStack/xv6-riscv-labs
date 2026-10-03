// growstack: tests for the growable user stack.
// growstack runs every check; growstack NAME runs one.
// Each check runs in a child, so that the stack it grows,
// or the kill it expects, does not touch this process.

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "user/user.h"

#define FRAME 1024 // bytes of local array in each recursive call

int failed;
uint64 lowest; // lowest sp seen by dive()
int report = -1; // if >= 0, dive() writes its sp to this fd

void forkbottom(void);

// Recurse depth more levels, each with FRAME bytes of locals filled
// with its own depth; check them on the way back. At the bottom,
// call forkbottom() if dofork is set. Returns 1 if every array
// was intact.
int
dive(int depth, int dofork)
{
  volatile char a[FRAME];
  uint64 sp = r_sp();
  int ok;

  for (int i = 0; i < FRAME; i++)
    a[i] = depth;
  if (sp < lowest)
    lowest = sp;
  if (report >= 0)
    write(report, &sp, sizeof(sp));
  if (depth > 0)
    ok = dive(depth - 1, dofork);
  else {
    ok = 1;
    if (dofork)
      forkbottom();
  }
  for (int i = 0; i < FRAME; i++) {
    if (a[i] != (char)depth)
      ok = 0;
    a[i] = ~depth; // scribble, so a shared copy would show
  }
  return ok;
}

// run f in a child; return its exit status.
int
inchild(void (*f)(void))
{
  int pid = fork();
  if (pid < 0) {
    printf("growstack: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    f();
    exit(0);
  }
  int status;
  wait(&status);
  return status;
}

void
deep1(void)
{
  lowest = USTACKTOP;
  int ok = dive(64, 0);
  printf("growstack: deep: 65 calls used %ld bytes of stack\n",
         USTACKTOP - lowest);
  exit(ok && USTACKTOP - lowest > 64 * FRAME ? 0 : 1);
}

// deep: recursion far beyond one page of stack.
void
deep(void)
{
  int st = inchild(deep1);
  printf("growstack: deep: %s\n", st == 0 ? "OK" : "FAIL");
  if (st != 0)
    failed = 1;
}

int forkchild;

void
forkbottom(void)
{
  int pid = fork();
  if (pid < 0) {
    printf("growstack: fork: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    forkchild = 1;
    return; // the child unwinds its copy, checking and scribbling
  }
  int status;
  wait(&status); // the parent unwinds only after the child is done
  if (status != 0) {
    printf("growstack: fork: child's copy of the stack was wrong\n");
    exit(1);
  }
}

void
fork1(void)
{
  int ok = dive(64, 1);
  if (forkchild)
    exit(ok ? 0 : 1);
  if (!ok)
    printf("growstack: fork: parent's stack changed under it\n");
  exit(ok ? 0 : 1);
}

// fork: fork with 65 frames on the stack. The child checks every
// frame and scribbles over it; then the parent checks its own.
void
forktest(void)
{
  int st = inchild(fork1);
  printf("growstack: fork: %s\n", st == 0 ? "OK" : "FAIL");
  if (st != 0)
    failed = 1;
}

// read into buf, which lies up to 4 pages below the caller's
// frame: nothing has touched those pages, so only the kernel's
// copyout() can grow the stack to them. b is 50 bytes below a page
// boundary inside buf, so the 100 bytes land in two such pages.
int
readlow(int fd)
{
  char buf[4 * PGSIZE];
  char *b = (char *)PGROUNDUP((uint64)buf + PGSIZE) - 50;

  if (read(fd, b, 100) != 100)
    return 0;
  for (int i = 0; i < 100; i++)
    if (b[i] != (char)i)
      return 0;
  return 1;
}

void
read1(void)
{
  int fds[2];
  char data[100];

  for (int i = 0; i < 100; i++)
    data[i] = i;
  if (pipe(fds) < 0 || write(fds[1], data, 100) != 100)
    exit(1);
  exit(readlow(fds[0]) ? 0 : 1);
}

// read: read() into stack pages nobody has touched.
void
readtest(void)
{
  int st = inchild(read1);
  printf("growstack: read: %s\n", st == 0 ? "OK" : "FAIL");
  if (st != 0)
    failed = 1;
}

void
forever(void)
{
  dive(1 << 20, 0);
}

// overflow: unbounded recursion must be killed at the limit, not
// before it, and not below it. The child reports every frame's sp
// through a pipe; the last one is the deepest frame it reached.
void
overflow(void)
{
  int fds[2];
  uint64 sp, last = 0;

  if (pipe(fds) < 0) {
    printf("growstack: overflow: pipe failed\n");
    exit(1);
  }
  int pid = fork();
  if (pid == 0) {
    close(fds[0]);
    report = fds[1];
    forever();
    exit(0);
  }
  close(fds[1]);
  while (read(fds[0], &sp, sizeof(sp)) == sizeof(sp))
    last = sp;
  close(fds[0]);
  int status;
  wait(&status);
  // the frame below the last one did not fit above USTACKBASE.
  int ok = status == -1 && last >= USTACKBASE &&
           last < USTACKBASE + FRAME + 128;
  printf("growstack: overflow: status %d, deepest sp 0x%lx, limit 0x%lx: %s\n",
         status, last, USTACKBASE, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

int collidefd;

void
collide1(void)
{
  // grow the heap to MAXHEAP, lazily, in steps sbrk's int can take.
  uint64 top = (uint64)sbrk(0);
  while (top < MAXHEAP) {
    uint64 n = MAXHEAP - top;
    if (n > (1 << 30))
      n = 1 << 30;
    if (sbrklazy(n) == SBRK_ERROR)
      exit(1);
    top = (uint64)sbrk(0);
  }
  if (top != MAXHEAP)
    exit(2);
  if (sbrklazy(1) != SBRK_ERROR || sbrk(PGSIZE) != SBRK_ERROR)
    exit(3); // the heap must not grow past MAXHEAP
  *(volatile char *)(MAXHEAP - 1) = 1; // the heap's last byte works
  write(collidefd, "h", 1);
  *(volatile char *)MAXHEAP = 1; // the guard page: must be killed
  exit(4);
}

// collide: the heap grows up to MAXHEAP and no further, and the
// guard page between heap and stack kills.
void
collide(void)
{
  int fds[2];
  char c = 0;

  if (pipe(fds) < 0) {
    printf("growstack: collide: pipe failed\n");
    exit(1);
  }
  collidefd = fds[1];
  int st = inchild(collide1);
  close(fds[1]);
  read(fds[0], &c, 1);
  close(fds[0]);
  int ok = c == 'h' && st == -1;
  printf("growstack: collide: %s\n", ok ? "OK" : "FAIL");
  if (!ok) {
    printf("growstack: collide: heap checks %s, exit status %d\n",
           c == 'h' ? "passed" : "failed", st);
    failed = 1;
  }
}

struct test {
  void (*f)(void);
  char *name;
} tests[] = {
  {deep, "deep"},       {forktest, "fork"}, {readtest, "read"},
  {overflow, "overflow"}, {collide, "collide"},
};

int
main(int argc, char *argv[])
{
  int ran = 0;

  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (argc > 1 && strcmp(argv[1], tests[i].name) != 0)
      continue;
    tests[i].f();
    ran++;
  }
  if (ran == 0) {
    printf("usage: growstack [deep|fork|read|overflow|collide]\n");
    exit(1);
  }
  printf(failed ? "growstack: SOME FAILED\n" : "growstack: ALL OK\n");
  exit(failed);
}
