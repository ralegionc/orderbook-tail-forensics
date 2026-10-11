// Spins on one core reading the TSC and records every gap longer than a threshold: time the
// thread was not executing (interrupts, preemption, a hypervisor descheduling the vCPU).
//
//   hiccup [seconds=10] [cpu=6] [threshold_ns=2000]

#include <x86intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

#include "lob/tsc.hpp"

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 10;
  const int cpu = argc > 2 ? std::atoi(argv[2]) : 6;
  const double threshold_ns = argc > 3 ? std::atof(argv[3]) : 2000;
#ifdef _WIN32
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
  setpriority(PRIO_PROCESS, 0, -20);
#endif
  const double per_ns = lob::tsc_per_ns();
  const uint64_t thresh = uint64_t(threshold_ns * per_ns);
  const uint64_t end = __rdtsc() + uint64_t(seconds * 1e9 * per_ns);
  std::vector<uint64_t> gaps;
  gaps.reserve(1 << 20);
  uint64_t prev = __rdtsc(), now = prev;
  while (now < end) {
    now = __rdtsc();
    if (now - prev > thresh) gaps.push_back(now - prev);
    prev = now;
  }
  uint64_t lost = 0;
  for (uint64_t g : gaps) lost += g;
  std::sort(gaps.begin(), gaps.end());
  auto us = [&](uint64_t t) { return double(t) / per_ns / 1000.0; };
  auto pct = [&](double q) { return gaps.empty() ? 0.0 : us(gaps[size_t(q * double(gaps.size() - 1))]); };
  std::printf("cpu %d, %.0f s, TSC %.3f GHz: %zu gaps > %.0f us (%.1f per s), time lost %.2f%%\n", cpu, seconds,
              per_ns, gaps.size(), threshold_ns / 1000, double(gaps.size()) / seconds,
              100.0 * double(lost) / (seconds * 1e9 * per_ns));
  std::printf("gap size us: p50 %.1f  p90 %.1f  p99 %.1f  max %.1f\n", pct(0.5), pct(0.9), pct(0.99),
              gaps.empty() ? 0.0 : us(gaps.back()));
  return 0;
}
