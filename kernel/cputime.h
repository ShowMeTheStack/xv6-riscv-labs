// What cputime() fills in. All times are in microseconds.
struct cputime {
  uint64 real;   // time since boot
  uint64 utime;  // user time of the calling process
  uint64 stime;  // kernel time of the calling process
  uint64 cutime; // user time of reaped children
  uint64 cstime; // kernel time of reaped children
};
