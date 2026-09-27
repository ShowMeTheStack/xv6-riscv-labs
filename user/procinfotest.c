// Tests for the procinfo system call.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "user/user.h"

#define USED     1
#define SLEEPING 2
#define RUNNING  4
#define ZOMBIE   5

struct pinfo buf[NPROC + 1];
int fails, checks;

void
check(int ok, char *what)
{
  printf("procinfotest: %s: %s\n", what, ok ? "OK" : "FAIL");
  checks++;
  if (!ok)
    fails++;
}

// the record for pid in buf[0..n-1], or 0.
struct pinfo *
find(int pid, int n)
{
  for (int i = 0; i < n; i++)
    if (buf[i].pid == pid)
      return &buf[i];
  return 0;
}

// the snapshot's own record: we are running, on some hart.
void
selftest(void)
{
  int n = procinfo(buf, NPROC);
  struct pinfo *me = find(getpid(), n);

  check(n >= 3 && me != 0 && me->state == RUNNING && me->cpu >= 0 &&
          me->cpu < NCPU && strcmp(me->name, "procinfotest") == 0,
        "own record: running on a hart, right name");
}

// a child blocked in read shows as SLEEPING with us as parent,
// then as a ZOMBIE until we wait, then not at all.
void
childtest(void)
{
  int fds[2], pid, n, sawsleep, sawzombie, gone;
  struct pinfo *c;
  char ch;

  pipe(fds);
  pid = fork();
  if (pid == 0) {
    read(fds[0], &ch, 1);
    exit(0);
  }
  sawsleep = 0;
  for (int i = 0; i < 100 && !sawsleep; i++) {
    n = procinfo(buf, NPROC);
    c = find(pid, n);
    sawsleep = c && c->state == SLEEPING && c->ppid == getpid() &&
               c->cpu == -1;
    if (!sawsleep)
      pause(1);
  }
  write(fds[1], "x", 1);
  sawzombie = 0;
  for (int i = 0; i < 100 && !sawzombie; i++) {
    n = procinfo(buf, NPROC);
    c = find(pid, n);
    sawzombie = c && c->state == ZOMBIE && c->ppid == getpid();
    if (!sawzombie)
      pause(1);
  }
  wait(0);
  n = procinfo(buf, NPROC);
  gone = find(pid, n) == 0;
  check(sawsleep && sawzombie && gone,
        "child: sleeping, then zombie, then gone after wait");
  close(fds[0]);
  close(fds[1]);
}

// max limits both what is written and what is returned.
void
maxtest(void)
{
  int n1, n0, pid0, changed = 0, ok;

  memset(buf, 0xab, sizeof(buf));
  n1 = procinfo(buf, 1);
  pid0 = buf[0].pid;
  for (char *b = (char *)&buf[1]; b < (char *)&buf[NPROC + 1]; b++)
    if (*b != (char)0xab)
      changed++;
  memset(buf, 0xab, sizeof(buf));
  n0 = procinfo(buf, 0);
  ok = n1 == 1 && pid0 > 0 && changed == 0 && n0 == 0 &&
       *(char *)&buf[0] == (char)0xab && procinfo(buf, -1) == -1;
  if (!ok)
    printf("procinfotest: procinfo(buf, 1) returned %d and changed %d "
           "bytes after buf[0]; procinfo(buf, 0) returned %d\n",
           n1, changed, n0);
  check(ok, "max=1 writes one record, max=0 none, max=-1 fails");
}

// bad buffers fail cleanly; a lazily allocated one works.
void
addrtest(void)
{
  struct pinfo *lazy;
  int n, ok = 1;

  // address 0 is our text: mapped, but not writable.
  ok = ok && procinfo((struct pinfo *)0, 1) == -1;
  // above the program's size: not ours.
  ok = ok && procinfo((struct pinfo *)(sbrk(0) + 8192), 1) == -1;
  // the trapframe page is mapped, but not for user access.
  ok = ok && procinfo((struct pinfo *)TRAPFRAME, 1) == -1;
  lazy = (struct pinfo *)sbrklazy(2 * 4096);
  // slot 0 is always init's.
  n = procinfo(lazy, NPROC);
  ok = ok && n >= 3 && lazy[0].pid == 1 && lazy[0].ppid == 0;
  check(ok, "bad buffers return -1; a lazy buffer works");
}

// check what the design guarantees about one snapshot: each
// record on its own, and the parent links (frozen by wait_lock
// during the scan). Not checked: that no two records claim the
// same hart, since records are taken at different moments.
// Return 0 if all hold, else what is wrong, with *bad set to
// the record's index. *running counts the RUNNING records.
char *
inconsistent(int n, int *running, int *bad)
{
  *running = 0;
  for (int i = 0; i < n; i++) {
    struct pinfo *p = &buf[i];
    *bad = i;
    if (p->pid <= 0 || p->state < USED || p->state > ZOMBIE)
      return "bad pid or state";
    for (int j = 0; j < i; j++)
      if (buf[j].pid == p->pid)
        return "pid twice";
    if (p->state == RUNNING) {
      if (p->cpu < 0 || p->cpu >= NCPU)
        return "running, but on no hart";
      (*running)++;
    } else if (p->cpu != -1) {
      return "on a hart, but not running";
    }
    // with wait_lock held for the whole scan, a parent
    // cannot disappear before the scan is over.
    if (p->ppid != 0 && find(p->ppid, n) == 0)
      return "parent not in snapshot";
  }
  return 0;
}

// snapshots taken for 2 seconds while one child spins and
// two others fork and reap children as fast as they can.
void
stresstest(void)
{
  int pids[3], n, running, most = 0, bad = 0, snaps = 0, maxpid = 0;
  int t0, i;
  char *why;

  for (int k = 0; k < 3; k++) {
    pids[k] = fork();
    if (pids[k] == 0) {
      for (;;) {
        if (k == 0) {
          for (volatile int j = 0; j < 100000; j++)
            ;
        } else {
          int c = fork();
          if (c == 0)
            exit(0);
          if (c > 0)
            wait(0);
        }
      }
    }
  }
  t0 = uptime();
  while (uptime() - t0 < 20) {
    n = procinfo(buf, NPROC);
    snaps++;
    if ((why = inconsistent(n, &running, &i)) != 0 && bad++ == 0)
      printf("procinfotest: first bad snapshot: %s: "
             "pid %d ppid %d state %d cpu %d\n",
             why, buf[i].pid, buf[i].ppid, buf[i].state, buf[i].cpu);
    if (running > most)
      most = running;
    for (int j = 0; j < n; j++)
      if (buf[j].pid > maxpid)
        maxpid = buf[j].pid;
  }
  for (int k = 0; k < 3; k++)
    kill(pids[k]);
  for (int k = 0; k < 3; k++)
    wait(0);
  printf("procinfotest: %d snapshots, %d bad, newest pid seen %d, "
         "most RUNNING at once %d\n",
         snaps, bad, maxpid, most);
  check(bad == 0 && maxpid > pids[2] + 100,
        "snapshots during fork/exit churn: every one consistent");
}

int
main(int argc, char *argv[])
{
  selftest();
  childtest();
  maxtest();
  addrtest();
  stresstest();
  if (fails == 0)
    printf("procinfotest: %d checks OK\n", checks);
  else
    printf("procinfotest: %d FAILED\n", fails);
  exit(fails != 0);
}
