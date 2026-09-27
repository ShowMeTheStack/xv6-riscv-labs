// One record of a process-table snapshot, as procinfo()
// copies it out to user space.
struct pinfo {
  int pid;
  int ppid;      // parent's pid, 0 if none
  int state;     // enum procstate in proc.h: 1 USED .. 5 ZOMBIE
  int cpu;       // hart running it if RUNNING, else -1
  uint64 sz;     // size of user memory (bytes)
  char name[16]; // always NUL-terminated
};
