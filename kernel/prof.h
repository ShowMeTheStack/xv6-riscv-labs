// A sampling profiler's histogram: one page.
// Bucket i counts samples whose pc was in
// [lo + (i << shift), lo + ((i+1) << shift)).

#define PROFBUCKETS 1016

struct prof {
  uint64 lo, hi; // pcs in [lo, hi) are counted in buckets
  int shift;     // each bucket covers 1 << shift bytes
  int nbucket;   // buckets in use
  uint nsample;  // samples taken
  uint nout;     // samples whose pc was outside [lo, hi)
  uint bucket[PROFBUCKETS];
};
