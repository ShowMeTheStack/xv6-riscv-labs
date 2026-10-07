// Test the lock-order checker: run its self-tests, each a lock
// order that should or should not be reported, and check that the
// kernel's own locks have caused no report.

#include "kernel/types.h"
#include "kernel/lockdep.h"
#include "user/user.h"

struct test {
  char *what;
  int reports; // reports expected
} tests[] = {
  {"A then B; later B then A", 1},
  {"A then B, B then C; later C then A", 1},
  {"two locks of one class, one inside the other", 1},
  {"two sleep-locks of one class, the inner one SUB_CHILD, both ways round", 0},
  {"role 0 then SUB_CHILD; later SUB_CHILD then role 0", 1},
  {"A then B; later B then a trylock of A", 0},
};

int
main(void)
{
  struct ldstat st;
  int i, r, ok = 0, n = 0;

  if (lockdep(0, &st) < 0) {
    printf("lockdeptest: lockdep(0) failed\n");
    exit(1);
  }
  printf("lockdeptest: %d classes, %d orders, %ld acquisitions checked, "
         "%ld of them slowly\n", st.classes, st.edges, st.checks, st.slow);

  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    printf("lockdeptest: test %d: %s\n", i + 1, tests[i].what);
    r = lockdep(i + 1, 0);
    n++;
    if (r == tests[i].reports) {
      ok++;
      printf("lockdeptest: test %d: %d report(s), as expected: OK\n", i + 1, r);
    } else {
      printf("lockdeptest: test %d: %d report(s), expected %d: FAIL\n", i + 1, r,
             tests[i].reports);
    }
  }

  lockdep(0, &st);
  n++;
  if (st.on && st.reports == 0) {
    ok++;
    printf("lockdeptest: checker on, no report about the kernel's locks: OK\n");
  } else {
    printf("lockdeptest: checker on %d, reports %d: FAIL\n", st.on, st.reports);
  }
  printf("lockdeptest: %d of %d checks OK; read the reports above to see that "
         "each names both orders\n", ok, n);
  exit(ok == n ? 0 : 1);
}
