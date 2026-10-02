//
// tests for demand-paged exec.
// each test prints "dexectest: <name>: OK" or "... FAIL".
// run all: dexectest; run one: dexectest <name>.
//

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/elf.h"
#include "kernel/vm.h"
#include "user/user.h"

#define NBIG 16 // pages in big[]
#define PERPAGE (PGSIZE / sizeof(int))
#define PAGE(k) (k * PERPAGE)

// 16 pages of initialized data. They are part of the file, so after
// exec each of them is loaded from the file when first touched.
// Every int is 0x5a5a5a5a except the first of each page, which is
// 1000 + the page number. Each test touches its own pages first.
int big[NBIG * PERPAGE] __attribute__((aligned(PGSIZE))) = {
  [0 ... NBIG * PERPAGE - 1] = 0x5a5a5a5a,
  [PAGE(0)] = 1000,  [PAGE(1)] = 1001,  [PAGE(2)] = 1002,
  [PAGE(3)] = 1003,  [PAGE(4)] = 1004,  [PAGE(5)] = 1005,
  [PAGE(6)] = 1006,  [PAGE(7)] = 1007,  [PAGE(8)] = 1008,
  [PAGE(9)] = 1009,  [PAGE(10)] = 1010, [PAGE(11)] = 1011,
  [PAGE(12)] = 1012, [PAGE(13)] = 1013, [PAGE(14)] = 1014,
  [PAGE(15)] = 1015,
};

// a page of read-only data, in the text segment.
const char ro[PGSIZE] __attribute__((aligned(PGSIZE))) = "read-only";

// bss: never written, must read as zeros.
char zeros[2 * PGSIZE];

char *self; // this program's file name, argv[0]
int failed;

void
result(char *name, int ok)
{
  printf("dexectest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

// does page k of big[] hold what the file says?
int
pageok(int k)
{
  int *q = &big[PAGE(k)];

  if (q[0] != 1000 + k)
    return 0;
  for (int i = 1; i < PERPAGE; i++)
    if (q[i] != 0x5a5a5a5a)
      return 0;
  return 1;
}

// count free pages by allocating them all with sbrk.
int
countfree(void)
{
  char *sz0 = sbrk(0);
  int n = 0;

  while (sbrk(PGSIZE) != SBRK_ERROR)
    n++;
  sbrk(-(sbrk(0) - sz0));
  return n;
}

// read this program's ELF header and program headers.
int
readphdrs(char *file, struct proghdr *ph, int max)
{
  struct elfhdr elf;
  int fd, n = 0;

  if ((fd = open(file, O_RDONLY)) < 0)
    return -1;
  if (read(fd, &elf, sizeof(elf)) != sizeof(elf) || elf.magic != ELF_MAGIC) {
    close(fd);
    return -1;
  }
  for (int i = 0; i < elf.phnum && n < max; i++) {
    // read() can't seek: reopen and skip to the i-th header.
    close(fd);
    fd = open(file, O_RDONLY);
    char skip[64];
    uint64 off = elf.phoff + i * sizeof(struct proghdr);
    while (off > 0) {
      int m = off > sizeof(skip) ? sizeof(skip) : off;
      if (read(fd, skip, m) != m)
        break;
      off -= m;
    }
    if (read(fd, &ph[n], sizeof(ph[n])) != sizeof(ph[n]))
      break;
    if (ph[n].type == ELF_PROG_LOAD)
      n++;
  }
  close(fd);
  return n;
}

// file offset of virtual address va, or -1.
int
fileoff(char *file, uint64 va)
{
  struct proghdr ph[4];
  int n = readphdrs(file, ph, 4);

  for (int i = 0; i < n; i++)
    if (va >= ph[i].vaddr && va < ph[i].vaddr + ph[i].filesz)
      return ph[i].off + (va - ph[i].vaddr);
  return -1;
}

// after exec, touching a page of the program costs one page,
// and only then.
void
lazytest(void)
{
  volatile int x;

  countfree(); // first call allocates page-table pages; ignore it
  int before = countfree();
  x = big[PAGE(1)];
  x = big[PAGE(2)];
  int after = countfree();
  (void)x;
  printf("dexectest: lazy: touching 2 data pages took %d free pages\n",
         before - after);
  result("lazy", before - after == 2 && pageok(1) && pageok(2));
}

// a child forked before a page is touched loads it from the same
// file.
void
forktest(void)
{
  int pid = fork();
  if (pid < 0) {
    printf("dexectest: fork failed\n");
    result("fork", 0);
    return;
  }
  if (pid == 0)
    exit(pageok(3) ? 0 : 1);
  int xstatus;
  wait(&xstatus);
  result("fork", xstatus == 0 && pageok(3));
}

// the kernel copies to and from untouched program pages while it
// holds spinlocks: pipewrite and piperead hold pi->lock, kwait holds
// wait_lock and the child's p->lock.
void
lockedtest(void)
{
  int fds[2];
  int *dst = &big[PAGE(4)];
  int *status = &big[PAGE(5)];
  int *src = &big[PAGE(8)];

  if (pipe(fds) < 0) {
    result("locked", 0);
    return;
  }
  int pid = fork();
  if (pid < 0) {
    result("locked", 0);
    return;
  }
  if (pid == 0) {
    int v[2] = {4004, 4005};
    write(fds[1], v, sizeof(v));
    exit(7);
  }
  int m = write(fds[1], src, 2 * sizeof(int)); // from page 8
  close(fds[1]);
  int n = 0, k;
  while (n < 4 * sizeof(int) &&
         (k = read(fds[0], (char *)dst + n, 4 * sizeof(int) - n)) > 0)
    n += k; // into page 4
  close(fds[0]);
  int w = wait(status); // into page 5
  // the pipe holds the child's two ints and page 8's first two, in
  // either order.
  int ok = (dst[0] == 4004 && dst[1] == 4005 && dst[2] == 1008 &&
            dst[3] == 0x5a5a5a5a) ||
           (dst[0] == 1008 && dst[1] == 0x5a5a5a5a && dst[2] == 4004 &&
            dst[3] == 4005);
  result("locked", m == 2 * sizeof(int) && n == 4 * sizeof(int) && ok &&
                     w == pid && *status == 7);
}

// read() from this program's own file into its own unloaded pages:
// fileread holds the file's inode lock while it copies.
void
selfreadtest(void)
{
  char *dst = (char *)&big[PAGE(6)];
  int fd = open(self, O_RDONLY);

  if (fd < 0) {
    printf("dexectest: cannot open %s\n", self);
    result("selfread", 0);
    return;
  }
  int n = read(fd, dst, PGSIZE); // page 6: data, writable
  close(fd);
  int ok = n == PGSIZE && *(uint *)dst == ELF_MAGIC;

  fd = open(self, O_RDONLY);
  int m = read(fd, (char *)ro, 16); // text: must fail
  close(fd);
  result("selfread", ok && m < 0 && strcmp(ro, "read-only") == 0);
}

// copy file src to dst.
int
copyfile(char *src, char *dst)
{
  char buf[512];
  int n, in, out;

  if ((in = open(src, O_RDONLY)) < 0)
    return -1;
  if ((out = open(dst, O_CREATE | O_WRONLY | O_TRUNC)) < 0) {
    close(in);
    return -1;
  }
  while ((n = read(in, buf, sizeof(buf))) > 0)
    if (write(out, buf, n) != n)
      break;
  close(in);
  close(out);
  return n == 0 ? 0 : -1;
}

// start a copy of this program as a helper that waits for a byte
// on its stdin, then checks the pages of big[] (see main).
// returns its pid; *go is the pipe to write the byte to.
int
starthelper(char *file, int *go)
{
  int down[2], up[2];
  char c;

  if (pipe(down) < 0 || pipe(up) < 0)
    return -1;
  int pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    close(0);
    dup(down[0]);
    close(1);
    dup(up[1]);
    close(down[0]);
    close(down[1]);
    close(up[0]);
    close(up[1]);
    exec(file, (char *[]){file, "helper", 0});
    exit(2);
  }
  close(down[0]);
  close(up[1]);
  // the helper writes one byte once it runs; until then we must
  // not change its file.
  if (read(up[0], &c, 1) != 1) {
    close(up[0]);
    close(down[1]);
    wait(0);
    return -1;
  }
  close(up[0]);
  *go = down[1];
  return pid;
}

// what the helper reports by its exit status, or -1 if killed.
int
finishhelper(int go)
{
  int xstatus;

  write(go, "g", 1);
  close(go);
  wait(&xstatus);
  return xstatus;
}

// the file of a running program is unlinked: the process keeps
// it alive, and its pages still load.
void
unlinktest(void)
{
  int go;

  if (copyfile(self, "dxcopy") < 0) {
    result("unlink", 0);
    return;
  }
  int pid = starthelper("dxcopy", &go);
  if (pid < 0) {
    result("unlink", 0);
    return;
  }
  int r = unlink("dxcopy");
  int st = finishhelper(go);
  result("unlink", r == 0 && st == 0 && open("dxcopy", O_RDONLY) < 0);
}

// the file of a running program is truncated: a page that is not
// loaded yet can't be read, so the process is killed.
void
truncatetest(void)
{
  int go;

  if (copyfile(self, "dxcopy") < 0) {
    result("truncate", 0);
    return;
  }
  int pid = starthelper("dxcopy", &go);
  if (pid < 0) {
    result("truncate", 0);
    return;
  }
  close(open("dxcopy", O_WRONLY | O_TRUNC));
  int st = finishhelper(go);
  unlink("dxcopy");
  result("truncate", st == -1);
}

// the file of a running program is overwritten: a page that is
// not loaded yet comes from the new contents.
void
modifytest(void)
{
  int go, fd;
  int v = 2007;

  if (copyfile(self, "dxcopy") < 0) {
    result("modify", 0);
    return;
  }
  int off = fileoff("dxcopy", (uint64)&big[PAGE(7)]);
  int pid = starthelper("dxcopy", &go);
  if (off < 0 || pid < 0) {
    result("modify", 0);
    return;
  }
  // overwrite the first int of page 7 in the file.
  fd = open("dxcopy", O_RDWR);
  char skip[512];
  for (int left = off; left > 0;) {
    int m = left > sizeof(skip) ? sizeof(skip) : left;
    read(fd, skip, m);
    left -= m;
  }
  write(fd, &v, sizeof(v));
  close(fd);
  int st = finishhelper(go);
  unlink("dxcopy");
  // the helper exits with 3 if it saw 2007 in page 7.
  result("modify", st == 3);
}

// sbrk(-n) below the end of the image and back again: the pages
// that come back must be zeros, not the file's bytes. The child
// gives back its own stack doing this, so from then on it calls
// only the system-call stubs, which use no stack, until it exits.
void
shrinktest(void)
{
  int pid = fork();
  if (pid < 0) {
    result("shrink", 0);
    return;
  }
  if (pid == 0) {
    int n = sbrk(0) - (char *)&big[PAGE(8)];
    sys_sbrk(-n, SBRK_EAGER); // give back pages 8-15, bss, stack
    sys_sbrk(n, SBRK_LAZY);   // take the addresses back, lazily
    exit(big[PAGE(9)]);       // 0 if page 9 came back zeroed
  }
  int xstatus;
  wait(&xstatus);
  result("shrink", xstatus == 0);
}

// the whole image, touched at last: the text is the file's bytes,
// with zeros after them, and the bss is zero.
void
contenttest(void)
{
  struct proghdr ph[4];
  char buf[512];
  int ok = 1;
  int n = readphdrs(self, ph, 4);

  if (n < 1)
    ok = 0;
  for (int i = 0; i < n; i++) {
    if (ph[i].flags & ELF_PROG_FLAG_WRITE)
      continue; // data: the program has written some of it
    // compare the segment with the file.
    int fd = open(self, O_RDONLY);
    uint64 left = ph[i].off;
    while (left > 0) {
      int m = left > sizeof(buf) ? sizeof(buf) : left;
      read(fd, buf, m);
      left -= m;
    }
    char *m = (char *)ph[i].vaddr;
    for (uint64 done = 0; done < ph[i].filesz;) {
      int k = ph[i].filesz - done > sizeof(buf) ? sizeof(buf)
                                                : ph[i].filesz - done;
      if (read(fd, buf, k) != k || memcmp(m + done, buf, k) != 0)
        ok = 0;
      done += k;
    }
    close(fd);
    // the rest of the last page is zero.
    for (uint64 a = ph[i].vaddr + ph[i].filesz;
         a < PGROUNDUP(ph[i].vaddr + ph[i].memsz); a++)
      if (*(char *)a != 0)
        ok = 0;
  }
  for (int k = 0; k < NBIG; k++)
    if (!pageok(k) && k != 4 && k != 5 && k != 6) // 4-6 were written
      ok = 0;
  for (int i = 0; i < sizeof(zeros); i++)
    if (zeros[i] != 0)
      ok = 0;
  result("content", ok);
}

struct test {
  char *name;
  void (*f)(void);
} tests[] = {
  {"lazy", lazytest},         {"fork", forktest},
  {"locked", lockedtest},     {"selfread", selfreadtest},
  {"unlink", unlinktest},     {"truncate", truncatetest},
  {"modify", modifytest},     {"shrink", shrinktest},
  {"content", contenttest},
};

int
main(int argc, char *argv[])
{
  self = argv[0];
  if (argc == 2 && strcmp(argv[1], "helper") == 0) {
    // started by starthelper: say we run, wait for the go byte,
    // then touch pages 7 to 15.
    char c;
    write(1, "r", 1);
    if (read(0, &c, 1) != 1)
      exit(1);
    if (big[PAGE(7)] == 2007)
      exit(3); // modifytest changed the file
    for (int k = 7; k < NBIG; k++)
      if (!pageok(k))
        exit(1);
    exit(0);
  }

  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    if (argc < 2 || strcmp(argv[1], tests[i].name) == 0)
      tests[i].f();
  printf("dexectest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
