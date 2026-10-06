// Mutual exclusion lock: a ticket lock.
// A hart takes the next ticket and waits until its number is
// served, so harts get the lock in the order they asked.
struct spinlock {
  uint next;    // next ticket to hand out
  uint serving; // ticket now allowed to hold the lock

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.
};
