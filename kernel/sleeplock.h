// Long-term locks for processes
struct sleeplock {
  uint locked;        // Is the lock held?
  int cls;            // lockdep class (see lockdep.c)
  struct spinlock lk; // spinlock protecting this sleep lock

  // For debugging:
  char *name; // Name of lock.
  int pid;    // Process holding lock
};
