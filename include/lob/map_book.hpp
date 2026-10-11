#pragma once
#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Textbook book: std::map of price levels, std::list FIFO per level, unordered_map id index.
// Every node-based container allocates through Alloc, so the allocator can be swapped.
template <template <class> class Alloc = std::allocator>
class MapBook {
  struct Order {
    OrderId id;
    Qty qty;
  };
  using OrderList = std::list<Order, Alloc<Order>>;
  struct Level {
    OrderList orders;
  };
  template <class Cmp>
  using Levels = std::map<Price, Level, Cmp, Alloc<std::pair<const Price, Level>>>;
  struct Loc {
    Side side;
    Price price;
    typename OrderList::iterator it;
  };
  using Index = std::unordered_map<OrderId, Loc, std::hash<OrderId>, std::equal_to<OrderId>,
                                   Alloc<std::pair<const OrderId, Loc>>>;

  Levels<std::greater<Price>> bids_;
  Levels<std::less<Price>> asks_;
  Index index_;
  uint64_t filled_ = 0;

  template <class F>
  decltype(auto) with_side(Side s, F&& f) {
    if (s == Side::Buy) return f(bids_);
    return f(asks_);
  }

 public:
  explicit MapBook(std::size_t reserve_ids = 0, float max_load = 1.0f) {
    index_.max_load_factor(max_load);
    if (reserve_ids) index_.reserve(reserve_ids);
  }

  uint32_t apply(const Event& e) {
    switch (e.type) {
      case EvType::Add: return add(e.id, e.side, e.price, e.qty);
      case EvType::Cancel: return reduce(e.id, e.qty, false);
      case EvType::Delete: return remove(e.id);
      case EvType::Replace: return replace(e.id, e.id + e.aux, e.price, e.qty);
      case EvType::Execute: return reduce(e.id, e.qty, true);
      case EvType::Sweep: return sweep(e.side, e.price, e.qty);
    }
    return 0;
  }

  uint32_t add(OrderId id, Side s, Price p, Qty q) {
    uint32_t tags = 0;
    auto it = with_side(s, [&](auto& m) {
      auto [lv, inserted] = m.try_emplace(p);
      if (inserted) tags |= kNewLevel;
      lv->second.orders.push_back({id, q});
      return std::prev(lv->second.orders.end());
    });
    const std::size_t buckets = index_.bucket_count();
    index_.emplace(id, Loc{s, p, it});
    if (index_.bucket_count() != buckets) tags |= kRehash;
    return tags;
  }

  uint32_t remove(OrderId id) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return kMiss;
    return erase_at(ix);
  }

  // Cancel or execute q shares; removes the order when nothing is left.
  uint32_t reduce(OrderId id, Qty q, bool execute) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return kMiss;
    Order& o = *ix->second.it;
    const Qty done = std::min(q, o.qty);
    if (execute) filled_ += done;
    if (done < o.qty) {
      o.qty -= done;
      return 0;
    }
    return erase_at(ix);
  }

  uint32_t replace(OrderId id, OrderId new_id, Price p, Qty q) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return kMiss;
    const Side s = ix->second.side;
    return erase_at(ix) | add(new_id, s, p, q);
  }

  uint32_t sweep(Side aggressor, Price limit, Qty q) {
    uint32_t tags = 0;
    int levels = 0;
    auto run = [&](auto& m, auto crosses) {
      while (q > 0 && !m.empty()) {
        auto lv = m.begin();
        if (!crosses(lv->first)) break;
        ++levels;
        auto& orders = lv->second.orders;
        while (q > 0 && !orders.empty()) {
          Order& o = orders.front();
          const Qty f = std::min(q, o.qty);
          o.qty -= f;
          q -= f;
          filled_ += f;
          if (o.qty == 0) {
            index_.erase(o.id);
            orders.pop_front();
          }
        }
        if (orders.empty()) {
          m.erase(lv);
          tags |= kLevelGone;
        }
      }
    };
    if (aggressor == Side::Buy) run(asks_, [&](Price p) { return p <= limit; });
    else run(bids_, [&](Price p) { return p >= limit; });
    if (levels > 1) tags |= kMultiLevel;
    return tags;
  }

  // The fills sweep() would produce, without changing the book.
  std::vector<Fill> peek_sweep(Side aggressor, Price limit, Qty q) const {
    std::vector<Fill> out;
    auto run = [&](const auto& m, auto crosses) {
      for (auto lv = m.begin(); q > 0 && lv != m.end() && crosses(lv->first); ++lv)
        for (auto o = lv->second.orders.begin(); q > 0 && o != lv->second.orders.end(); ++o) {
          const Qty f = std::min(q, o->qty);
          out.push_back({o->id, f});
          q -= f;
        }
    };
    if (aggressor == Side::Buy) run(asks_, [&](Price p) { return p <= limit; });
    else run(bids_, [&](Price p) { return p >= limit; });
    return out;
  }

  bool lookup(OrderId id, Side& s, Price& p, Qty& q) const {
    auto ix = index_.find(id);
    if (ix == index_.end()) return false;
    s = ix->second.side;
    p = ix->second.price;
    q = ix->second.it->qty;
    return true;
  }

  std::size_t live_orders() const { return index_.size(); }
  uint64_t filled() const { return filled_; }
  std::size_t buckets() const { return index_.bucket_count(); }

  // Mean bucket occupancy seen by a lookup of a random live id: sum(size^2) / n. Slow; untimed use only.
  double expected_chain() const {
    if (index_.empty()) return 0;
    double s = 0;
    for (std::size_t b = 0; b < index_.bucket_count(); ++b) {
      const double n = double(index_.bucket_size(b));
      s += n * n;
    }
    return s / double(index_.size());
  }

  std::vector<LevelSnap> snapshot() const {
    std::vector<LevelSnap> out;
    auto dump = [&](Side s, const auto& m) {
      for (const auto& [p, lv] : m) {
        LevelSnap snap{s, p, {}};
        for (const Order& o : lv.orders) snap.orders.emplace_back(o.id, o.qty);
        out.push_back(std::move(snap));
      }
    };
    dump(Side::Buy, bids_);
    dump(Side::Sell, asks_);
    return out;
  }

 private:
  uint32_t erase_at(typename Index::iterator ix) {
    uint32_t tags = 0;
    const Loc& loc = ix->second;
    with_side(loc.side, [&](auto& m) {
      auto lv = m.find(loc.price);
      lv->second.orders.erase(loc.it);
      if (lv->second.orders.empty()) {
        m.erase(lv);
        tags |= kLevelGone;
      }
    });
    index_.erase(ix);
    return tags;
  }
};

}  // namespace lob
