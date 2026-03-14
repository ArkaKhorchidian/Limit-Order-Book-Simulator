#pragma once

#include "order.hpp"
#include "types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>

namespace lob {

// ---------------------------------------------------------------------------
// OrderBook: the core limit order book data structure.
//
// Responsibilities:
//   - Maintain bid and ask sides, each sorted by price priority.
//   - Provide O(log n) add / O(1) amortised cancel.
//   - Expose O(1) best-bid / best-ask queries via cached iterators.
//   - Emit Trade events synchronously through a registered callback.
//
// Non-responsibilities (delegated to MatchingEngine):
//   - Deciding whether an incoming order should cross the book.
//   - Order validation and sequencing.
//
// Thread-safety: NONE.  This is a single-threaded core, consistent with
// how most exchange matching engines are structured.  External locking or an
// SPSC feed queue is required for concurrent producers.
//
// ---------------------------------------------------------------------------
// Data structure choice rationale:
//
//   std::map<Price, PriceLevel, Comparator>
//     Pros : O(log n) insert/erase, O(1) begin() for best price,
//            ordered iteration for book-depth display and matching walks.
//     Cons : pointer-chasing through a red-black tree; not cache-friendly
//            for very deep books (1000s of levels).
//
//   Alternative considered: flat sorted std::vector<PriceLevel> with binary
//   search.  Better cache behaviour for reads, but O(n) insert/erase due to
//   shifting.  Worth exploring as a v2 if profiling shows map is the
//   bottleneck (it usually isn't — the matching walk is).
//
//   Per-level: std::list<Order> rather than std::deque<Order>.
//     Reason: std::list provides stable iterators, which lets the cancel
//     index hold PriceLevel::iterator directly for O(1) removal.
//     std::deque would require an O(n) scan at cancel time because its
//     iterators are invalidated by push_back/push_front.
//     Cache penalty of list is acceptable here because a single price level
//     rarely holds more than a handful of orders in practice.
// ---------------------------------------------------------------------------

class OrderBook {
public:
  // -----------------------------------------------------------------------
  // Callback types
  // -----------------------------------------------------------------------

  // Called synchronously inside add_order / cancel_order whenever a fill
  // occurs.  Keep the handler fast — it runs on the matching-engine thread.
  using TradeCallback = std::function<void(const Trade &)>;

  // -----------------------------------------------------------------------
  // Construction
  // -----------------------------------------------------------------------

  OrderBook() = default;
  ~OrderBook() = default;

  // Non-copyable: the cancel index holds iterators into the maps; copying
  // them is non-trivial and never needed in a production system.
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;

  // Move is fine — STL containers move their internal structure.
  OrderBook(OrderBook &&) noexcept = default;
  OrderBook &operator=(OrderBook &&) noexcept = default;

  // -----------------------------------------------------------------------
  // Configuration
  // -----------------------------------------------------------------------

  void set_trade_callback(TradeCallback cb) noexcept {
    trade_cb_ = std::move(cb);
  }

  // -----------------------------------------------------------------------
  // Mutating operations
  // -----------------------------------------------------------------------

  // Place a passive (non-crossing) limit order into the book.
  //
  // Callers (typically MatchingEngine) are responsible for ensuring the
  // order does not cross the opposite side before calling this.  The book
  // itself does not match — it only stores.
  //
  // Returns OK or one of: DUPLICATE_ID, INVALID_PRICE, INVALID_QUANTITY.
  [[nodiscard]] OrderError add_limit_order(Order order);

  // Remove an order by id.
  //
  // The cancel index provides O(1) lookup of the price level and the exact
  // list iterator, making removal O(1) (list::erase is O(1)).
  // If the price level becomes empty after removal it is erased from the
  // map — O(log n) — and the cached best-bid/ask iterator is updated.
  //
  // Returns OK or NOT_FOUND.
  [[nodiscard]] OrderError cancel_order(OrderId id);

  // Reduce the quantity of a resting order (partial cancel).
  // Used when the matching engine partially fills a passive order.
  // If reduce_by >= order.quantity the order is fully cancelled.
  //
  // Returns OK or NOT_FOUND.
  [[nodiscard]] OrderError reduce_order(OrderId id, Quantity reduce_by);

  // -----------------------------------------------------------------------
  // Top-of-book queries  (O(1))
  // -----------------------------------------------------------------------

  // Best bid price (highest buy), or std::nullopt if no bids.
  [[nodiscard]] std::optional<Price> best_bid() const noexcept;

  // Best ask price (lowest sell), or std::nullopt if no asks.
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;

  // Reference to the front-of-queue order at the best bid level.
  // Valid only while the book is not mutated.
  [[nodiscard]] const Order *top_bid_order() const noexcept;
  [[nodiscard]] const Order *top_ask_order() const noexcept;

  // Mutable access for the matching engine to reduce qty in-place.
  [[nodiscard]] Order *top_bid_order() noexcept;
  [[nodiscard]] Order *top_ask_order() noexcept;

  // Pop the front order from the best bid/ask level.
  // Called by the matching engine after a complete fill.
  // If the level becomes empty, it is removed from the map.
  void pop_top_bid() noexcept;
  void pop_top_ask() noexcept;

  // -----------------------------------------------------------------------
  // Depth queries
  // -----------------------------------------------------------------------

  [[nodiscard]] std::size_t bid_level_count() const noexcept {
    return bids_.size();
  }
  [[nodiscard]] std::size_t ask_level_count() const noexcept {
    return asks_.size();
  }

  [[nodiscard]] bool empty() const noexcept {
    return bids_.empty() && asks_.empty();
  }

  // Total quantity resting at a specific price level.
  [[nodiscard]] Quantity total_qty_at(Side side, Price price) const noexcept;

  // Number of orders resting at a specific price level.
  [[nodiscard]] std::size_t order_count_at(Side side,
                                           Price price) const noexcept;

  // -----------------------------------------------------------------------
  // Iteration (for book-depth display, replay checks, etc.)
  // -----------------------------------------------------------------------
  //
  // Exposes read-only spans over bid/ask price levels from best to worst.
  // The callback-based API avoids exposing internal iterator types.
  //
  // visitor(price, total_qty, order_count) called for each level.
  using LevelVisitor = std::function<void(Price, Quantity, std::size_t)>;
  void visit_bids(LevelVisitor visitor) const;
  void visit_asks(LevelVisitor visitor) const;

private:
  // -----------------------------------------------------------------------
  // Internal types
  // -----------------------------------------------------------------------

  // Each price level is a FIFO list of resting orders.
  // std::list gives O(1) erase-by-iterator, required for O(1) cancel.
  using PriceLevel = std::list<Order>;

  // Bids: highest price first (best bid == begin()).
  using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;

  // Asks: lowest price first (best ask == begin()).
  using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

  // -----------------------------------------------------------------------
  // Cancel index
  // -----------------------------------------------------------------------

  // Stores everything needed to remove an order in O(1):
  //   - which side (bids_ or asks_)
  //   - which price level (via Price → direct map lookup, O(log n))
  //   - which element in the list (PriceLevel::iterator, O(1) erase)
  //
  // Note: std::list iterators are stable — they are not invalidated by
  // insertions or erasures of other elements in the same list.
  struct OrderLocation {
    Side side;
    Price price;               // key into bids_ or asks_
    PriceLevel::iterator iter; // points directly at the Order node
  };

  std::unordered_map<OrderId, OrderLocation> order_index_;

  // -----------------------------------------------------------------------
  // Book sides
  // -----------------------------------------------------------------------

  BidMap bids_;
  AskMap asks_;

  // -----------------------------------------------------------------------
  // Cached top-of-book iterators
  //
  // std::map::begin() is documented O(1) on all major STL implementations
  // (it maintains a pointer to the leftmost node).  Caching the iterator
  // here is therefore a micro-optimisation that makes the intent explicit
  // rather than a meaningful latency saving.
  //
  // The real value is that it allows the matching engine to avoid a map
  // lookup inside the matching loop — it can hold a raw pointer to
  // top_bid_order() / top_ask_order() for the duration of a single
  // aggressor walk.
  //
  // Invariant: best_bid_it_ == bids_.end()  ⟺  bids_.empty()
  //            best_ask_it_ == asks_.end()  ⟺  asks_.empty()
  // -----------------------------------------------------------------------
  BidMap::iterator best_bid_it_{bids_.end()};
  AskMap::iterator best_ask_it_{asks_.end()};

  // -----------------------------------------------------------------------
  // Event dispatch
  // -----------------------------------------------------------------------

  TradeCallback trade_cb_; // may be null; checked before invocation

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  // Insert order into the appropriate map/level and update the index.
  void insert_bid(Order order);
  void insert_ask(Order order);

  // Remove an empty price level and repair the cached iterator.
  void prune_empty_bid_level(BidMap::iterator it) noexcept;
  void prune_empty_ask_level(AskMap::iterator it) noexcept;

  // Locate a price level on the given side, returning nullptr if absent.
  [[nodiscard]] const PriceLevel *find_level(Side side,
                                             Price price) const noexcept;
};

} // namespace lob
