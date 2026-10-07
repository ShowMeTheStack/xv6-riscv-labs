// What lockdep(0, &st) reports; shared with user programs.
struct ldstat {
  int on;        // 1 while the checker runs
  int classes;   // lock classes and subclasses seen
  int edges;     // lock orders learned
  int reports;   // reports about the kernel's own locks
  uint64 checks; // acquisitions checked
  uint64 slow;   // checks that took the checker's lock
};
