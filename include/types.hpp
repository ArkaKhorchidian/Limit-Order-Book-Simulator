#pragma once

#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Fundamental domain types for the limit order book.
//
// Design note: all prices are represented as fixed-point int64_t scaled by
// PRICE_SCALE (1e8).  This mirrors what real exchange matching engines do —
// floating-point comparison for prices introduces rounding error and
// branch-prediction hazards that are simply unacceptable in an HFT context.
// Using integer arithmetic also gives us deterministic bit-exact results
// across compilers and platforms.
// ---------------------------------------------------------------------------

namespace lob {

// ---------------------------------------------------------------------------
// Price: fixed-point integer, 8 decimal places.
//
// e.g. $100.12345678 → Price{10012345678}
//      $0.00000001   → Price{1}          (minimum tick, 1e-8)
//
// Range: ±92.2 quadrillion (int64_t max), more than sufficient for any asset.
// ---------------------------------------------------------------------------
using Price = int64_t;
using OrderId = uint64_t;
using Quantity = uint32_t;
using Timestamp = uint64_t; // nanoseconds since epoch

inline constexpr int64_t PRICE_SCALE = 100'000'000LL; // 1e8

// Convert a human-readable decimal (e.g. 100.05) to a Price.
// Prefer this at the boundary (feed parser, tests) — never inside the hot path.
[[nodiscard]] constexpr Price price_from_double(double d) noexcept {
  return static_cast<Price>(d * PRICE_SCALE + 0.5);
}

// Convert back for display/logging only.
[[nodiscard]] constexpr double price_to_double(Price p) noexcept {
  return static_cast<double>(p) / static_cast<double>(PRICE_SCALE);
}

// Construct from integer dollars + integer sub-units (avoids floating-point
// altogether — useful in tests where the exact value must be auditable).
// e.g. price_from_units(100, 50'000'000) == $100.50
[[nodiscard]] constexpr Price price_from_units(int64_t whole,
                                               int64_t sub) noexcept {
  return whole * PRICE_SCALE + sub;
}

// ---------------------------------------------------------------------------
// Side: which side of the book the order rests on.
// ---------------------------------------------------------------------------
enum class Side : uint8_t {
  BID = 0, // buy side — descending price priority
  ASK = 1, // sell side — ascending price priority
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::BID ? Side::ASK : Side::BID;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
  return s == Side::BID ? "BID" : "ASK";
}

// ---------------------------------------------------------------------------
// OrderType: instruction semantics.
//
// LIMIT  — rest at price if no immediate match; standard book order.
// MARKET — cross at any price; rejected if book is empty on opposite side.
// IOC    — immediate-or-cancel: fill what you can, cancel the rest.
//
// CANCEL is handled as a separate operation (cancel_order) rather than an
// order type, but kept here for feed-replay purposes.
// ---------------------------------------------------------------------------
enum class OrderType : uint8_t {
  LIMIT = 0,
  MARKET = 1,
  IOC = 2,
};

// ---------------------------------------------------------------------------
// OrderError: returned by book/engine operations instead of throwing.
//
// No exceptions on the hot path — exceptions carry overhead and make latency
// distributions non-deterministic.  Error codes are cheap and explicit.
// ---------------------------------------------------------------------------
enum class OrderError : uint8_t {
  OK = 0,
  DUPLICATE_ID = 1,     // order_id already present in book
  NOT_FOUND = 2,        // cancel for unknown order_id
  EMPTY_BOOK = 3,       // market order with no liquidity on opposite side
  INVALID_PRICE = 4,    // price <= 0 for a limit order
  INVALID_QUANTITY = 5, // quantity == 0
};

[[nodiscard]] constexpr std::string_view to_string(OrderError e) noexcept {
  switch (e) {
  case OrderError::OK:
    return "OK";
  case OrderError::DUPLICATE_ID:
    return "DUPLICATE_ID";
  case OrderError::NOT_FOUND:
    return "NOT_FOUND";
  case OrderError::EMPTY_BOOK:
    return "EMPTY_BOOK";
  case OrderError::INVALID_PRICE:
    return "INVALID_PRICE";
  case OrderError::INVALID_QUANTITY:
    return "INVALID_QUANTITY";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Trade: emitted by the matching engine whenever a fill occurs.
//
// Both aggressor (incoming) and passive (resting) sides are identified so
// downstream consumers (P&L attribution, risk, audit) can route correctly.
// ---------------------------------------------------------------------------
struct Trade {
  OrderId aggressor_id;
  OrderId passive_id;
  Price price;         // fill price == passive order's limit price
  Quantity qty;        // filled quantity
  Timestamp timestamp; // nanoseconds; taken from the aggressor's clock
};

} // namespace lob
