#pragma once
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace lob {

// Serialised timestamp reads (rdtscp + lfence) with compiler barriers on both sides.
inline uint64_t tsc_begin() {
  asm volatile("" ::: "memory");
  unsigned aux;
  const uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  asm volatile("" ::: "memory");
  return t;
}
inline uint64_t tsc_end() {
  asm volatile("" ::: "memory");
  unsigned aux;
  const uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  asm volatile("" ::: "memory");
  return t;
}

// TSC ticks per nanosecond, measured against steady_clock over `ms` milliseconds.
inline double tsc_per_ns(int ms = 300) {
  using clk = std::chrono::steady_clock;
  const auto c0 = clk::now();
  const uint64_t t0 = __rdtsc();
  while (clk::now() - c0 < std::chrono::milliseconds(ms)) {
  }
  const auto c1 = clk::now();
  const uint64_t t1 = __rdtsc();
  return double(t1 - t0) / double(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
}

// Median cost of an empty tsc_begin/tsc_end pair, in ticks.
inline uint64_t tsc_overhead(int reps = 200000) {
  std::vector<uint64_t> d(reps);
  for (auto& x : d) {
    const uint64_t a = tsc_begin();
    x = tsc_end() - a;
  }
  std::nth_element(d.begin(), d.begin() + reps / 2, d.end());
  return d[reps / 2];
}

}  // namespace lob
