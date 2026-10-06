// locktest [nproc [total]]: nproc processes share one kernel
// spinlock through lockbench(); report how evenly they got it.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/lockbench.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int nproc = 3, total = 300000;
  int fds[2], sum = 0, overlaps = 0, count = 0, maxrun = 0;
  int hart[NCPU];
  uint64 elapsed = 0;
  struct lbstat st;

  if (argc > 1)
    nproc = atoi(argv[1]);
  if (argc > 2)
    total = atoi(argv[2]);
  if (nproc < 1 || nproc > 8 || total < 1) {
    printf("usage: locktest [nproc [total]]\n");
    exit(1);
  }
  printf("locktest: %d processes, %d acquisitions of one lock\n", nproc,
         total);

  if (lockbench(0, total, 0) < 0) {
    printf("locktest: another lockbench run is in progress\n");
    exit(1);
  }
  pipe(fds);
  for (int i = 0; i < nproc; i++) {
    if (fork() == 0) {
      close(fds[0]);
      if (lockbench(nproc, total, &st) < 0)
        st.n = -1;
      write(fds[1], &st, sizeof(st));
      exit(0);
    }
  }
  close(fds[1]);

  memset(hart, 0, sizeof(hart));
  for (int i = 0; i < nproc; i++) {
    if (read(fds[0], &st, sizeof(st)) != sizeof(st) || st.n < 0) {
      printf("locktest: lockbench failed: FAIL\n");
      exit(1);
    }
    printf("locktest: process %d: %d acquisitions, wait mean %lu ns, "
           "longest %lu us\n",
           i, st.n, st.n ? st.waited * 100 / st.n : 0, st.maxwait / 10);
    sum += st.n;
    for (int h = 0; h < NCPU; h++)
      hart[h] += st.hart[h];
    if (st.elapsed > elapsed)
      elapsed = st.elapsed;
    overlaps = st.overlaps;
    count = st.count;
    maxrun = st.maxrun;
  }
  for (int i = 0; i < nproc; i++)
    wait(0);

  printf("locktest: per hart:");
  for (int h = 0; h < NCPU; h++)
    if (hart[h])
      printf(" %d", hart[h]);
  printf("\n");
  printf("locktest: longest run by one hart: %d\n", maxrun);
  printf("locktest: %lu ms, %lu acquisitions per ms\n", elapsed / 10000,
         elapsed >= 10000 ? (uint64)total / (elapsed / 10000) : 0);

  int ok = sum == total && count == total && overlaps == 0;
  printf("locktest: mutual exclusion (%d counted, %d overlaps): %s\n", sum,
         overlaps, ok ? "OK" : "FAIL");
  printf("locktest: fairness is not checked: compare the numbers above\n");
  exit(ok ? 0 : 1);
}
