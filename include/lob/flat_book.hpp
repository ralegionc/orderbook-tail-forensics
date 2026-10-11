#pragma once
#include <algorithm>
#include <bit>
#include <functional>
#include <map>
#include <vector>

#include "lob/id_index.hpp"
#include "lob/types.hpp"

namespace lob {

// Two-level bitmap over ladder slots: finds the lowest/highest occupied slot in a few word ops.
class LevelBits {
  std::vector<uint64_t> w_, s_;

 public:
  static constexpr uint32_t npos = UINT32_MAX;
  explicit LevelBits(uint32_t n) : w_((n + 63) / 64), s_((w_.size() + 63) / 64) {}
  void set(uint32_t i) {
    w_[i >> 6] |= 1ull << (i & 63);
    s_[i >> 12] |= 1ull << ((i >> 6) & 63);
  }
  void clear(uint32_t i) {
    uint64_t& w = w_[i >> 6];
    w &= ~(1ull << (i & 63));
    if (!w) s_[i >> 12] &= ~(1ull << ((i >> 6) & 63));
  }
  uint32_t first() const {
    for (std::size_t k = 0; k < s_.size(); ++k)
      if (s_[k]) {
        const std::size_t wi = k * 64 + std::countr_zero(s_[k]);
        return uint32_t(wi * 64 + std::countr_zero(w_[wi]));
      }
    return npos;
  }
  uint32_t last() const {
    for (std::size_t k = s_.size(); k-- > 0;)
      if (s_[k]) {
        const std::size_t wi = k * 64 + 63 - std::countl_zero(s_[k]);
        return uint32_t(wi * 64 + 63 - std::countl_zero(w_[wi]));
      }
    return npos;
  }
};

// Cache-friendly book: a fixed price ladder around a reference price, intrusive FIFO lists
// of pooled nodes addressed by uint32 index, and an open-addressing id index. Prices outside
// the ladder (stub quotes, sub-penny) fall back to a std::map per side.
class FlatBook {
 public:
  struct Config {
    Price center = 0;                  // reference price; the ladder is centred on it
    uint32_t window = 1u << 16;        // ladder slots per side
    Price tick = 100;                  // ITCH units per slot (one cent)
    uint32_t node_capacity = 1u << 17; // preallocated order nodes
    uint32_t index_capacity = 1u << 17;// orders the id index holds before growing
  };

  explicit FlatBook(const Config& c)
      : tick_(c.tick),
        window_(c.window),
        base_(c.center / c.tick > c.window / 2 ? c.center / c.tick - c.window / 2 : 0),
        ladder_{std::vector<Level>(c.window), std::vector<Level>(c.window)},
        bits_{LevelBits(c.window), LevelBits(c.window)},
        index_(c.index_capacity) {
    grow_nodes(std::max<uint32_t>(c.node_capacity, 1));
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
    const uint32_t idx = alloc_node(tags);
    nodes_[idx] = Node{id, p, q, kNil, kNil, s};
    Level& lv = level_for_add(s, p, tags);
    if (lv.tail == kNil) {
      lv.head = lv.tail = idx;
    } else {
      nodes_[lv.tail].next = idx;
      nodes_[idx].prev = lv.tail;
      lv.tail = idx;
    }
    if (index_.insert(id, idx)) tags |= kRehash;
    return tags;
  }

  uint32_t remove(OrderId id) {
    const uint32_t* v = index_.find(id);
    if (!v) return kMiss;
    const uint32_t idx = *v;
    index_.erase(id);
    return erase_node(idx);
  }

  uint32_t reduce(OrderId id, Qty q, bool execute) {
    const uint32_t* v = index_.find(id);
    if (!v) return kMiss;
    const uint32_t idx = *v;
    Node& n = nodes_[idx];
    const Qty done = std::min(q, n.qty);
    if (execute) filled_ += done;
    if (done < n.qty) {
      n.qty -= done;
      return 0;
    }
    index_.erase(id);
    return erase_node(idx);
  }

  uint32_t replace(OrderId id, OrderId new_id, Price p, Qty q) {
    const uint32_t* v = index_.find(id);
    if (!v) return kMiss;
    const uint32_t idx = *v;
    const Side s = nodes_[idx].side;
    index_.erase(id);
    const uint32_t tags = erase_node(idx);
    return tags | add(new_id, s, p, q);
  }

  uint32_t sweep(Side aggressor, Price limit, Qty q) {
    const Side passive = opposite(aggressor);
    uint32_t tags = 0;
    int levels = 0;
    while (q > 0) {
      const Best b = best(passive);
      if (!b.lv) break;
      if (aggressor == Side::Buy ? b.price > limit : b.price < limit) break;
      ++levels;
      Level& lv = *b.lv;
      while (q > 0 && lv.head != kNil) {
        const uint32_t idx = lv.head;
        Node& n = nodes_[idx];
        const Qty f = std::min(q, n.qty);
        n.qty -= f;
        q -= f;
        filled_ += f;
        if (n.qty == 0) {
          index_.erase(n.id);
          unlink(lv, idx);
          free_node(idx);
        }
      }
      if (lv.head == kNil) {
        drop_level(passive, b);
        tags |= kLevelGone;
      }
    }
    if (levels > 1) tags |= kMultiLevel;
    return tags;
  }

  std::size_t live_orders() const { return index_.size(); }
  uint64_t filled() const { return filled_; }

  std::vector<LevelSnap> snapshot() const {
    std::vector<LevelSnap> bids, asks;
    auto dump = [&](Side s, Price p, const Level& lv, std::vector<LevelSnap>& out) {
      LevelSnap snap{s, p, {}};
      for (uint32_t i = lv.head; i != kNil; i = nodes_[i].next) snap.orders.emplace_back(nodes_[i].id, nodes_[i].qty);
      out.push_back(std::move(snap));
    };
    for (int s = 0; s < 2; ++s) {
      auto& out = s == 0 ? bids : asks;
      for (uint32_t slot = 0; slot < window_; ++slot)
        if (ladder_[s][slot].head != kNil) dump(Side(s), slot_price(slot), ladder_[s][slot], out);
    }
    for (const auto& [p, lv] : obids_) dump(Side::Buy, p, lv, bids);
    for (const auto& [p, lv] : oasks_) dump(Side::Sell, p, lv, asks);
    std::sort(bids.begin(), bids.end(), [](auto& a, auto& b) { return a.price > b.price; });
    std::sort(asks.begin(), asks.end(), [](auto& a, auto& b) { return a.price < b.price; });
    bids.insert(bids.end(), asks.begin(), asks.end());
    return bids;
  }

 private:
  static constexpr uint32_t kNil = UINT32_MAX;
  struct Node {
    OrderId id;
    Price price;
    Qty qty;
    uint32_t prev, next;
    Side side;
  };
  struct Level {
    uint32_t head = kNil, tail = kNil;
  };
  struct Best {
    Level* lv = nullptr;
    Price price = 0;
    uint32_t slot = LevelBits::npos;  // npos when the level lives in the overflow map
  };

  Price tick_;
  uint32_t window_;
  uint64_t base_;  // price / tick of slot 0
  std::vector<Level> ladder_[2];
  LevelBits bits_[2];
  std::map<Price, Level, std::greater<Price>> obids_;  // overflow levels, best first
  std::map<Price, Level, std::less<Price>> oasks_;
  std::vector<Node> nodes_;
  uint32_t free_ = kNil;
  IdIndex index_;
  uint64_t filled_ = 0;

  Price slot_price(uint32_t slot) const { return Price((base_ + slot) * tick_); }

  bool slot_of(Price p, uint32_t& slot) const {
    if (p % tick_) return false;
    const uint64_t t = p / tick_;
    if (t < base_ || t - base_ >= window_) return false;
    slot = uint32_t(t - base_);
    return true;
  }

  void grow_nodes(uint32_t extra) {
    const uint32_t old = uint32_t(nodes_.size());
    nodes_.resize(old + extra);
    for (uint32_t i = old + extra; i-- > old;) {
      nodes_[i].next = free_;
      free_ = i;
    }
  }

  uint32_t alloc_node(uint32_t& tags) {
    if (free_ == kNil) {
      grow_nodes(uint32_t(nodes_.size()));
      tags |= kPoolGrow;
    }
    const uint32_t idx = free_;
    free_ = nodes_[idx].next;
    return idx;
  }

  void free_node(uint32_t idx) {
    nodes_[idx].next = free_;
    free_ = idx;
  }

  void unlink(Level& lv, uint32_t idx) {
    const Node& n = nodes_[idx];
    if (n.prev != kNil) nodes_[n.prev].next = n.next;
    else lv.head = n.next;
    if (n.next != kNil) nodes_[n.next].prev = n.prev;
    else lv.tail = n.prev;
  }

  Level& level_for_add(Side s, Price p, uint32_t& tags) {
    uint32_t slot;
    if (slot_of(p, slot)) {
      Level& lv = ladder_[int(s)][slot];
      if (lv.head == kNil) {
        bits_[int(s)].set(slot);
        tags |= kNewLevel;
      }
      return lv;
    }
    tags |= kOverflow;
    auto emplace = [&](auto& m) -> Level& {
      auto [it, inserted] = m.try_emplace(p);
      if (inserted) tags |= kNewLevel;
      return it->second;
    };
    return s == Side::Buy ? emplace(obids_) : emplace(oasks_);
  }

  // Unlinks and frees a node whose id has already left the index.
  uint32_t erase_node(uint32_t idx) {
    const Side s = nodes_[idx].side;
    const Price p = nodes_[idx].price;
    uint32_t tags = 0, slot;
    if (slot_of(p, slot)) {
      Level& lv = ladder_[int(s)][slot];
      unlink(lv, idx);
      if (lv.head == kNil) {
        bits_[int(s)].clear(slot);
        tags |= kLevelGone;
      }
    } else {
      tags |= kOverflow;
      auto drop = [&](auto& m) {
        auto it = m.find(p);
        unlink(it->second, idx);
        if (it->second.head == kNil) {
          m.erase(it);
          tags |= kLevelGone;
        }
      };
      if (s == Side::Buy) drop(obids_);
      else drop(oasks_);
    }
    free_node(idx);
    return tags;
  }

  Best best(Side s) {
    Best b;
    const uint32_t slot = s == Side::Buy ? bits_[0].last() : bits_[1].first();
    if (slot != LevelBits::npos) b = {&ladder_[int(s)][slot], slot_price(slot), slot};
    auto consider = [&](auto& m, auto better) {
      if (!m.empty() && (!b.lv || better(m.begin()->first, b.price)))
        b = {&m.begin()->second, m.begin()->first, LevelBits::npos};
    };
    if (s == Side::Buy) consider(obids_, std::greater<Price>{});
    else consider(oasks_, std::less<Price>{});
    return b;
  }

  void drop_level(Side s, const Best& b) {
    if (b.slot != LevelBits::npos) bits_[int(s)].clear(b.slot);
    else if (s == Side::Buy) obids_.erase(b.price);
    else oasks_.erase(b.price);
  }
};

}  // namespace lob
