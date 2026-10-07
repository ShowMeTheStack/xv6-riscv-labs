// Long-term locks for processes
struct sleeplock {
  uint locked;        // Is the lock held?
  int cls;            // lockdep class (see lockdep.c)
  struct spinlock lk; // spinlock protecting this sleep lock

  // For debugging:
  char *name; // Name of lock.
  int pid;    // Process holding lock
};

// Roles for acquiresleep_nested(): a sleep-lock taken while
// another lock of its class is held. Role 0 is the default.
#define SUB_CHILD 1 // an inode, while its directory is locked
