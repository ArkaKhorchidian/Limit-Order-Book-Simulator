#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <functional>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// MatchingEngine: price-time priority matching logic.
//
// Owns an OrderBook and is the single entry point for order submission.
// Separating the engine from the book allows the book to be tested in
// isolation (pure data-structure correctness) while the engine tests focus
// on matching semantics.
//
// Matching algorithm (price-time priority):
//   1. On LIMIT order arrival: check if it crosses the opposite side.
//      An incoming BID crosses if its price >= best ask.
//      An incoming ASK crosses if its price <= best bid.
//   2. Walk opposite side level-by-level (best price first), within each
//      level drain the FIFO queue front-to-back.
//   3. For each fill: emit a Trade, update quantities.
//   4. If aggressor has remaining qty after the walk:
//        LIMIT  → rest the residual in the book.
//        MARKET → cancel (market orders never rest).
//        IOC    → cancel (immediate-or-cancel).
//
// Complexity per submission:
//   - No match:          O(log n) — one map insert.
//   - k fills, 1 level:  O(k) fill loop + O(log n) possible level erase.
//   - k fills, m levels: O(k) fill loop + O(m log n) level erases.
//   In practice k and m are small; the hot path is the no-match case.
// ---------------------------------------------------------------------------

class MatchingEngine {
public:
  // -----------------------------------------------------------------------
  // Callbacks
  // -----------------------------------------------------------------------

  using TradeCallback = std::function<void(const Trade &)>;
  using RejectCallback = std::function<void(OrderId, OrderError)>;

  // -----------------------------------------------------------------------
  // Construction
  // -----------------------------------------------------------------------

  MatchingEngine() = default;
  ~MatchingEngine() = default;

  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine &operator=(const MatchingEngine &) = delete;
  MatchingEngine(MatchingEngine &&) noexcept = default;
  MatchingEngine &operator=(MatchingEngine &&) noexcept = default;

  // -----------------------------------------------------------------------
  // Configuration
  // -----------------------------------------------------------------------

  void set_trade_callback(TradeCallback cb) noexcept;
  void set_reject_callback(RejectCallback cb) noexcept;

  // -----------------------------------------------------------------------
  // Order submission
  // -----------------------------------------------------------------------

  // Primary entry point.  Thread-unsafe — call from a single thread or
  // protect with an external SPSC queue (see lock-free-queue companion repo).
  //
  // Returns OK on success, or an error code without side effects on failure.
  [[nodiscard]] OrderError submit(Order order);

  // Convenience wrappers for common cases.
  [[nodiscard]] OrderError submit_limit(OrderId id, Side side, Price price,
                                        Quantity qty, Timestamp ts);
  [[nodiscard]] OrderError submit_market(OrderId id, Side side, Quantity qty,
                                         Timestamp ts);

  // Cancel a resting order by id.
  [[nodiscard]] OrderError cancel(OrderId id);

  // -----------------------------------------------------------------------
  // Book access (read-only)
  // -----------------------------------------------------------------------

  [[nodiscard]] const OrderBook &book() const noexcept { return book_; }

  [[nodiscard]] std::optional<Price> best_bid() const noexcept {
    return book_.best_bid();
  }
  [[nodiscard]] std::optional<Price> best_ask() const noexcept {
    return book_.best_ask();
  }

  // -----------------------------------------------------------------------
  // Statistics
  // -----------------------------------------------------------------------

  [[nodiscard]] uint64_t total_trades() const noexcept { return total_trades_; }
  [[nodiscard]] uint64_t total_orders_submitted() const noexcept {
    return total_submitted_;
  }

private:
  // -----------------------------------------------------------------------
  // Matching helpers
  // -----------------------------------------------------------------------

  // Attempt to match `order` against the ask side (called for BID orders).
  // Fills are emitted via trade_cb_; `order.quantity` is decremented in place.
  void match_against_asks(Order &order);

  // Attempt to match `order` against the bid side (called for ASK orders).
  void match_against_bids(Order &order);

  // Emit a single fill event.
  void emit_trade(const Order &aggressor, const Order &passive,
                  Quantity fill_qty, Timestamp ts);

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------

  OrderBook book_;
  TradeCallback trade_cb_;
  RejectCallback reject_cb_;

  uint64_t total_trades_ = 0;
  uint64_t total_submitted_ = 0;
};

} // namespace lob
