// renametest: tests for rename().
//
//   renametest        functional tests, then seven processes renaming
//                     between two directories at once on all harts.
//   renametest concurrent N   only the concurrent test, N rounds.
//   renametest crash  renames back and forth until the machine is
//                     killed; renametest check, after a reboot, says
//                     whether every rename happened completely or not
//                     at all. A driver outside xv6 must do the killing.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"

static int failures;

static void
writefile(char *path, char *s)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  if (fd < 0 || write(fd, s, strlen(s)) != strlen(s)) {
    printf("renametest: cannot write %s\n", path);
    exit(1);
  }
  close(fd);
}

// Does the file hold exactly s?
static int
holds(char *path, char *s)
{
  char buf[64];
  int fd, n;

  if ((fd = open(path, O_RDONLY)) < 0)
    return 0;
  n = read(fd, buf, sizeof(buf));
  close(fd);
  return n == strlen(s) && memcmp(buf, s, n) == 0;
}

static int
exists(char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static uint
ino(char *path)
{
  struct stat st;
  if (stat(path, &st) < 0)
    return 0;
  return st.ino;
}

static int
nlink(char *path)
{
  struct stat st;
  if (stat(path, &st) < 0)
    return -1;
  return st.nlink;
}

// Record one check; print the first failure of each test.
static int bad;
static void
expect(int ok, char *what)
{
  if (!ok && bad++ == 0)
    printf("renametest: failed: %s\n", what);
}

static void
result(char *test)
{
  printf("renametest: %s: %s\n", test, bad ? "FAIL" : "OK");
  failures += bad != 0;
  bad = 0;
}

// Remove path and everything under it.
static void
rmrf(char *path)
{
  char buf[128], *p;
  struct dirent de;
  struct stat st;
  int fd;

  if (stat(path, &st) < 0)
    return;
  if (st.type == T_DIR && (fd = open(path, O_RDONLY)) >= 0) {
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0 || strcmp(de.name, ".") == 0 ||
          strcmp(de.name, "..") == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      rmrf(buf);
    }
    close(fd);
  }
  unlink(path);
}

static void
samedir(void)
{
  uint i;

  writefile("rt/f1", "one");
  i = ino("rt/f1");
  expect(rename("rt/f1", "rt/f2") == 0, "rename rt/f1 rt/f2");
  expect(!exists("rt/f1"), "rt/f1 still exists");
  expect(ino("rt/f2") == i, "rt/f2 is not the same inode");
  expect(nlink("rt/f2") == 1, "rt/f2 nlink is not 1");
  expect(holds("rt/f2", "one"), "rt/f2 lost its content");
  result("same directory");
}

static void
crossdir(void)
{
  uint i = ino("rt/f2");

  mkdir("rt/a");
  mkdir("rt/a/b");
  // down: the old parent is above the new one
  expect(rename("rt/f2", "rt/a/b/f") == 0, "rename rt/f2 rt/a/b/f");
  // sideways: unrelated parents
  mkdir("rt/c");
  expect(rename("rt/a/b/f", "rt/c/f") == 0, "rename rt/a/b/f rt/c/f");
  // up: the new parent is above the old one
  expect(rename("rt/c/f", "rt/f3") == 0, "rename rt/c/f rt/f3");
  expect(!exists("rt/f2") && !exists("rt/a/b/f") && !exists("rt/c/f"),
         "an old name still exists");
  expect(ino("rt/f3") == i && nlink("rt/f3") == 1, "rt/f3 inode or nlink");
  expect(holds("rt/f3", "one"), "rt/f3 lost its content");
  result("across directories");
}

static void
replace(void)
{
  struct stat st;
  char buf[8];
  uint i;
  int fd;

  writefile("rt/new", "new");
  writefile("rt/old", "old!");
  i = ino("rt/new");
  fd = open("rt/old", O_RDONLY); // keeps the replaced file alive
  expect(rename("rt/new", "rt/old") == 0, "rename rt/new rt/old");
  expect(!exists("rt/new"), "rt/new still exists");
  expect(ino("rt/old") == i && holds("rt/old", "new"), "rt/old is not new");
  expect(fstat(fd, &st) == 0 && st.nlink == 0, "replaced file nlink not 0");
  expect(read(fd, buf, 4) == 4 && memcmp(buf, "old!", 4) == 0,
         "replaced file unreadable while open");
  close(fd); // its last reference: the kernel frees it now

  // two names for one file: rename does nothing, successfully
  link("rt/old", "rt/h");
  expect(rename("rt/old", "rt/h") == 0, "rename of a file onto itself");
  expect(exists("rt/old") && exists("rt/h") && nlink("rt/h") == 2,
         "hard links changed");
  unlink("rt/h");
  result("replace a file");
}

static void
movedir(void)
{
  int na, nc, nd;

  mkdir("rt/a/d");
  mkdir("rt/a/d/e");
  na = nlink("rt/a"); // 1 + subdirectories b and d
  nc = nlink("rt/c");
  nd = nlink("rt/a/d");
  expect(rename("rt/a/d", "rt/c/d") == 0, "rename rt/a/d rt/c/d");
  expect(!exists("rt/a/d") && exists("rt/c/d/e"), "the subtree did not move");
  expect(ino("rt/c/d/..") == ino("rt/c"), "rt/c/d/.. is not rt/c");
  expect(ino("rt/c/d/e/..") == ino("rt/c/d"), "rt/c/d/e/.. is wrong");
  expect(nlink("rt/a") == na - 1, "old parent nlink");
  expect(nlink("rt/c") == nc + 1, "new parent nlink");
  expect(nlink("rt/c/d") == nd, "moved directory nlink");
  // within one directory nothing but the name changes
  expect(rename("rt/c/d", "rt/c/d2") == 0, "rename rt/c/d rt/c/d2");
  expect(ino("rt/c/d2/..") == ino("rt/c") && nlink("rt/c") == nc + 1,
         "rename within rt/c changed .. or nlink");
  result("move a directory");
}

static void
cycle(void)
{
  int nc = nlink("rt/c");

  expect(rename("rt/c", "rt/c/x") < 0, "moved rt/c into itself");
  expect(rename("rt/c", "rt/c/d2/e/x") < 0, "moved rt/c below itself");
  expect(rename("rt", "rt/a/b/x") < 0, "moved rt below itself");
  expect(exists("rt/c/d2/e") && nlink("rt/c") == nc, "rt/c changed");
  result("refuse a cycle");
}

static void
refusals(void)
{
  writefile("rt/g", "g");
  expect(rename("rt/c/.", "rt/x") < 0, "renamed rt/c/.");
  expect(rename("rt/c/..", "rt/x") < 0, "renamed rt/c/..");
  expect(rename("rt/g", "rt/c/..") < 0, "renamed onto rt/c/..");
  expect(rename("rt/g", "rt/c") < 0, "replaced a directory");
  expect(rename("rt/c", "rt/g") < 0, "replaced a file with a directory");
  expect(rename("rt/nothere", "rt/x") < 0, "renamed a missing file");
  expect(rename("/", "rt/x") < 0, "renamed /");
  expect(rename("rt/g", "rt/nothere/x") < 0, "renamed into a missing dir");
  expect(holds("rt/g", "g") && exists("rt/c/d2") && !exists("rt/x"),
         "a refused rename changed something");
  result("refusals");
}

static int rounds = 200;

// Seven processes on three harts. Two rename files between ct/a
// and ct/b in opposite directions; one renames between ct/a and
// its subdirectory ct/a/s; one moves the directory m between
// ct/a and ct/b, and each time also tries to move m's current
// parent into m, which must fail. Those four do a fixed number of
// rounds. Until they finish, one process keeps trying to unlink
// ct/a/s (which must fail: it is not empty), and two churn names
// in ct/a and ct/b (create, rename onto an existing file, rename
// away, unlink), so that names change under the other renames.
static void
concurrent(void)
{
  static char *moves[3][2] = {
    {"ct/a/x", "ct/b/y"},
    {"ct/b/z", "ct/a/w"},
    {"ct/a/s/q", "ct/a/q"},
  };
  int i, j, pid, xs, watchdog, t0, done, ok;

  rmrf("ct");
  mkdir("ct");
  mkdir("ct/s");
  mkdir("ct/a");
  mkdir("ct/b");
  rename("ct/s", "ct/a/s");
  writefile("ct/a/x", "x");
  writefile("ct/b/z", "z");
  writefile("ct/a/s/q", "q");
  writefile("ct/a/s/keep", "keep");
  mkdir("ct/a/m");
  expect(ino("ct/a/s") < ino("ct/a"), "setup: ct/a/s numbered after ct/a");

  t0 = uptime();

  watchdog = fork();
  if (watchdog == 0) {
    pause(3 * rounds); // 60 seconds for the usual 200 rounds
    printf("renametest: concurrent: not finished after %d seconds\n",
           3 * rounds / 10);
    exit(0);
  }
  for (i = 0; i < 7; i++) {
    if (fork() == 0) {
      int nbad = 0;
      if (i < 3) {
        for (j = 0; j < rounds; j++) {
          nbad += rename(moves[i][0], moves[i][1]) < 0;
          nbad += rename(moves[i][1], moves[i][0]) < 0;
        }
      } else if (i == 3) {
        for (j = 0; j < rounds; j++) {
          nbad += rename("ct/a/m", "ct/b/m") < 0;
          nbad += rename("ct/b", "ct/b/m/x") == 0; // a cycle
          nbad += rename("ct/b/m", "ct/a/m") < 0;
          nbad += rename("ct/a", "ct/a/m/x") == 0; // a cycle
        }
      } else if (i == 4) {
        while (!exists("ct/stop"))
          nbad += unlink("ct/a/s") == 0;
      } else {
        // churners: their own renames may fail when the other
        // churner got there first; only the totals are checked
        while (!exists("ct/stop")) {
          if (i == 5) {
            close(open("ct/a/t", O_CREATE | O_WRONLY));
            rename("ct/a/t", "ct/b/t");
          } else {
            rename("ct/b/t", "ct/a/u");
            unlink("ct/a/u");
          }
        }
      }
      exit(nbad);
    }
  }
  for (done = 0; done < 7;) {
    pid = wait(&xs);
    if (pid == watchdog) {
      watchdog = -1;
      continue;
    }
    expect(xs == 0, "a worker's rename or unlink did the wrong thing");
    if (++done == 4)
      writefile("ct/stop", ""); // the fixed-round workers are done
  }
  if (watchdog > 0) {
    kill(watchdog);
    wait(0);
  }
  expect(exists("ct/a/x") && exists("ct/b/z") && exists("ct/a/s/q") &&
           exists("ct/a/m"),
         "a file or directory is missing");
  expect(!exists("ct/b/y") && !exists("ct/a/w") && !exists("ct/a/q") &&
           !exists("ct/b/m"),
         "a file or directory has two names");
  // link counts: ct holds a and b, ct/a holds s and m, ct/b none
  expect(nlink("ct/a") == 3 && nlink("ct/b") == 1 && nlink("ct") == 3,
         "a directory's link count is wrong");
  expect(ino("ct/a/m/..") == ino("ct/a"), "ct/a/m/.. is not ct/a");
  ok = nlink("ct/a/x") == 1 && nlink("ct/b/z") == 1 && nlink("ct/a/s/q") == 1;
  expect(ok && (!exists("ct/b/t") || nlink("ct/b/t") == 1) &&
           (!exists("ct/a/t") || nlink("ct/a/t") == 1),
         "a file's link count is wrong");
  printf("renametest: concurrent: %d renames, %d refused cycles in %d ticks\n",
         8 * rounds, 2 * rounds, uptime() - t0);
  result("concurrent");
  rmrf("ct");
}

// Crash mode: f is a file and d a directory, each in cr/a or
// cr/b; each round moves both to the other directory.
static void
crash(void)
{
  char *fa = "cr/a/f", *fb = "cr/b/f", *da = "cr/a/d", *db = "cr/b/d";
  int n, fin, din;

  if (!exists("cr")) {
    mkdir("cr");
    mkdir("cr/a");
    mkdir("cr/b");
    writefile(fa, "crash");
    mkdir(da);
  }
  fin = exists(fa); // is f in cr/a?
  din = exists(da);
  printf("renametest: crash: renaming until killed\n");
  for (n = 1;; n++) {
    if (rename(fin ? fa : fb, fin ? fb : fa) < 0)
      printf("renametest: crash: cannot move f\n");
    fin = !fin;
    if (rename(din ? da : db, din ? db : da) < 0)
      printf("renametest: crash: cannot move d\n");
    din = !din;
    if (n % 50 == 0)
      printf("renametest: crash: %d rounds\n", n);
  }
}

static void
check(void)
{
  int fa = exists("cr/a/f"), fb = exists("cr/b/f");
  int da = exists("cr/a/d"), db = exists("cr/b/d");
  char *f = fa ? "cr/a/f" : "cr/b/f";

  expect(fa + fb == 1, "f has not exactly one name");
  expect(da + db == 1, "d has not exactly one name");
  expect(nlink(f) == 1 && holds(f, "crash"), "f nlink or content");
  expect(ino(da ? "cr/a/d/.." : "cr/b/d/..") == ino(da ? "cr/a" : "cr/b"),
         "d's .. is not its parent");
  expect(nlink("cr/a") == 1 + da && nlink("cr/b") == 1 + db,
         "parent nlink does not match where d is");
  printf("renametest: check: f in cr/%s (%d name%s), d in cr/%s (%d name%s)\n",
         fa ? "a" : "b", fa + fb, fa + fb == 1 ? "" : "s", da ? "a" : "b",
         da + db, da + db == 1 ? "" : "s");
  result("check");
}

int
main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "crash") == 0)
    crash();
  if (argc == 3 && strcmp(argv[1], "concurrent") == 0) {
    rounds = atoi(argv[2]);
    concurrent();
    exit(failures);
  }
  if (argc == 2 && strcmp(argv[1], "check") == 0) {
    check();
    exit(failures);
  }
  rmrf("rt");
  if (mkdir("rt") < 0) {
    printf("renametest: mkdir rt failed\n");
    exit(1);
  }
  samedir();
  crossdir();
  replace();
  movedir();
  cycle();
  refusals();
  concurrent();
  rmrf("rt");
  if (failures)
    printf("renametest: SOME TESTS FAILED\n");
  else
    printf("renametest: ALL OK (crash consistency: see renametest crash)\n");
  exit(failures);
}
