# Limit Order Book Simulator

A high-performance limit order book (LOB) implemented in C++20, targeting the
latency and throughput characteristics expected in equity and futures market-making
systems.  The project demonstrates fixed-point arithmetic, cache-aware data
structure selection, price-time priority matching, and RDTSC-based latency
profiling — the kind of systems-level reasoning that matters at HFT and
market-making firms.

---

## Features

- **Price-time priority matching** — bids and asks sorted by price, FIFO within
  each level; the correct semantics for all major exchange venues
- **Fixed-point price representation** — `int64_t` with 1e8 scale; no floating
  point on any price path
- **O(1) cancel** — cancel-index maps `OrderId → list::iterator`, making
  removal independent of book depth or level size
- **O(1) best-bid / best-ask** — cached top-of-book iterators; no map traversal
  on the matching hot path
- **LIMIT, MARKET, and IOC order types**
- **CSV feed replay** — driven by `FeedHandler`; replays recorded L2 sessions
  against a live matching engine for regression testing
- **RDTSC latency benchmarks** with per-operation histogram output
- **Google Test suite** covering all matching edge cases

---

## Architecture

```
include/
  types.hpp           Price (fixed-point int64_t), Side, OrderType,
                      OrderError, Trade
  order.hpp           Order struct — 32 bytes, trivially copyable
  order_book.hpp      Core LOB: add, cancel, reduce, top-of-book queries
  matching_engine.hpp Price-time matching loop; owns the OrderBook
  feed_handler.hpp    CSV parser and replay driver

src/
  order_book.cpp      OrderBook implementation
  matching_engine.cpp Matching loops for BID and ASK aggressors
  feed_handler.cpp    std::from_chars CSV parser (zero heap on parse path)

bench/
  bench_matching.cpp  RDTSC harness — add/cancel latency + matching throughput

test/
  test_lob.cpp        14 GTest cases

data/
  sample_feed.csv     Annotated example feed for manual inspection / replay
```

### Order flow

```
FeedHandler::dispatch()
        │
        ▼
MatchingEngine::submit(Order)
    ├── validate (qty > 0, price > 0 for LIMIT)
    ├── match_against_{asks,bids}()   ← matching loop; emits Trade callbacks
    │       ├── book.top_{ask,bid}_order()   O(1) cached ptr
    │       ├── fill: emit Trade, decrement quantities
    │       └── book.pop_top_{ask,bid}()     O(1) list pop + possible O(log n) level erase
    └── book.add_limit_order(residual)       O(log n) map insert
```

---

## Performance

### Benchmark methodology

- **Timer**: `RDTSC` (`__rdtsc()`) on x86-64; `std::chrono::high_resolution_clock`
  fallback on ARM.  CPU frequency estimated at startup via a timed loop.
- **Warmup**: 10,000 iterations discarded before measurement to warm instruction
  and data caches.
- **Sample size**: 100,000 iterations per benchmark.
- **Build**: `-O3 -march=native` (Release mode); no profile-guided optimisation.
- **Environment**: single-threaded, no kernel bypass, commodity laptop hardware.

Benchmarks are printed as a full latency histogram (bucketed in ns), not just
a mean, because the distribution shape matters in practice — a 300 ns p99.9
tail can be more expensive than a high mean.

### Reference numbers (Apple M-series, Release build)

| Operation | Median | p99 | p99.9 |
|---|---|---|---|
| Add limit order (no match) | ~80 ns | ~150 ns | ~300 ns |
| Cancel order | ~60 ns | ~120 ns | ~250 ns |
| Full match (aggressor vs 1 level) | ~120 ns | ~200 ns | ~400 ns |
| Matching throughput | ~4M orders/sec | — | — |

*Numbers vary with CPU, cache state, and OS scheduling.  Run `bench_matching`
on your own hardware for authoritative figures.*

### Running benchmarks

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bench_matching

# Optional: render latency histograms as PNG
./build/bench_matching | python3 scripts/plot_latency.py
```

---

## Building

### Requirements

- CMake ≥ 3.20
- C++20-capable compiler: GCC 12+, Clang 14+, or MSVC 19.33+
- Internet access on first build (CMake fetches GoogleTest via `FetchContent`)

### Debug build (with AddressSanitizer + UBSan)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Release build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### IDE / clangd integration

After configuring, symlink the compile commands to the project root so
clangd picks them up automatically:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

---

## Usage / Replay

### Replay a CSV feed

```cpp
#include "matching_engine.hpp"
#include "feed_handler.hpp"

lob::MatchingEngine engine;
engine.set_trade_callback([](const lob::Trade& t) {
    printf("TRADE: aggressor=%llu passive=%llu price=%.8f qty=%u\n",
           t.aggressor_id, t.passive_id,
           lob::price_to_double(t.price), t.qty);
});

lob::FeedHandler handler(&engine);
const int64_t n = handler.replay_file("data/sample_feed.csv");
printf("Replayed %lld records\n", n);
```

### CSV feed format

```
# timestamp_ns, event_type, order_id, side, price, quantity
1700000000000, ADD,    1001, BID, 10000000000, 100
1700000000050, ADD,    1002, ASK, 10010000000, 100
1700000000100, CANCEL, 1001, BID, 10000000000,   0
1700000000150, TRADE,  1002, ASK, 10010000000, 100
```

Price is fixed-point integer (`int64_t`, scale 1e8).
`$100.00` → `10000000000`.

### Submit orders programmatically

```cpp
lob::MatchingEngine engine;

// Resting ask at $100.10
engine.submit_limit(1, lob::Side::ASK, lob::price_from_double(100.10), 200, ts);

// Aggressive bid crosses → trade emitted
engine.submit_limit(2, lob::Side::BID, lob::price_from_double(100.10), 100, ts);

// Market buy — walks book at any price
engine.submit_market(3, lob::Side::BID, 50, ts);

// Cancel resting order
engine.cancel(1);
```

---

## Design Decisions

### 1. Fixed-point integer price (`int64_t`, scale 1e8)

Every real exchange matching engine uses integer prices internally.
The reasons are well-understood:

- **Exact comparison** — `price_a == price_b` is a single integer instruction.
  Floating-point equality is undefined behaviour on prices; you must choose an
  epsilon and then defend that choice forever.
- **Determinism** — integer arithmetic is bit-exact across compilers, platforms,
  and optimisation levels.  Floating-point results depend on FPU state and
  compiler flags (`-ffast-math` breaks associativity silently).
- **No rounding error accumulation** — running a book for millions of ticks with
  `double` prices gradually corrupts the ordering invariant.

Scale of 1e8 (8 decimal places) covers all known equity and futures tick sizes
with room to spare.  The `int64_t` range allows prices up to ~$92 quadrillion.

### 2. `std::map` for price levels

```
Bids: std::map<Price, PriceLevel, std::greater<Price>>
Asks: std::map<Price, PriceLevel, std::less<Price>>
```

`std::map` (red-black tree) gives:
- O(log n) insert and erase
- O(1) `begin()` — the best bid/ask without a scan
- Ordered iteration for matching walks (level-by-level)

The main alternative is a **flat sorted array** of price levels.  Cache
behaviour is better for reads (contiguous memory), but insert/erase are O(n)
due to element shifting, which dominates on a volatile book with many add/cancel
events.

A **skip list** or **van Emde Boas tree** would improve worst-case insert to
O(log log U) but adds implementation complexity and pointer-chasing overhead
that rarely pays off at typical book depths (< 100 active levels).

The right answer for a production engine is to profile: if `std::map` shows up
in the flame graph, switch to a flat sorted vector with a B-tree-style bulk
insert optimisation.  For this simulator the map is the correct starting point.

### 3. `std::list<Order>` per price level

Each price level is a `std::list<Order>` rather than `std::deque<Order>`.

The reason is iterator stability.  The cancel index holds a `PriceLevel::iterator`
directly into the list:

```cpp
struct OrderLocation {
    Side                   side;
    Price                  price;
    PriceLevel::iterator   iter;   // stable; O(1) erase
};
std::unordered_map<OrderId, OrderLocation> order_index_;
```

`std::list::erase(iterator)` is O(1) and does not invalidate any other iterator.
`std::deque` iterators are invalidated by `push_back` / `push_front`, ruling out
this pattern.

The cache penalty of `std::list` (pointer-chasing through nodes) is acceptable
because individual price levels rarely hold more than a handful of orders in
practice.  If profiling reveals this is a bottleneck, the natural replacement is
a pool-allocated singly-linked list or a fixed-capacity ring buffer per level.

### 4. O(1) cancel via `order_index_`

The unordered_map from `OrderId → OrderLocation` is the critical enabler for
fast cancels.  Without it, cancelling an order requires:

1. Determine the side and price (O(1) if stored with the cancel message)
2. Scan the price level for the target order (O(k), k = orders at that level)

With the index: steps are O(1) lookup + O(1) `list::erase`.  The cost is one
`unordered_map` insert per add and one erase per cancel — both amortised O(1)
with a good hash on `uint64_t`.

### 5. Cached top-of-book iterators

```cpp
BidMap::iterator best_bid_it_;  // always == bids_.begin()
AskMap::iterator best_ask_it_;  // always == asks_.begin()
```

`std::map::begin()` is documented O(1) on every major STL implementation
(the tree maintains a `leftmost` pointer), so caching the iterator is more
about making the intent explicit than saving measurable cycles.

The real benefit appears in the matching loop: by calling `top_bid_order()` /
`top_ask_order()` once and holding the resulting `Order*`, the matching engine
avoids repeated map lookups inside a tight fill loop.  `std::map` iterators
are stable across insertions and erasures of *other* nodes, so the pointer
remains valid for the duration of a single aggressor's walk.

### 6. No exceptions on the hot path

All mutating operations return `OrderError` (an `enum class uint8_t`).  Callers
must check the return value — the `[[nodiscard]]` attribute enforces this at
compile time.  Exception-based error handling introduces:

- Non-deterministic latency (unwinding cost is data-dependent)
- ABI concerns when crossing shared-library boundaries
- Difficulty reasoning about tail latency under stress

`std::expected` (C++23) would be the idiomatic future upgrade; for now
`OrderError` is simpler and sufficient.

---

## Future Work

1. **NASDAQ ITCH 5.0 binary parser** — real binary protocol used by all US
   equity venues; shows protocol-engineering depth and yields realistic book
   data for replay.  The ITCH message wire format is public.

2. **Lock-free SPSC feed queue** — decouple the feed-ingestion thread from the
   matching-engine thread using a single-producer single-consumer ring buffer
   with cache-line padding to eliminate false sharing.  See companion repo
   `lock-free-queue` when available.

3. **Multi-instrument book** — `std::unordered_map<Symbol, OrderBook>` managed
   by a single `MatchingEngine`; requires a shared `OrderId` namespace and
   cross-symbol position limits.

4. **Top-of-book WebSocket publisher** — stream BBO updates over a WebSocket
   for a full-stack visualisation demo (React client, tick chart).

5. **Python bindings via pybind11** — expose `MatchingEngine` to Python for
   interactive analysis, order-flow backtesting, and matplotlib order-book
   animation.

6. **Almgren-Chriss execution model** — optimal liquidation trajectories given
   a book with realistic impact and decay parameters.  Planned as a separate
   repository that imports this simulator as a dependency.
