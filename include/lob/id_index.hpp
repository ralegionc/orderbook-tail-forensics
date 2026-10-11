#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing OrderId -> uint32 map. Linear probing, Fibonacci hashing,
// backward-shift deletion (no tombstones). Key 0 marks an empty slot.
class IdIndex {
  struct Slot {
    OrderId key;
    uint32_t val;
  };
  std::vector<Slot> tab_;
  std::size_t mask_ = 0;
  int shift_ = 64;
  std::size_t used_ = 0;

  std::size_t home(OrderId k) const { return (k * 0x9E3779B97F4A7C15ull) >> shift_; }

  void resize(std::size_t cap) {
    std::vector<Slot> old;
    old.swap(tab_);
    tab_.assign(cap, Slot{0, 0});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
    for (const Slot& s : old)
      if (s.key) place(s.key, s.val);
  }

  void place(OrderId k, uint32_t v) {
    std::size_t i = home(k);
    while (tab_[i].key) i = (i + 1) & mask_;
    tab_[i] = {k, v};
  }

 public:
  // Holds at least `capacity` keys before the first resize (max load 1/2).
  explicit IdIndex(std::size_t capacity) { resize(std::bit_ceil(std::max<std::size_t>(capacity * 2, 16))); }

  std::size_t size() const { return used_; }

  uint32_t* find(OrderId k) {
    for (std::size_t i = home(k);; i = (i + 1) & mask_) {
      if (tab_[i].key == k) return &tab_[i].val;
      if (!tab_[i].key) return nullptr;
    }
  }

  // Returns true when the table had to grow.
  bool insert(OrderId k, uint32_t v) {
    bool grew = false;
    if ((used_ + 1) * 2 > tab_.size()) {
      resize(tab_.size() * 2);
      grew = true;
    }
    place(k, v);
    ++used_;
    return grew;
  }

  void erase(OrderId k) {
    std::size_t i = home(k);
    while (tab_[i].key != k) {
      if (!tab_[i].key) return;
      i = (i + 1) & mask_;
    }
    for (std::size_t j = i;;) {
      j = (j + 1) & mask_;
      if (!tab_[j].key) break;
      const std::size_t h = home(tab_[j].key);
      const bool stays = i < j ? (i < h && h <= j) : (i < h || h <= j);
      if (!stays) {
        tab_[i] = tab_[j];
        i = j;
      }
    }
    tab_[i].key = 0;
    --used_;
  }
};

}  // namespace lob
