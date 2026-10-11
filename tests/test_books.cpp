// Directed tests plus a differential fuzz: every book variant must end in exactly the same
// state (levels, queue order, quantities, filled shares) as the textbook MapBook.

#include <cstdio>
#include <cstdlib>
#include <random>

#include "lob/flat_book.hpp"
#include "lob/map_book.hpp"
#include "lob/pool_alloc.hpp"

using namespace lob;

static int failures = 0;
static bool check(bool ok, const char* what, int line) {
  if (!ok) {
    std::fprintf(stderr, "line %d CHECK failed: %s\n", line, what);
    ++failures;
  }
  return ok;
}
#define CHECK(cond) check(bool(cond), #cond, __LINE__)

constexpr Price kMid = 290'0000;  // $290.0000 in ITCH units
constexpr Price C = 100;          // one cent

FlatBook small_flat() {
  FlatBook::Config c;
  c.center = kMid;
  c.window = 4096;  // +-$20.48, so far prices exercise the overflow maps
  c.node_capacity = 8;
  c.index_capacity = 8;  // force pool and index growth
  return FlatBook(c);
}

template <class Book>
void directed(Book b) {
  b.add(1, Side::Buy, kMid - C, 100);
  b.add(2, Side::Buy, kMid - C, 200);
  b.add(3, Side::Buy, kMid - 2 * C, 300);
  b.add(4, Side::Sell, kMid, 50);
  // FIFO within a level, then the next level, stopping at the limit.
  CHECK(b.sweep(Side::Sell, kMid - 2 * C, 350) == (kLevelGone | kMultiLevel));
  auto s = b.snapshot();
  CHECK(s.size() == 2);
  CHECK(s[0].price == kMid - 2 * C && s[0].orders.size() == 1 && s[0].orders[0].second == 250);
  CHECK(b.filled() == 350);
  // Replace loses time priority.
  b.add(5, Side::Buy, kMid - 2 * C, 10);
  b.replace(3, 6, kMid - 2 * C, 250);
  s = b.snapshot();
  CHECK(s[0].orders.size() == 2 && s[0].orders[0].first == 5 && s[0].orders[1].first == 6);
  // Unknown ids are reported, not fatal.
  CHECK(b.remove(999) == kMiss);
  CHECK(b.reduce(999, 1, true) == kMiss);
  // Partial cancel keeps the order; full execution removes it.
  CHECK(b.reduce(4, 20, false) == 0);
  CHECK(b.reduce(4, 30, true) == kLevelGone);
  CHECK(b.live_orders() == 2);
}

template <class... Books>
void fuzz(uint64_t seed, int steps, Books&... books) {
  std::mt19937_64 rng(seed);
  MapBook<> ref;
  std::vector<OrderId> ids;
  OrderId next = 1;
  auto pick = [&]() -> OrderId {
    if (ids.empty() || rng() % 50 == 0) return 1'000'000'000ull + rng() % 1000;  // unknown id
    const std::size_t i = rng() % ids.size();
    Side s;
    Price p;
    Qty q;
    if (!ref.lookup(ids[i], s, p, q)) {  // already gone: drop it, still use it once
      const OrderId dead = ids[i];
      ids[i] = ids.back();
      ids.pop_back();
      return dead;
    }
    return ids[i];
  };
  auto price_for = [&](Side s) -> Price {
    const uint32_t r = rng() % 100;
    const Price off = r < 3 ? Price(3000 + rng() % 2000) * C   // far stub, overflow map
                      : r < 6 ? Price(1 + rng() % 30) * C + 50 // sub-penny, overflow map
                              : Price(1 + rng() % 30) * C;
    return s == Side::Buy ? kMid - off : kMid + off - C;  // never crosses: bids < mid <= asks
  };

  for (int step = 0; step < steps; ++step) {
    Event e{};
    const uint32_t r = rng() % 100;
    if (r < 45 || ids.empty()) {
      e.type = EvType::Add;
      e.side = rng() & 1 ? Side::Buy : Side::Sell;
      e.id = next++;
      e.price = price_for(e.side);
      e.qty = 1 + rng() % 500;
      ids.push_back(e.id);
    } else if (r < 55) {
      e = {0, pick(), 0, Qty(1 + rng() % 300), 0, EvType::Cancel, Side::Buy, 0};
    } else if (r < 70) {
      e = {0, pick(), 0, 0, 0, EvType::Delete, Side::Buy, 0};
    } else if (r < 80) {
      e.type = EvType::Replace;
      e.id = pick();
      Side s = Side::Buy;
      Price p;
      Qty q;
      ref.lookup(e.id, s, p, q);
      e.aux = uint32_t(next - e.id);
      if (e.id >= next) e.aux = 1;  // unknown id: any delta, the replace is a miss
      else ids.push_back(next++);
      e.price = price_for(s);
      e.qty = 1 + rng() % 500;
    } else if (r < 88) {
      e = {0, pick(), 0, Qty(1 + rng() % 300), 0, EvType::Execute, Side::Buy, 0};
    } else {
      e.type = EvType::Sweep;
      e.side = rng() & 1 ? Side::Buy : Side::Sell;
      const Price depth = Price(rng() % 8) * C;
      e.price = e.side == Side::Buy ? kMid + depth : kMid - C - depth;
      e.qty = 1 + rng() % 3000;
    }
    const uint32_t want = ref.apply(e) & kMiss;
    ((CHECK((books.apply(e) & kMiss) == want)), ...);
    if (step % 997 == 0 || step == steps - 1) {
      const auto snap = ref.snapshot();
      ((CHECK(books.snapshot() == snap)), ...);
      ((CHECK(books.filled() == ref.filled())), ...);
      ((CHECK(books.live_orders() == ref.live_orders())), ...);
      if (failures) {
        std::fprintf(stderr, "diverged at seed %llu step %d\n", (unsigned long long)seed, step);
        std::exit(1);
      }
    }
  }
}

int main() {
  directed(MapBook<>());
  directed(MapBook<PoolAlloc>());
  directed(small_flat());
  for (uint64_t seed = 1; seed <= 5; ++seed) {
    MapBook<PoolAlloc> pool;
    FlatBook flat = small_flat();
    FlatBook::Config wide;
    wide.center = kMid;
    FlatBook flat_wide(wide);
    fuzz(seed, 200000, pool, flat, flat_wide);
  }
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
