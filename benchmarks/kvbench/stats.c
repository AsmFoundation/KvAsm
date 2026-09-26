/*
 * stats.c — latency histogram, key-choice distributions and key formatting.
 */
#include <math.h>

#include "kvbench.h"

/**
 * Bucket index of a latency.
 * @param ns  latency in nanoseconds
 * @returns index in [0, HIST_BUCKETS): exact below 64 ns, then 64 linear
 *          sub-buckets per power of two
 */
size_t hist_bucket(uint64_t ns) {
  if (ns < HIST_SUB) return (size_t)ns;
  int exponent = 63 - __builtin_clzll(ns); /* >= 6 */
  int shift = exponent - 6;
  /* ns >> shift is in [64, 127]: the leading 1 plus 6 mantissa bits. */
  return (size_t)(shift + 1) * HIST_SUB + (size_t)((ns >> shift) - HIST_SUB);
}

/**
 * Representative latency of a bucket (its midpoint).
 * @param index  bucket index
 * @returns nanoseconds; relative error vs any member <= 1/128
 */
uint64_t hist_bucket_value(size_t index) {
  if (index < HIST_SUB) return index;
  size_t shift = index / HIST_SUB - 1;
  uint64_t mantissa = HIST_SUB + index % HIST_SUB;
  return (mantissa << shift) + ((1ull << shift) >> 1);
}

/**
 * Adds one sample.
 * @param h   histogram
 * @param ns  latency in nanoseconds
 */
void hist_record(Hist *h, uint64_t ns) {
  h->count[hist_bucket(ns)]++;
  h->total++;
  h->sum += (double)ns;
  if (ns > h->max) h->max = ns;
}

/**
 * Adds all samples of src to dst (histograms are mergeable by addition).
 * @param dst  accumulator
 * @param src  histogram to add
 */
void hist_merge(Hist *dst, const Hist *src) {
  for (size_t i = 0; i < HIST_BUCKETS; i++) dst->count[i] += src->count[i];
  dst->total += src->total;
  dst->sum += src->sum;
  if (src->max > dst->max) dst->max = src->max;
}

/**
 * Latency at a percentile (nearest-rank on buckets).
 * @param h    histogram
 * @param pct  percentile in (0, 100]; 100 returns the exact maximum
 * @returns nanoseconds (bucket midpoint, never above the exact maximum),
 *          0 when empty
 */
uint64_t hist_percentile(const Hist *h, double pct) {
  if (h->total == 0) return 0;
  if (pct >= 100.0) return h->max;
  uint64_t rank = (uint64_t)ceil(pct / 100.0 * (double)h->total);
  if (rank == 0) rank = 1;
  uint64_t seen = 0;
  for (size_t i = 0; i < HIST_BUCKETS; i++) {
    seen += h->count[i];
    if (seen >= rank) {
      uint64_t v = hist_bucket_value(i);
      return v > h->max ? h->max : v;
    }
  }
  return h->max;
}

/**
 * Prepares a Zipfian generator exactly as YCSB's ZipfianGenerator (Gray et
 * al., "Quickly generating billion-record synthetic databases", 1994).
 * @param z      generator
 * @param n      number of items
 * @param theta  skew in (0, 1); YCSB's default is 0.99
 * @remarks O(n) once (the zeta sum); about 30 ms for n = 10^6.
 */
void zipf_init(Zipf *z, uint64_t n, double theta) {
  double zetan = 0.0;
  for (uint64_t i = 1; i <= n; i++) zetan += 1.0 / pow((double)i, theta);
  double zeta2 = 1.0 + 1.0 / pow(2.0, theta);
  z->n = n;
  z->theta = theta;
  z->zetan = zetan;
  z->alpha = 1.0 / (1.0 - theta);
  z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - theta)) / (1.0 - zeta2 / zetan);
  z->half_pow_theta = 1.0 + pow(0.5, theta);
}

/**
 * Draws a Zipfian rank: rank 0 is the most popular item.
 * @param z    generator
 * @param rng  RNG state
 * @returns rank in [0, n)
 */
uint64_t zipf_next(const Zipf *z, uint64_t *rng) {
  double u = rng_unit(rng);
  double uz = u * z->zetan;
  if (uz < 1.0) return 0;
  if (uz < z->half_pow_theta) return 1;
  uint64_t r = (uint64_t)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
  return r >= z->n ? z->n - 1 : r;
}

/**
 * Spreads Zipfian ranks over the key space (YCSB's "scrambled" Zipfian), so
 * the hottest keys are not neighbours in id order.
 * @param rank  Zipfian rank
 * @param n     key space size
 * @returns key id in [0, n); like YCSB's FNV-based variant this is a hash,
 *          so a few ranks share an id — the popularity curve is preserved
 */
uint64_t scramble(uint64_t rank, uint64_t n) {
  uint64_t z = rank + 0x9E3779B97F4A7C15ull; /* splitmix64 finalizer */
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return z % n;
}

/**
 * Formats a key: "key:" + 12 zero-padded digits (16 bytes, no terminator).
 * @param dst  16 writable bytes
 * @param id   key id, < 10^12
 */
void key_format(char *dst, uint64_t id) {
  memcpy(dst, KEY_PREFIX, 4);
  for (int i = KEY_BYTES - 1; i >= 4; i--) {
    dst[i] = (char)('0' + id % 10);
    id /= 10;
  }
}
