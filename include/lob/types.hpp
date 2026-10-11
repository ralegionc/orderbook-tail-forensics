#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

using Price = uint32_t;  // ITCH units: 1/10000 of a dollar
using Qty = uint32_t;
using OrderId = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };
inline constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class EvType : uint8_t {
  Add = 1,      // resting limit order (never crosses)
  Cancel = 2,   // remove qty shares from order id
  Delete = 3,   // remove order id
  Replace = 4,  // delete id, add id+aux at price/qty on the same side
  Execute = 5,  // execute qty shares of resting order id
  Sweep = 6,    // aggressor of `side` takes up to qty shares at prices no worse than `price`
};
inline constexpr int kNumEvTypes = 7;
inline constexpr const char* kEvTypeNames[kNumEvTypes] = {"?", "add", "cancel", "delete", "replace", "execute", "sweep"};

struct Event {
  uint64_t ts;    // ns since midnight
  OrderId id;     // 0 for Sweep
  Price price;
  Qty qty;
  uint32_t aux;   // Replace: new_id - id
  EvType type;
  Side side;
  uint16_t sym;
};
static_assert(sizeof(Event) == 32);

// What an event did inside a book. Used to attribute tail latency.
enum Tag : uint32_t {
  kNewLevel = 1u << 0,    // created a price level
  kLevelGone = 1u << 1,   // erased a price level
  kRehash = 1u << 2,      // order-id index grew
  kMultiLevel = 1u << 3,  // sweep crossed more than one price level
  kMiss = 1u << 4,        // referenced an unknown order id
  kOverflow = 1u << 5,    // flat book: price outside the ladder window
  kPoolGrow = 1u << 6,    // flat book: order node pool grew
};
inline constexpr int kNumTags = 7;
inline constexpr const char* kTagNames[kNumTags] = {"new_level", "level_gone", "rehash", "multi_level",
                                                    "miss",      "overflow",   "pool_grow"};

struct Fill {
  OrderId id;
  Qty qty;
  bool operator==(const Fill&) const = default;
};

// Full book state, bids best-first then asks best-first, each level in queue order.
struct LevelSnap {
  Side side;
  Price price;
  std::vector<std::pair<OrderId, Qty>> orders;
  bool operator==(const LevelSnap&) const = default;
};

}  // namespace lob
