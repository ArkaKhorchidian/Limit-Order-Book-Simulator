// ---------------------------------------------------------------------------
// bench_matching.cpp — Latency and throughput benchmarks for the LOB.
//
// Methodology:
//   - RDTSC-based timing: reads the hardware timestamp counter directly,
//     giving ~1ns resolution without syscall overhead.
//   - Each benchmark warms up for WARMUP_ITERS iterations (discarded) before
//     the measurement window.  This avoids cold-cache effects in the results.
//   - Latency histogram: results bucketed in ns; printed as a table so the
//     distribution shape is visible (not just mean/p99 summary stats).
//   - Throughput: synthetic burst of N orders, wall-clock timed.
//
// On Apple Silicon (ARM): __builtin_ia32_rdtsc is not available.
// We detect the platform and fall back to std::chrono::high_resolution_clock.
// ---------------------------------------------------------------------------

#include "matching_engine.hpp"
#include "types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace lob;

// ---------------------------------------------------------------------------
// Platform-portable cycle counter
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
inline uint64_t rdtsc() noexcept { return __rdtsc(); }
static constexpr bool HAS_RDTSC = true;
#else
// ARM / other: use high_resolution_clock ticks as a proxy.
inline uint64_t rdtsc() noexcept {
  return static_cast<uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
}
static constexpr bool HAS_RDTSC = false;
#endif

// Estimate CPU frequency to convert cycles → nanoseconds.
// Simple: time a known-duration sleep and count cycles.
static double estimate_ghz() {
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t c0 = rdtsc();
  volatile uint64_t sink = 0;
  for (int i = 0; i < 1'000'000; ++i)
    sink += i;
  const uint64_t c1 = rdtsc();
  const auto t1 = std::chrono::steady_clock::now();
  const double ns = static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  return static_cast<double>(c1 - c0) / ns;
}

// ---------------------------------------------------------------------------
// Histogram utility
// ---------------------------------------------------------------------------

struct Histogram {
  std::vector<uint64_t> samples;

  void record(uint64_t v) { samples.push_back(v); }

  void print(const char *label, double ns_per_cycle) const {
    if (samples.empty())
      return;
    std::vector<uint64_t> s = samples;
    std::sort(s.begin(), s.end());

    auto cycles_to_ns = [ns_per_cycle](uint64_t c) {
      return HAS_RDTSC ? static_cast<double>(c) * ns_per_cycle
                       : static_cast<double>(c);
    };

    const double p50 = cycles_to_ns(s[s.size() * 50 / 100]);
    const double p90 = cycles_to_ns(s[s.size() * 90 / 100]);
    const double p99 = cycles_to_ns(s[s.size() * 99 / 100]);
    const double p999 = cycles_to_ns(s[s.size() * 999 / 1000]);
    const double mean = cycles_to_ns(
        std::accumulate(s.begin(), s.end(), uint64_t{0}) / s.size());

    std::printf("\n=== %s ===\n", label);
    std::printf("  n       : %zu samples\n", s.size());
    std::printf("  mean    : %.1f ns\n", mean);
    std::printf("  p50     : %.1f ns\n", p50);
    std::printf("  p90     : %.1f ns\n", p90);
    std::printf("  p99     : %.1f ns\n", p99);
    std::printf("  p99.9   : %.1f ns\n", p999);
    std::printf("  max     : %.1f ns\n", cycles_to_ns(s.back()));

    // Bucket histogram (log-scale buckets in ns).
    static constexpr double BUCKETS[] = {0,   50,   100,  150,  200,   300,
                                         500, 1000, 2000, 5000, 10000, 1e9};
    constexpr int N_BUCKETS =
        static_cast<int>(sizeof(BUCKETS) / sizeof(BUCKETS[0])) - 1;
    int counts[N_BUCKETS] = {};
    for (uint64_t c : s) {
      double ns_val = cycles_to_ns(c);
      for (int b = 0; b < N_BUCKETS; ++b) {
        if (ns_val < BUCKETS[b + 1]) {
          ++counts[b];
          break;
        }
      }
    }
    std::printf("\n  Latency distribution (ns):\n");
    for (int b = 0; b < N_BUCKETS; ++b) {
      if (counts[b] == 0)
        continue;
      std::printf("  [%6.0f, %6.0f) : %d\n", BUCKETS[b], BUCKETS[b + 1],
                  counts[b]);
    }
  }
};

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

static constexpr int WARMUP_ITERS = 10'000;
static constexpr int BENCH_ITERS = 100'000;

// Bench 1: Add a limit order (no match) and cancel it.
void bench_add_cancel(double ns_per_cycle) {
  Histogram add_hist, cancel_hist;

  // Pre-generate orders to avoid timing the random-number generation.
  std::vector<Order> orders;
  orders.reserve(WARMUP_ITERS + BENCH_ITERS);
  for (int i = 0; i < WARMUP_ITERS + BENCH_ITERS; ++i) {
    orders.push_back(Order{
        .id = static_cast<OrderId>(i + 1),
        .price =
            price_from_double(100.0 - (i % 20) * 0.01), // spread across levels
        .timestamp = static_cast<Timestamp>(i),
        .quantity = 100,
        .side = Side::BID,
        .type = OrderType::LIMIT,
    });
  }

  MatchingEngine engine;

  for (int i = 0; i < WARMUP_ITERS + BENCH_ITERS; ++i) {
    const bool measuring = (i >= WARMUP_ITERS);
    const OrderId id = orders[i].id;

    const uint64_t t0 = rdtsc();
    (void)engine.submit(orders[i]);
    const uint64_t t1 = rdtsc();

    const uint64_t t2 = rdtsc();
    (void)engine.cancel(id);
    const uint64_t t3 = rdtsc();

    if (measuring) {
      add_hist.record(t1 - t0);
      cancel_hist.record(t3 - t2);
    }
  }

  add_hist.print("Add limit order (no match)", ns_per_cycle);
  cancel_hist.print("Cancel order", ns_per_cycle);
}

// Bench 2: Matching throughput — submit a burst of alternating bids/asks
// that cross, measure total time / orders.
void bench_matching_throughput() {
  MatchingEngine engine;
  int trade_count = 0;
  engine.set_trade_callback([&](const Trade &) { ++trade_count; });

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < BENCH_ITERS; ++i) {
    const Price p = price_from_double(100.00);
    const OrderId ask_id = static_cast<OrderId>(2 * i + 1);
    const OrderId bid_id = static_cast<OrderId>(2 * i + 2);
    (void)engine.submit_limit(ask_id, Side::ASK, p, 100,
                              static_cast<Timestamp>(i));
    (void)engine.submit_limit(bid_id, Side::BID, p, 100,
                              static_cast<Timestamp>(i));
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_s = std::chrono::duration<double>(end - start).count();
  const double orders_per_sec = (2.0 * BENCH_ITERS) / elapsed_s;

  std::printf("\n=== Matching throughput ===\n");
  std::printf("  orders  : %d\n", 2 * BENCH_ITERS);
  std::printf("  trades  : %d\n", trade_count);
  std::printf("  elapsed : %.3f s\n", elapsed_s);
  std::printf("  rate    : %.0f orders/sec\n", orders_per_sec);
}

// Bench 3: Full match — single aggressive order vs 1 level.
void bench_full_match_one_level(double ns_per_cycle) {
  Histogram hist;

  MatchingEngine engine;
  OrderId id_gen = 1;
  Timestamp ts = 0;

  // Warmup.
  for (int i = 0; i < WARMUP_ITERS; ++i) {
    (void)engine.submit_limit(id_gen++, Side::ASK, price_from_double(100.0),
                              100, ts++);
    (void)engine.submit_limit(id_gen++, Side::BID, price_from_double(100.0),
                              100, ts++);
  }

  for (int i = 0; i < BENCH_ITERS; ++i) {
    (void)engine.submit_limit(id_gen++, Side::ASK, price_from_double(100.0),
                              100, ts++);
    const uint64_t t0 = rdtsc();
    (void)engine.submit_limit(id_gen++, Side::BID, price_from_double(100.0),
                              100, ts++);
    const uint64_t t1 = rdtsc();
    hist.record(t1 - t0);
  }

  hist.print("Full match — aggressor vs 1 level", ns_per_cycle);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  const double ghz = estimate_ghz();
  const double ns_per_cycle = HAS_RDTSC ? (1.0 / ghz) : 1.0;

  std::printf("LOB Benchmark\n");
  std::printf("=============\n");
  if (HAS_RDTSC) {
    std::printf("Clock    : RDTSC (~%.2f GHz)\n", ghz);
  } else {
    std::printf("Clock    : std::chrono::high_resolution_clock (no RDTSC)\n");
  }
  std::printf("Warmup   : %d iters\n", WARMUP_ITERS);
  std::printf("Measured : %d iters\n", BENCH_ITERS);

  bench_add_cancel(ns_per_cycle);
  bench_matching_throughput();
  bench_full_match_one_level(ns_per_cycle);

  std::printf("\n");
  return 0;
}
