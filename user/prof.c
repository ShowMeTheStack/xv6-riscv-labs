// prof: run a command under the sampling profiler, then show
// where its samples landed. the report goes to the standard
// error, so the command's own output can be redirected.
//
//   prof command args...         the command's user pcs
//   prof -k command args...      the kernel's pcs, on all harts,
//                                while the command runs
//   prof -k 0xLO 0xHI command... only kernel pcs in [LO, HI)

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/elf.h"
#include "kernel/prof.h"
#include "user/user.h"

#define NSYM 512
#define NTOP 10

static struct prof pr;
static uint64 symaddr[NSYM]; // the program's functions, from NAME.sym
static char *symname[NSYM];
static int nsym;
static uint symcount[NSYM]; // samples per function

// find the executable segment [*lo, *hi) in path's ELF headers.
int
textrange(char *path, uint64 *lo, uint64 *hi)
{
  uint64 buf[64]; // the first 512 bytes; uint64 for alignment
  struct elfhdr *eh = (struct elfhdr *)buf;
  struct proghdr *ph;
  int fd, n;

  if ((fd = open(path, O_RDONLY)) < 0)
    return -1;
  n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n < (int)sizeof(*eh) || eh->magic != ELF_MAGIC ||
      eh->phoff + eh->phnum * sizeof(*ph) > n)
    return -1;
  for (int i = 0; i < eh->phnum; i++) {
    ph = (struct proghdr *)((char *)buf + eh->phoff) + i;
    if (ph->type == ELF_PROG_LOAD && (ph->flags & ELF_PROG_FLAG_EXEC)) {
      *lo = ph->vaddr;
      *hi = ph->vaddr + ph->memsz;
      return 0;
    }
  }
  return -1;
}

int
hexdigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// parse a hex number such as 0x80000b4a.
uint64
hex(char *s)
{
  uint64 n = 0;

  if (s[0] == '0' && s[1] == 'x')
    s += 2;
  while (hexdigit(*s) >= 0)
    n = n * 16 + hexdigit(*s++);
  return n;
}

// read /NAME.sym, written by the Makefile: lines of
// "0000000000000058 matchhere". skip sections and files.
void
loadsyms(char *path)
{
  char file[32], *s, *buf;
  struct stat st;
  int fd, n;

  s = path + strlen(path);
  while (s > path && s[-1] != '/')
    s--;
  if (strlen(s) + 6 > sizeof(file))
    return;
  file[0] = '/'; // the Makefile puts them in the root directory
  strcpy(file + 1, s);
  strcpy(file + strlen(file), ".sym");
  if ((fd = open(file, O_RDONLY)) < 0)
    return;
  if (fstat(fd, &st) < 0 || (buf = malloc(st.size + 1)) == 0) {
    close(fd);
    return;
  }
  n = read(fd, buf, st.size);
  close(fd);
  if (n < 0)
    return;
  buf[n] = 0;

  for (s = buf; *s && nsym < NSYM;) {
    uint64 a = 0;
    char *name;
    while (hexdigit(*s) >= 0)
      a = a * 16 + hexdigit(*s++);
    if (*s == ' ')
      s++;
    name = s;
    while (*s && *s != '\n')
      s++;
    if (*s)
      *s++ = 0;
    if (*name && strchr(name, '.') == 0) {
      symaddr[nsym] = a;
      symname[nsym++] = name;
    }
  }
}

// which function contains a? -1 if none is known.
int
lookup(uint64 a)
{
  int best = -1;

  for (int i = 0; i < nsym; i++)
    if (symaddr[i] <= a && (best < 0 || symaddr[i] > symaddr[best]))
      best = i;
  return best;
}

// the index of the largest of n counts not yet printed (used[i] == 0).
int
biggest(uint *count, char *used, int n)
{
  int best = -1;

  for (int i = 0; i < n; i++)
    if (!used[i] && count[i] > 0 && (best < 0 || count[i] > count[best]))
      best = i;
  if (best >= 0)
    used[best] = 1;
  return best;
}

void
report(char *cmd)
{
  static char used[PROFBUCKETS];
  int in = pr.nsample - pr.nout;

  fprintf(2, "prof: %s: %d samples, %d outside [0x%lx, 0x%lx), %d-byte buckets\n",
         cmd, pr.nsample, pr.nout, pr.lo, pr.hi, 1 << pr.shift);
  if (in == 0)
    return;

  if (nsym > 0) {
    for (int i = 0; i < pr.nbucket; i++) {
      int f = lookup(pr.lo + ((uint64)i << pr.shift));
      if (f >= 0)
        symcount[f] += pr.bucket[i];
    }
    fprintf(2, "by function:\n");
    for (int k = 0, f; k < NTOP && (f = biggest(symcount, used, nsym)) >= 0;
         k++)
      fprintf(2, "  %d\t%d%%\t%s\n", symcount[f], symcount[f] * 100 / in,
             symname[f]);
    memset(used, 0, sizeof(used));
  }

  fprintf(2, "hottest buckets:\n");
  for (int k = 0, b; k < NTOP && (b = biggest(pr.bucket, used, pr.nbucket)) >= 0;
       k++) {
    uint64 a = pr.lo + ((uint64)b << pr.shift);
    int f = lookup(a);
    fprintf(2, "  %d\t%d%%\t0x%lx", pr.bucket[b], pr.bucket[b] * 100 / in, a);
    if (f >= 0)
      fprintf(2, "\t%s+0x%lx", symname[f], a - symaddr[f]);
    fprintf(2, "\n");
  }
}

// where is the symbol called name? 0 if unknown.
uint64
symbol(char *name)
{
  for (int i = 0; i < nsym; i++)
    if (strcmp(symname[i], name) == 0)
      return symaddr[i];
  return 0;
}

int
main(int argc, char *argv[])
{
  uint64 lo, hi;
  int shift, fd, kernel = 0, t0;
  char **cmd = argv + 1;

  if (argc >= 3 && strcmp(argv[1], "-k") == 0) {
    // profile the kernel, using kernel.sym, a copy of the
    // kernel's symbol table that the Makefile puts in fs.img.
    kernel = 1;
    cmd = argv + 2;
    loadsyms("kernel");
    lo = symbol("_entry");
    hi = symbol("etext");
    if (argc >= 5 && cmd[0][0] == '0' && cmd[0][1] == 'x') {
      lo = hex(cmd[0]);
      hi = hex(cmd[1]);
      cmd += 2;
    }
    if (lo == 0 || hi <= lo) {
      fprintf(2, "prof: no kernel range\n");
      exit(1);
    }
  } else if (argc < 2) {
    fprintf(2, "usage: prof [-k [0xLO 0xHI]] command args...\n");
    exit(1);
  } else if (textrange(cmd[0], &lo, &hi) < 0) {
    fprintf(2, "prof: cannot read %s\n", cmd[0]);
    exit(1);
  }

  // the smallest buckets that cover the range; at least 2 bytes,
  // the size of the smallest (compressed) instruction.
  for (shift = 1; ((hi - lo - 1) >> shift) >= PROFBUCKETS; shift++)
    ;

  unlink("prof.out");
  t0 = uptime();
  if (fork() == 0) {
    if (profile(lo, hi, shift) < 0) {
      fprintf(2, "prof: profile failed\n");
      exit(1);
    }
    exec(cmd[0], cmd);
    fprintf(2, "prof: exec %s failed\n", cmd[0]);
    exit(1);
  }
  wait(0);
  fprintf(2, "prof: %d ticks\n", uptime() - t0);

  if ((fd = open("prof.out", O_RDONLY)) < 0 ||
      read(fd, &pr, sizeof(pr)) != sizeof(pr)) {
    fprintf(2, "prof: no prof.out\n");
    exit(1);
  }
  close(fd);
  if (!kernel)
    loadsyms(cmd[0]);
  report(kernel ? "kernel" : cmd[0]);
  exit(0);
}
