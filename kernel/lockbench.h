// What lockbench() reports to each caller.
struct lbstat {
  int n;           // acquisitions by this caller
  int hart[NCPU];  // ... made on each hart
  uint64 maxwait;  // longest wait, in ticks of the time CSR
  uint64 waited;   // all waits added up
  uint64 elapsed;  // start line to finish, in ticks
  // the same for every caller:
  int count;       // acquisitions in all
  int overlaps;    // times two harts were inside at once
  int maxrun;      // most acquisitions in a row by one hart
};
