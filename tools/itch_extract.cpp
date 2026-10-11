// Streams a decompressed Nasdaq TotalView-ITCH 5.0 file and writes the visible order flow of
// selected symbols as lob::Event records.
//
//   gzip -dc 12302019.NASDAQ_ITCH50.gz | itch_extract out.bin AAPL,MSFT,...
//
// ITCH reports executions against resting orders but not the incoming order that caused them.
// Consecutive executions on one symbol, side and timestamp are grouped and replayed as a single
// Sweep. A reference book checks that a price-time-priority sweep would fill exactly the orders
// Nasdaq reported; when it would not (hidden liquidity, crosses), the group is written as
// per-order Execute events instead, so the replayed book never drifts from Nasdaq's.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "lob/event_io.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be48(const uint8_t* p) { return uint64_t(be16(p)) << 32 | be32(p + 2); }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

class Reader {
  FILE* f_;
  std::vector<uint8_t> buf_ = std::vector<uint8_t>(1 << 24);
  std::size_t pos_ = 0, end_ = 0;

 public:
  explicit Reader(FILE* f) : f_(f) {}
  // Makes n bytes available at data(); false at end of input.
  bool need(std::size_t n) {
    if (end_ - pos_ >= n) return true;
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
    end_ -= pos_;
    pos_ = 0;
    while (end_ < n) {
      const std::size_t r = std::fread(buf_.data() + end_, 1, buf_.size() - end_, f_);
      if (r == 0) return false;
      end_ += r;
    }
    return true;
  }
  const uint8_t* data() const { return buf_.data() + pos_; }
  void skip(std::size_t n) { pos_ += n; }
};

constexpr uint64_t kOpenNs = 34200ull * 1000000000ull;  // 09:30:00

struct SymState {
  uint16_t idx = 0;
  MapBook<> ref;
  // pending execution group
  uint64_t ts = 0;
  Side side = Side::Buy;  // resting side
  std::vector<Fill> fills;
  Price worst = 0;
  // stats
  Price ref_price = 0, first_exec = 0, first_add = 0;
  uint64_t adds = 0, cancels = 0, deletes = 0, replaces = 0, sweeps = 0, swept_execs = 0;
  uint64_t fallback_execs = 0, misses = 0, subpenny = 0, peak_live = 0;
};

struct Extractor {
  EventWriter& out;
  std::vector<SymState>& syms;

  void emit(SymState& st, uint64_t ts, EvType t, Side s, OrderId id, Price p, Qty q, uint32_t aux = 0) {
    const Event e{ts, id, p, q, aux, t, s, st.idx};
    out.write(e);
    st.ref.apply(e);
  }

  void flush(SymState& st) {
    if (st.fills.empty()) return;
    Qty total = 0;
    for (const Fill& f : st.fills) total += f.qty;
    const Side aggr = opposite(st.side);
    if (st.ref.peek_sweep(aggr, st.worst, total) == st.fills) {
      emit(st, st.ts, EvType::Sweep, aggr, 0, st.worst, total);
      ++st.sweeps;
      st.swept_execs += st.fills.size();
    } else {
      for (const Fill& f : st.fills) emit(st, st.ts, EvType::Execute, st.side, f.id, 0, f.qty);
      st.fallback_execs += st.fills.size();
    }
    st.fills.clear();
  }

  void execution(SymState& st, uint64_t ts, OrderId id, Qty q) {
    Side s;
    Price p;
    Qty left;
    if (!st.ref.lookup(id, s, p, left)) {
      ++st.misses;
      return;
    }
    if (!st.fills.empty() && (ts != st.ts || s != st.side)) flush(st);
    if (st.fills.empty()) {
      st.ts = ts;
      st.side = s;
      st.worst = p;
    }
    st.fills.push_back({id, q});
    st.worst = s == Side::Sell ? std::max(st.worst, p) : std::min(st.worst, p);
    if (!st.first_exec) st.first_exec = p;
    if (!st.ref_price && ts >= kOpenNs) st.ref_price = p;
  }

  void message(SymState& st, char type, uint64_t ts, const uint8_t* m) {
    switch (type) {
      case 'A':
      case 'F': {
        flush(st);
        const Side s = m[19] == 'B' ? Side::Buy : Side::Sell;
        const Price p = be32(m + 32);
        emit(st, ts, EvType::Add, s, be64(m + 11), p, be32(m + 20));
        ++st.adds;
        if (p % 100) ++st.subpenny;
        if (!st.first_add) st.first_add = p;
        st.peak_live = std::max<uint64_t>(st.peak_live, st.ref.live_orders());
        break;
      }
      case 'E':
      case 'C': execution(st, ts, be64(m + 11), be32(m + 19)); break;
      case 'X':
        flush(st);
        emit(st, ts, EvType::Cancel, Side::Buy, be64(m + 11), 0, be32(m + 19));
        ++st.cancels;
        break;
      case 'D':
        flush(st);
        emit(st, ts, EvType::Delete, Side::Buy, be64(m + 11), 0, 0);
        ++st.deletes;
        break;
      case 'U': {
        flush(st);
        const OrderId old_id = be64(m + 11), new_id = be64(m + 19);
        const Qty q = be32(m + 27);
        const Price p = be32(m + 31);
        Side s;
        Price op;
        Qty oq;
        if (!st.ref.lookup(old_id, s, op, oq)) {
          ++st.misses;
          break;
        }
        if (new_id > old_id && new_id - old_id <= UINT32_MAX) {
          emit(st, ts, EvType::Replace, s, old_id, p, q, uint32_t(new_id - old_id));
        } else {
          emit(st, ts, EvType::Delete, s, old_id, 0, 0);
          emit(st, ts, EvType::Add, s, new_id, p, q);
        }
        ++st.replaces;
        if (p % 100) ++st.subpenny;
        break;
      }
      default: break;  // trades of hidden orders, crosses, imbalances: no visible-book change
    }
  }
};

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');)
    if (!t.empty()) out.push_back(t);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: itch_extract OUT.bin SYM1,SYM2,... [ITCH_FILE|-]\n";
    return 2;
  }
  const std::string out_path = argv[1];
  const std::vector<std::string> wanted = split(argv[2]);
  FILE* in = stdin;
  if (argc > 3 && std::string(argv[3]) != "-") in = std::fopen(argv[3], "rb");
  if (!in) {
    std::cerr << "cannot open input\n";
    return 1;
  }
#ifdef _WIN32
  if (in == stdin) _setmode(_fileno(stdin), _O_BINARY);
#endif

  std::vector<SymState> syms(wanted.size());
  std::vector<SymInfo> infos(wanted.size());
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    syms[i].idx = uint16_t(i);
    std::memset(infos[i].name, ' ', 8);
    std::memcpy(infos[i].name, wanted[i].data(), std::min<std::size_t>(8, wanted[i].size()));
  }
  std::vector<int> locate_to_sym(65536, -1);

  EventWriter out(out_path, uint32_t(wanted.size()));
  Extractor ex{out, syms};
  Reader rd(in);
  uint64_t total = 0;
  while (rd.need(2)) {
    const uint16_t len = be16(rd.data());
    if (!rd.need(2 + std::size_t(len))) break;
    const uint8_t* m = rd.data() + 2;
    const char type = char(m[0]);
    const uint16_t locate = be16(m + 1);
    if (type == 'R') {
      for (std::size_t i = 0; i < wanted.size(); ++i)
        if (std::memcmp(m + 11, infos[i].name, 8) == 0) locate_to_sym[locate] = int(i);
    } else if (const int s = locate_to_sym[locate]; s >= 0) {
      ex.message(syms[s], type, be48(m + 5), m);
    }
    rd.skip(2 + std::size_t(len));
    if ((++total & ((1u << 25) - 1)) == 0) std::cerr << "  " << (total >> 20) << "M messages\r" << std::flush;
  }
  for (auto& st : syms) ex.flush(st);

  std::printf("\n%llu ITCH messages read, %llu events written\n", (unsigned long long)total,
              (unsigned long long)out.count());
  std::printf("%-6s %9s %9s %9s %9s %8s %7s %9s %7s %8s %9s\n", "sym", "adds", "cancels", "deletes", "replaces",
              "sweeps", "sweep%", "fallback", "misses", "subpenny", "peak_live");
  for (std::size_t i = 0; i < syms.size(); ++i) {
    SymState& st = syms[i];
    infos[i].ref_price = st.ref_price ? st.ref_price : st.first_exec ? st.first_exec : st.first_add;
    const uint64_t execs = st.swept_execs + st.fallback_execs;
    std::printf("%-6s %9llu %9llu %9llu %9llu %8llu %6.1f%% %9llu %7llu %8llu %9llu  ref=$%.2f\n",
                wanted[i].c_str(), (unsigned long long)st.adds, (unsigned long long)st.cancels,
                (unsigned long long)st.deletes, (unsigned long long)st.replaces, (unsigned long long)st.sweeps,
                execs ? 100.0 * double(st.swept_execs) / double(execs) : 0.0, (unsigned long long)st.fallback_execs,
                (unsigned long long)st.misses, (unsigned long long)st.subpenny, (unsigned long long)st.peak_live,
                infos[i].ref_price / 1e4);
  }
  out.finish(infos);
  return 0;
}
