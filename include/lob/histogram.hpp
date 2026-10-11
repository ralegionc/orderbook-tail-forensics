#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace lob {

// Log-linear histogram of uint32 values: exact below 64, then 32 sub-buckets per power of
// two (about 3% relative resolution).
class Histogram {
  static constexpr int kSub = 32;
  static constexpr int kBuckets = 64 + (32 - 6) * kSub;
  std::array<uint64_t, kBuckets> c_{};
  uint64_t n_ = 0;
  double sum_ = 0;
  uint32_t max_ = 0;

 public:
  static int bucket(uint32_t v) {
    if (v < 64) return int(v);
    const int e = 31 - std::countl_zero(v);  // >= 6
    return 64 + (e - 6) * kSub + int((v >> (e - 5)) & (kSub - 1));
  }
  // Smallest value that lands in bucket b.
  static uint32_t lower(int b) {
    if (b < 64) return uint32_t(b);
    const int e = (b - 64) / kSub + 6, sub = (b - 64) % kSub;
    return (uint32_t(kSub + sub)) << (e - 5);
  }
  static uint32_t mid(int b) {
    if (b < 64) return uint32_t(b);
    const int e = (b - 64) / kSub + 6;
    return lower(b) + (1u << (e - 5)) / 2;
  }

  void add(uint32_t v) {
    ++c_[bucket(v)];
    ++n_;
    sum_ += v;
    if (v > max_) max_ = v;
  }
  void merge(const Histogram& o) {
    for (int i = 0; i < kBuckets; ++i) c_[i] += o.c_[i];
    n_ += o.n_;
    sum_ += o.sum_;
    if (o.max_ > max_) max_ = o.max_;
  }
  uint64_t count() const { return n_; }
  double mean() const { return n_ ? sum_ / double(n_) : 0; }
  uint32_t max() const { return max_; }

  // Bucket holding quantile q (0..1).
  int quantile_bucket(double q) const {
    const uint64_t rank = uint64_t(q * double(n_));
    uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
      acc += c_[i];
      if (acc > rank) return i;
    }
    return kBuckets - 1;
  }
  uint32_t quantile(double q) const { return n_ ? mid(quantile_bucket(q)) : 0; }

  uint64_t count_from(int b) const {
    uint64_t s = 0;
    for (int i = b; i < kBuckets; ++i) s += c_[i];
    return s;
  }
};

}  // namespace lob
