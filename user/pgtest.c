#include "kernel/types.h"
#include "user/user.h"

// Tests process groups and killpg, the kernel half of Ctrl-C.
// The keystroke itself (consoleintr, the shell's setfg) can only
// be typed from outside: ctrlc-test.py does that.

#define NMEMBER 4 // the leader and its three children
#define PATIENCE 30 // ticks the members get to die (3 s)

int fails;

void
check(int ok, char *what)
{
  printf("pgtest: %s: %s\n", what, ok ? "OK" : "FAIL");
  if (!ok)
    fails++;
}

// a child of the leader: report our pid, then block in the
// kernel in one of three ways until killed.
void
member(int how, int ready)
{
  int pid = getpid();
  int block[2];
  char c;

  if (how == 0) {
    // keep both ends, so that the read can never see EOF.
    pipe(block);
    write(ready, &pid, sizeof(pid));
    read(block[0], &c, 1); // piperead
  } else if (how == 1) {
    write(ready, &pid, sizeof(pid));
    pause(10000); // sys_pause
  } else {
    write(ready, &pid, sizeof(pid));
    read(0, &c, 1); // consoleread; nobody types
  }
  exit(0);
}

int
main(void)
{
  int ready[2], done[2], ping[2], pong[2], wd[2];
  int pids[NMEMBER];
  int leader, bg, watchdog, pid, i, n, st;
  char c;

  check(setpgid(1, 1) < 0, "setpgid on a process that is not our child fails");
  check(killpg(0) < 0, "killpg(0), the group of init and the shell, is refused");

  // a process in another group, which must survive.
  pipe(ping);
  pipe(pong);
  bg = fork();
  if (bg == 0) {
    setpgid(0, 0);
    read(ping[0], &c, 1);
    write(pong[1], "!", 1);
    exit(0);
  }
  setpgid(bg, bg);

  // the group: a leader that waits, and its three children.
  // every member inherits done[1]; done[0] sees EOF when all
  // of them have exited.
  pipe(ready);
  pipe(done);
  leader = fork();
  if (leader == 0) {
    setpgid(0, 0);
    close(done[0]);
    for (i = 0; i < NMEMBER - 1; i++)
      if (fork() == 0)
        member(i, ready[1]);
    pid = getpid();
    write(ready[1], &pid, sizeof(pid));
    for (;;)
      if (wait(0) < 0)
        pause(10000); // only if a child died early
  }
  setpgid(leader, leader);
  close(done[1]);
  for (i = 0; i < NMEMBER; i++)
    if (read(ready[0], &pids[i], sizeof(int)) != sizeof(int))
      pids[i] = -1;

  // a watchdog that kills the members one by one with kill()
  // if killpg has not got rid of them in time. It writes a
  // byte to wd first, so that pgtest can tell that it acted.
  pipe(wd);
  watchdog = fork();
  if (watchdog == 0) {
    close(done[0]);
    pause(PATIENCE);
    write(wd[1], "!", 1);
    for (i = 0; i < NMEMBER; i++)
      kill(pids[i]);
    exit(1);
  }
  close(wd[1]);

  pause(5); // let all four fall asleep
  check(killpg(leader) == 0, "killpg finds the group (wait, pipe read, pause, console read)");
  n = read(done[0], &c, 1); // EOF once every member has exited
  check(n == 0, "every member exited");

  kill(watchdog);
  st = 0;
  for (i = 0; i < 2; i++) {
    pid = wait(&n);
    if (pid == leader)
      st = n;
  }
  // the watchdog has exited: wd is at EOF unless it acted.
  check(read(wd[0], &c, 1) == 0, "...within 3 seconds, without the watchdog's kill()");
  check(st == -1, "the leader exited with status -1, like any killed process");

  write(ping[1], "?", 1);
  check(read(pong[0], &c, 1) == 1, "a process in another group survives");
  wait(0);

  if (fails == 0)
    printf("pgtest: ALL OK (the Ctrl-C keystroke itself is tested by ctrlc-test.py)\n");
  else
    printf("pgtest: SOME TESTS FAILED\n");
  exit(fails != 0);
}
