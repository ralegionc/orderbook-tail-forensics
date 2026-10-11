#pragma once
#include <cstddef>
#include <new>
#include <vector>

namespace lob {

// Free-list pool for one (size, alignment). Single-threaded; chunks are never returned.
template <std::size_t Size, std::size_t Align>
class FixedPool {
  union Slot {
    Slot* next;
    alignas(Align) unsigned char bytes[Size];
  };
  static constexpr std::size_t kChunk = 4096;  // slots per chunk

  Slot* free_ = nullptr;
  std::vector<Slot*> chunks_;

  void refill() {
    auto* c = static_cast<Slot*>(::operator new(sizeof(Slot) * kChunk, std::align_val_t{alignof(Slot)}));
    chunks_.push_back(c);
    for (std::size_t i = kChunk; i-- > 0;) {
      c[i].next = free_;
      free_ = &c[i];
    }
  }

 public:
  static FixedPool& get() {
    static FixedPool* p = new FixedPool;  // leaked on purpose: outlives static containers
    return *p;
  }
  void* alloc() {
    if (!free_) refill();
    Slot* s = free_;
    free_ = s->next;
    return s;
  }
  void release(void* p) {
    auto* s = static_cast<Slot*>(p);
    s->next = free_;
    free_ = s;
  }
};

// Node allocations (n == 1) come from FixedPool; arrays (hash buckets) go to operator new.
template <class T>
struct PoolAlloc {
  using value_type = T;
  PoolAlloc() = default;
  template <class U>
  PoolAlloc(const PoolAlloc<U>&) {}

  T* allocate(std::size_t n) {
    if (n == 1) return static_cast<T*>(FixedPool<sizeof(T), alignof(T)>::get().alloc());
    return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
  }
  void deallocate(T* p, std::size_t n) {
    if (n == 1) return FixedPool<sizeof(T), alignof(T)>::get().release(p);
    ::operator delete(p, std::align_val_t{alignof(T)});
  }
  template <class U>
  bool operator==(const PoolAlloc<U>&) const { return true; }
};

}  // namespace lob
