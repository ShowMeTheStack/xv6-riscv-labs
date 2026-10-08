// Tests for symbolic links: symlink(), and open() following them.
// Every check is done by the program itself; each prints OK or FAIL.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/param.h"
#include "user/user.h"

static int failed;

static void
result(char *name, int ok)
{
  printf("symlinktest: %s: %s\n", name, ok ? "OK" : "FAIL");
  if (!ok)
    failed = 1;
}

// write s into a new (or truncated) file.
static int
mkfile(char *path, char *s)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  if (fd < 0)
    return -1;
  int n = write(fd, s, strlen(s));
  close(fd);
  return n == strlen(s) ? 0 : -1;
}

// does opening path (with flags) give exactly the bytes s?
static int
hasdata(char *path, int flags, char *s)
{
  char buf[64];
  int fd, n;

  if ((fd = open(path, O_RDONLY | flags)) < 0)
    return 0;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0)
    return 0;
  buf[n] = 0;
  return strcmp(buf, s) == 0;
}

// fstat of path opened with flags; -1 if it cannot be opened.
static int
statof(char *path, int flags, struct stat *st)
{
  int fd, r;

  if ((fd = open(path, O_RDONLY | flags)) < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

static void
cleanup(void)
{
  char name[32];
  int i;

  char *names[] = {"/stt/a", "/stt/l1", "/stt/l2", "/stt/rl", "/stt/rl2",
                   "/stt/later", "/stt/dl", "/stt/x", "/stt/y", "/stt/s",
                   "/stt/d/f", "/stt/d", "/stt/dlink", "/stt/sub", "/stt/b",
                   "/stt/lb", "/stt/f2", "/stt/tmp", "/stt/c/t", 0};
  for (i = 0; names[i]; i++)
    unlink(names[i]);
  for (i = 0; i <= MAXSYMLINK; i++) {
    strcpy(name, "/stt/c0");
    name[6] = '0' + i % 10;
    if (i == 10)
      strcpy(name, "/stt/c10");
    unlink(name);
  }
  for (i = 0; i < 4; i++) {
    strcpy(name, "/stt/c/l0");
    name[8] = '0' + i;
    unlink(name);
  }
  unlink("/stt/c");
  unlink("/stt");
}

static void
basic(void)
{
  struct stat st, sa;
  int fd, ok;

  ok = symlink("/stt/a", "/stt/l1") == 0 && hasdata("/stt/l1", 0, "hello");
  ok = ok && statof("/stt/l1", 0, &st) == 0 && statof("/stt/a", 0, &sa) == 0;
  ok = ok && st.type == T_FILE && st.ino == sa.ino;
  // writing through the link changes the target.
  if (ok && (fd = open("/stt/l1", O_WRONLY)) >= 0) {
    ok = write(fd, "HELLO", 5) == 5;
    close(fd);
  } else
    ok = 0;
  ok = ok && hasdata("/stt/a", 0, "HELLO");
  result("basic", ok);
}

static void
relative(void)
{
  int ok;

  // "a" means a in the link's directory /stt, not in the
  // current directory /, where there is no a.
  ok = symlink("a", "/stt/rl") == 0 && open("/a", O_RDONLY) < 0;
  ok = ok && hasdata("/stt/rl", 0, "HELLO");
  ok = ok && mkdir("/stt/sub") == 0 && chdir("/stt/sub") == 0;
  ok = ok && hasdata("../rl", 0, "HELLO");
  chdir("/");
  result("relative", ok);
}

static void
dangling(void)
{
  int ok;

  ok = symlink("/stt/later", "/stt/dl") == 0 && open("/stt/dl", O_RDONLY) < 0;
  ok = ok && mkfile("/stt/later", "later") == 0 && hasdata("/stt/dl", 0, "later");
  result("dangling", ok);
}

static void
nofollow(void)
{
  struct stat st, sa;
  int ok;

  ok = statof("/stt/l1", O_NOFOLLOW, &st) == 0;
  ok = ok && st.type == T_SYMLINK && st.size == 6 && st.nlink == 1;
  ok = ok && hasdata("/stt/l1", O_NOFOLLOW, "/stt/a");
  ok = ok && open("/stt/l1", O_RDWR | O_NOFOLLOW) < 0;
  // the target's link count does not count symbolic links.
  ok = ok && statof("/stt/a", 0, &sa) == 0 && sa.nlink == 1;
  result("nofollow", ok);
}

static void
chain(void)
{
  char name[16], target[16];
  int i, ok = 1;

  // c10 -> /stt/a, c9 -> c10, ..., c0 -> c1.
  for (i = 0; i <= MAXSYMLINK; i++) {
    strcpy(name, "/stt/c0");
    name[6] = '0' + i;
    strcpy(target, "c1");
    target[1] = '0' + i + 1;
    if (i == 9)
      strcpy(target, "c10");
    if (i == 10) {
      strcpy(name, "/stt/c10");
      strcpy(target, "/stt/a");
    }
    if (symlink(target, name) < 0)
      ok = 0;
  }
  // from c1, MAXSYMLINK links lead to a; from c0, one too many.
  ok = ok && hasdata("/stt/c1", 0, "HELLO");
  ok = ok && open("/stt/c0", O_RDONLY) < 0;
  ok = ok && symlink("/stt/l1", "/stt/l2") == 0 && hasdata("/stt/l2", 0, "HELLO");
  result("chain", ok);
}

static void
cycle(void)
{
  int ok;

  ok = symlink("y", "/stt/x") == 0 && symlink("x", "/stt/y") == 0;
  ok = ok && open("/stt/x", O_RDONLY) < 0;
  ok = ok && symlink("s", "/stt/s") == 0 && open("/stt/s", O_RDONLY) < 0;
  ok = ok && open("/stt/s", O_CREATE | O_RDWR) < 0;
  result("cycle", ok);
}

static void
directory(void)
{
  struct stat st, sd;
  struct dirent de;
  int fd, found = 0, ok;

  ok = mkdir("/stt/d") == 0 && mkfile("/stt/d/f", "f") == 0;
  ok = ok && symlink("d", "/stt/dlink") == 0;
  ok = ok && statof("/stt/dlink", 0, &st) == 0 && statof("/stt/d", 0, &sd) == 0;
  ok = ok && st.type == T_DIR && st.ino == sd.ino;
  if (ok && (fd = open("/stt/dlink", O_RDONLY)) >= 0) {
    while (read(fd, &de, sizeof(de)) == sizeof(de))
      if (de.inum != 0 && strcmp(de.name, "f") == 0)
        found = 1;
    close(fd);
  }
  ok = ok && found;
  ok = ok && open("/stt/dlink", O_RDWR) < 0;
  result("directory", ok);

  // only open follows, and only the last name in the path.
  ok = open("/stt/dlink/f", O_RDONLY) < 0 && chdir("/stt/dlink") < 0;
  result("not followed in the middle of a path or by chdir", ok);
}

static void
createthrough(void)
{
  int ok;

  // O_CREATE through a dangling link does not create the target.
  ok = symlink("b", "/stt/lb") == 0;
  ok = ok && open("/stt/lb", O_CREATE | O_WRONLY) < 0 && open("/stt/b", 0) < 0;
  ok = ok && mkfile("/stt/b", "old") == 0 && mkfile("/stt/lb", "bb") == 0;
  ok = ok && hasdata("/stt/b", 0, "bb");
  result("create through a link", ok);
}

static void
unlinks(void)
{
  struct stat st, sa;
  int ok, ino;

  // unlink removes the link, not the target.
  ok = unlink("/stt/l1") == 0 && open("/stt/l1", O_RDONLY) < 0;
  ok = ok && statof("/stt/a", 0, &sa) == 0 && sa.nlink == 1;
  ok = ok && hasdata("/stt/a", 0, "HELLO");
  result("unlink", ok);

  // a hard link to a symbolic link counts in the link's nlink.
  ok = link("/stt/rl", "/stt/rl2") == 0;
  ok = ok && statof("/stt/rl2", O_NOFOLLOW, &st) == 0;
  ok = ok && st.type == T_SYMLINK && st.nlink == 2;
  ok = ok && unlink("/stt/rl") == 0 && hasdata("/stt/rl2", 0, "HELLO");
  result("hard link to a symlink", ok);

  // the inode of an unlinked symlink is freed: the next file gets it.
  ok = symlink("/stt/a", "/stt/tmp") == 0;
  ok = ok && statof("/stt/tmp", O_NOFOLLOW, &st) == 0;
  ino = st.ino;
  ok = ok && unlink("/stt/tmp") == 0 && mkfile("/stt/f2", "x") == 0;
  ok = ok && statof("/stt/f2", 0, &st) == 0 && st.ino == ino;
  result("unlinked symlink's inode freed", ok);
}

static void
errors(void)
{
  int ok;

  ok = symlink("", "/stt/e") < 0;
  ok = ok && symlink("/stt/x", "/stt/a") < 0;
  ok = ok && symlink("/stt/a", "/stt/nodir/x") < 0;
  result("errors", ok);
}

// 3 processes create, remove and open the links /stt/c/l0..l3,
// whose targets are t (a file) or another of the links. Each must
// have reached t through links, and read links, at least once.
static int
worker(int id, int rounds, int out)
{
  char name[16], target[16], buf[16];
  struct stat st;
  unsigned int r = id * 7919 + 1;
  int i, fd, n, made = 0, gone = 0, followed = 0, seen = 0;

  for (i = 0; i < rounds; i++) {
    r = r * 1103515245 + 12345;
    strcpy(name, "/stt/c/l0");
    name[8] = '0' + (r >> 8) % 4;
    switch ((r >> 16) % 4) {
    case 0:
      if ((r >> 20) % 3 == 0)
        strcpy(target, "t");
      else {
        strcpy(target, "l0");
        target[1] = '0' + (r >> 24) % 4;
      }
      if (symlink(target, name) == 0)
        made++;
      break;
    case 1:
      if (unlink(name) == 0)
        gone++;
      break;
    case 2: // following must end at t, or fail
      if ((fd = open(name, O_RDONLY)) >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        if (fstat(fd, &st) < 0 || st.type != T_FILE || n != 6 ||
            memcmp(buf, "target", 6) != 0) {
          printf("worker %d: bad follow of %s\n", id, name);
          return 1;
        }
        followed++;
        close(fd);
      }
      break;
    case 3: // the link itself must hold a whole target
      if ((fd = open(name, O_RDONLY | O_NOFOLLOW)) >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        buf[n < 0 ? 0 : n] = 0;
        if (fstat(fd, &st) < 0 || st.type != T_SYMLINK ||
            (strcmp(buf, "t") != 0 &&
             (n != 2 || buf[0] != 'l' || buf[1] < '0' || buf[1] > '3'))) {
          printf("worker %d: bad link %s: '%s'\n", id, name, buf);
          return 1;
        }
        seen++;
        close(fd);
      }
      break;
    }
  }
  // one small write into a pipe is not interleaved with others.
  int counts[5] = {id, made, gone, followed, seen};
  if (write(out, counts, sizeof(counts)) != sizeof(counts))
    return 1;
  // a run in which nothing was followed or read proves nothing.
  return followed > 0 && seen > 0 ? 0 : 1;
}

static void
concurrent(int rounds)
{
  int i, pid, xs, ok, fds[2], c[5];

  ok = mkdir("/stt/c") == 0 && mkfile("/stt/c/t", "target") == 0;
  ok = ok && pipe(fds) == 0;
  for (i = 0; ok && i < 3; i++) {
    pid = fork();
    if (pid < 0)
      ok = 0;
    if (pid == 0) {
      close(fds[0]);
      exit(worker(i, rounds, fds[1]));
    }
  }
  close(fds[1]);
  for (i = 0; i < 3; i++) {
    if (wait(&xs) < 0 || xs != 0)
      ok = 0;
  }
  while (read(fds[0], c, sizeof(c)) == sizeof(c))
    printf("symlinktest: worker %d: %d links made, %d removed, "
           "%d opened through links, %d links read\n",
           c[0], c[1], c[2], c[3], c[4]);
  close(fds[0]);
  result("concurrent", ok);
}

int
main(int argc, char *argv[])
{
  int rounds = 2000;

  if (argc > 1)
    rounds = atoi(argv[1]);
  chdir("/");
  cleanup();
  if (mkdir("/stt") < 0 || mkfile("/stt/a", "hello") < 0) {
    printf("symlinktest: cannot set up /stt\n");
    exit(1);
  }
  basic();
  relative();
  dangling();
  nofollow();
  chain();
  cycle();
  directory();
  createthrough();
  unlinks();
  errors();
  concurrent(rounds);
  cleanup();
  printf("symlinktest: %s\n", failed ? "SOME TESTS FAILED" : "ALL OK");
  exit(failed);
}
