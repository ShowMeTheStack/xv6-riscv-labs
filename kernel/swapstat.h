// filled in by the swapstat system call.
struct swapstat {
  int freepages; // pages on the free list
  int slotsused; // swap slots in use
  int nslots;    // swap slots in all (NSWAP)
  int pageouts;  // pages written to swap since boot
  int pageins;   // pages read from swap since boot
};
