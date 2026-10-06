// Mutual exclusion lock.
struct spinlock {
  uint locked; // Is the lock held?
  int cls;     // lockdep class (see lockdep.c)

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.
};
