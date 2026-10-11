// Replays an event file through each book variant and times every event with the TSC.
//
//   replay_bench EVENTS.bin [--passes N] [--cpu K] [--variants a,b,...] [--out DIR] [--dump] [--no-probe]
//
// Pass 0 of each variant is "cold" (first touch of allocator and pages); later passes are
// "warm". Passes are interleaved across variants so drift (thermals, background load) is
// spread evenly. Writes summary.csv and attribution.csv to --out; --dump also writes the raw
// per-event latencies of the last pass as <variant>.lat (uint32 ticks, uint8 tags, uint8 type).

#include <mimalloc.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#endif

#include "lob/event_io.hpp"
#include "lob/flat_book.hpp"
#include "lob/histogram.hpp"
#include "lob/map_book.hpp"
#include "lob/pool_alloc.hpp"
#include "lob/tsc.hpp"

using namespace lob;

namespace {

struct Stats {
  Histogram cold, warm;
  Histogram by_tag[kNumTags], by_type[kNumEvTypes];  // warm passes only
  uint64_t checksum = 0;                             // filled shares + live orders, must agree
};

struct Variant {
  std::string name;
  std::function<void(const EventFile&, std::vector<uint32_t>&, std::vector<uint8_t>&, uint64_t&)> run;
  std::function<double(const EventFile&)> probe;  // map variants: mean lookup chain length, untimed
};

// Does nothing: its latency distribution is the timer and OS noise floor of the replay loop.
struct NullBook {
  uint64_t seen = 0;
  uint32_t apply(const Event& e) {
    seen += e.qty;
    asm volatile("" : "+r"(seen));
    return 0;
  }
  uint64_t filled() const { return 0; }
  std::size_t live_orders() const { return 0; }
};

template <class Book, class Make>
void run_pass(const EventFile& ef, Make make, std::vector<uint32_t>& ticks, std::vector<uint8_t>& tags,
              uint64_t& checksum) {
  std::vector<Book> books;
  books.reserve(ef.syms.size());
  for (const SymInfo& s : ef.syms) books.push_back(make(s));
  const Event* ev = ef.events.data();
  const std::size_t n = ef.events.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Event& e = ev[i];
    const uint64_t t0 = tsc_begin();
    const uint32_t t = books[e.sym].apply(e);
    const uint64_t t1 = tsc_end();
    ticks[i] = uint32_t(std::min<uint64_t>(t1 - t0, UINT32_MAX));
    tags[i] = uint8_t(t);
  }
  checksum = 0;
  for (const Book& b : books) checksum += b.filled() * 1000003ull + b.live_orders();
}

// Untimed replay sampling the id index every 100k events. Returns the day-average number of
// entries a lookup of a live id shares its bucket with (weighted by live orders).
template <class Book, class Make>
double probe_chain(const EventFile& ef, Make make) {
  std::vector<Book> books;
  for (const SymInfo& s : ef.syms) books.push_back(make(s));
  double num = 0, den = 0;
  for (std::size_t i = 0; i < ef.events.size(); ++i) {
    books[ef.events[i].sym].apply(ef.events[i]);
    if (i % 100000 == 99999)
      for (const Book& b : books) {
        num += b.expected_chain() * double(b.live_orders());
        den += double(b.live_orders());
      }
  }
  return den ? num / den : 0;
}

template <class Book, class Make>
Variant map_variant(std::string name, Make make) {
  return {std::move(name), [=](auto& ef, auto& t, auto& g, auto& c) { run_pass<Book>(ef, make, t, g, c); },
          [=](const EventFile& ef) { return probe_chain<Book>(ef, make); }};
}

// Peak simultaneous live orders per symbol over the day.
std::vector<std::size_t> peak_live(const EventFile& ef) {
  std::vector<MapBook<>> books(ef.syms.size());
  std::vector<std::size_t> peak(ef.syms.size());
  for (const Event& e : ef.events) {
    books[e.sym].apply(e);
    peak[e.sym] = std::max(peak[e.sym], books[e.sym].live_orders());
  }
  return peak;
}

const std::vector<std::string> kDefaultVariants = {"null",         "map-std",   "map-mimalloc", "map-pool",
                                                   "map-pool-reserve", "flat-grow", "flat"};

std::vector<Variant> all_variants(const EventFile& ef) {
  auto map_std = [](const SymInfo&) { return MapBook<std::allocator>(); };
  auto map_mi = [](const SymInfo&) { return MapBook<mi_stl_allocator>(); };
  auto map_pool = [](const SymInfo&) { return MapBook<PoolAlloc>(); };
  auto map_pool_res = [](const SymInfo&) { return MapBook<PoolAlloc>(1u << 18); };
  // Pre-sized to exactly the day's peak: never rehashes, but runs at load factor ~1 at the peak.
  auto map_pool_peak = [peak = peak_live(ef), base = ef.syms.data()](const SymInfo& s) {
    return MapBook<PoolAlloc>(peak[&s - base]);
  };
  auto flat = [](const SymInfo& s) {
    FlatBook::Config c;
    c.center = s.ref_price;
    return FlatBook(c);
  };
  auto flat_grow = [](const SymInfo& s) {
    FlatBook::Config c;
    c.center = s.ref_price;
    c.node_capacity = 1024;
    c.index_capacity = 1024;
    return FlatBook(c);
  };
  using V = Variant;
  using Pool = MapBook<PoolAlloc>;
  std::vector<Variant> v = {
      V{"null", [](auto& ef, auto& t, auto& g, auto& c) { run_pass<NullBook>(ef, [](const SymInfo&) { return NullBook{}; }, t, g, c); }, {}},
      map_variant<MapBook<std::allocator>>("map-std", map_std),
      map_variant<MapBook<mi_stl_allocator>>("map-mimalloc", map_mi),
      map_variant<Pool>("map-pool", map_pool),
      map_variant<Pool>("map-pool-reserve", map_pool_res),
      map_variant<Pool>("map-pool-peak", map_pool_peak),
      V{"flat-grow", [=](auto& ef, auto& t, auto& g, auto& c) { run_pass<FlatBook>(ef, flat_grow, t, g, c); }, {}},
      V{"flat", [=](auto& ef, auto& t, auto& g, auto& c) { run_pass<FlatBook>(ef, flat, t, g, c); }, {}},
  };
  // Load-factor sweep: same pooled book, growing on demand, at different max load factors.
  for (const char* lf : {"0.125", "0.25", "0.5", "2", "4"})
    v.push_back(map_variant<Pool>(std::string("map-pool-lf") + lf,
                                  [f = std::stof(lf)](const SymInfo&) { return Pool(0, f); }));
  return v;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');)
    if (!t.empty()) out.push_back(t);
  return out;
}

void pin(int cpu) {
#ifdef _WIN32
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set)) std::perror("sched_setaffinity");
  // Highest normal priority. Not SCHED_FIFO: default RT throttling would inject 50 ms stalls.
  if (setpriority(PRIO_PROCESS, 0, -20)) std::perror("setpriority (needs root; continuing)");
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: replay_bench EVENTS.bin [--passes N] [--cpu K] [--variants a,b] [--out DIR] [--dump] [--no-probe]\n";
    return 2;
  }
  int passes = 4, cpu = 2;
  bool dump = false, probe = true;
  std::string out_dir = "results";
  std::vector<std::string> only;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--passes" && i + 1 < argc) passes = std::stoi(argv[++i]);
    else if (a == "--cpu" && i + 1 < argc) cpu = std::stoi(argv[++i]);
    else if (a == "--variants" && i + 1 < argc) only = split(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--dump") dump = true;
    else if (a == "--no-probe") probe = false;
  }

  pin(cpu);
  const EventFile ef = read_events(argv[1]);
  const double per_ns = tsc_per_ns();
  const uint64_t overhead = tsc_overhead();
  std::printf("%zu events, %zu symbols, TSC %.3f GHz, timer overhead %llu ticks (%.1f ns), cpu %d\n",
              ef.events.size(), ef.syms.size(), per_ns, (unsigned long long)overhead, overhead / per_ns, cpu);

  if (only.empty()) only = kDefaultVariants;
  std::vector<Variant> variants;
  for (Variant& v : all_variants(ef))
    if (std::find(only.begin(), only.end(), v.name) != only.end()) variants.push_back(std::move(v));

  std::vector<Stats> stats(variants.size());
  std::vector<uint32_t> ticks(ef.events.size());
  std::vector<uint8_t> tags(ef.events.size());
  std::filesystem::create_directories(out_dir);
  if (FILE* meta = std::fopen((out_dir + "/meta.csv").c_str(), "w")) {
    std::fprintf(meta, "events,symbols,tsc_per_ns,overhead_ticks,cpu,passes\n%zu,%zu,%.6f,%llu,%d,%d\n",
                 ef.events.size(), ef.syms.size(), per_ns, (unsigned long long)overhead, cpu, passes);
    std::fclose(meta);
  }

  for (int p = 0; p < passes; ++p) {
    for (std::size_t v = 0; v < variants.size(); ++v) {
      Stats& st = stats[v];
      uint64_t checksum = 0;
      variants[v].run(ef, ticks, tags, checksum);
      if (p == 0) st.checksum = checksum;
      else if (checksum != st.checksum) std::fprintf(stderr, "checksum drift in %s\n", variants[v].name.c_str());
      for (std::size_t i = 0; i < ticks.size(); ++i) {
        if (p == 0) {
          st.cold.add(ticks[i]);
          continue;
        }
        st.warm.add(ticks[i]);
        st.by_type[int(ef.events[i].type)].add(ticks[i]);
        for (int b = 0; b < kNumTags; ++b)
          if (tags[i] >> b & 1) st.by_tag[b].add(ticks[i]);
      }
      if (dump && p == passes - 1) {
        FILE* f = std::fopen((out_dir + "/" + variants[v].name + ".lat").c_str(), "wb");
        for (std::size_t i = 0; i < ticks.size(); ++i) {
          const uint8_t rec[6] = {uint8_t(ticks[i]), uint8_t(ticks[i] >> 8), uint8_t(ticks[i] >> 16),
                                  uint8_t(ticks[i] >> 24), tags[i], uint8_t(ef.events[i].type)};
          std::fwrite(rec, 1, 6, f);
        }
        std::fclose(f);
      }
      std::fprintf(stderr, "  pass %d %-18s done\n", p, variants[v].name.c_str());
    }
  }

  for (std::size_t v = 0; v < stats.size(); ++v)
    if (variants[v].name != "null")
      for (std::size_t w = v + 1; w < stats.size(); ++w)
        if (variants[w].name != "null" && stats[w].checksum != stats[v].checksum)
          std::fprintf(stderr, "WARNING: %s disagrees with %s on final book state\n", variants[w].name.c_str(),
                       variants[v].name.c_str());

  auto ns = [&](double t) { return t / per_ns; };
  const double qs[] = {0.5, 0.9, 0.99, 0.999, 0.9999};
  FILE* sum = std::fopen((out_dir + "/summary.csv").c_str(), "w");
  std::fprintf(sum, "variant,phase,events,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns\n");
  std::printf("\n%-18s %-5s %8s %8s %8s %8s %8s %9s %10s\n", "variant", "phase", "mean", "p50", "p90", "p99",
              "p99.9", "p99.99", "max");
  for (std::size_t v = 0; v < variants.size(); ++v)
    for (int phase = 0; phase < 2; ++phase) {
      const Histogram& h = phase ? stats[v].warm : stats[v].cold;
      if (!h.count()) continue;
      std::fprintf(sum, "%s,%s,%llu,%.1f", variants[v].name.c_str(), phase ? "warm" : "cold",
                   (unsigned long long)h.count(), ns(h.mean()));
      std::printf("%-18s %-5s %8.0f", variants[v].name.c_str(), phase ? "warm" : "cold", ns(h.mean()));
      for (double q : qs) {
        std::fprintf(sum, ",%.1f", ns(h.quantile(q)));
        std::printf(" %8.0f", ns(h.quantile(q)));
      }
      std::fprintf(sum, ",%.1f\n", ns(h.max()));
      std::printf(" %10.0f\n", ns(h.max()));
    }
  std::fclose(sum);

  // Tail attribution: among warm events at or above p99.9, how over-represented is each tag/type?
  FILE* att = std::fopen((out_dir + "/attribution.csv").c_str(), "w");
  std::fprintf(att, "variant,kind,name,share_all,share_tail,lift\n");
  std::printf("\nTail attribution (warm, events >= p99.9): share of tail / share of all\n");
  for (std::size_t v = 0; v < variants.size(); ++v) {
    const Stats& st = stats[v];
    if (!st.warm.count()) continue;
    const int tb = st.warm.quantile_bucket(0.999);
    const double all = double(st.warm.count()), tail = double(st.warm.count_from(tb));
    std::printf("%-18s", variants[v].name.c_str());
    auto row = [&](const char* kind, const char* name, const Histogram& h) {
      if (!h.count()) return;
      const double sa = h.count() / all, stl = h.count_from(tb) / tail;
      std::fprintf(att, "%s,%s,%s,%.6f,%.6f,%.3f\n", variants[v].name.c_str(), kind, name, sa, stl, stl / sa);
      if (stl >= 0.05) std::printf("  %s %.0f%%/%.1f%%", name, 100 * stl, 100 * sa);
    };
    for (int b = 0; b < kNumTags; ++b) row("tag", kTagNames[b], st.by_tag[b]);
    for (int t = 1; t < kNumEvTypes; ++t) row("type", kEvTypeNames[t], st.by_type[t]);
    std::printf("\n");
  }
  std::fclose(att);

  // Hash chain lengths (untimed, after all timed passes so it cannot disturb them).
  bool header = false;
  FILE* ch = nullptr;
  for (std::size_t v = 0; v < variants.size(); ++v) {
    if (!probe || !variants[v].probe) continue;
    if (!header) {
      ch = std::fopen((out_dir + "/chains.csv").c_str(), "w");
      std::fprintf(ch, "variant,mean_chain,rehashes_per_pass,p50_ns,p90_ns,p99_ns,p999_ns\n");
      std::printf("\n%-18s %10s %9s %8s %8s %8s %8s\n", "variant", "chain", "rehashes", "p50", "p90", "p99", "p99.9");
      header = true;
    }
    const double chain = variants[v].probe(ef);
    const Stats& st = stats[v];
    const double rehashes = double(st.by_tag[2].count()) / std::max(1, passes - 1);
    std::fprintf(ch, "%s,%.4f,%.1f", variants[v].name.c_str(), chain, rehashes);
    std::printf("%-18s %10.3f %9.1f", variants[v].name.c_str(), chain, rehashes);
    for (double q : {0.5, 0.9, 0.99, 0.999}) {
      std::fprintf(ch, ",%.1f", ns(st.warm.quantile(q)));
      std::printf(" %8.0f", ns(st.warm.quantile(q)));
    }
    std::fprintf(ch, "\n");
    std::printf("\n");
  }
  if (ch) std::fclose(ch);
  return 0;
}
