#pragma once

#include "types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Order: the atomic unit of interest in the book.
//
// Kept deliberately small (32 bytes) so that a cache line (64 bytes) holds
// two orders.  The deque-of-orders layout means sequential access through a
// price level is cache-friendly when the working set fits in L2.
//
// No vtable, no heap allocation, trivially copyable — safe to memcpy for
// ring-buffer transport if we later add a lock-free feed queue.
// ---------------------------------------------------------------------------
// Fields ordered to avoid padding: 8+8+8+4+1+1 = 30 → padded to 32.
// Naïve ordering (id, side, type, price, qty, ts) would be 40 bytes due
// to the 6-byte gap the compiler inserts after two uint8_t fields to align
// Price (int64_t) on an 8-byte boundary.  Placing the 8-byte fields first
// eliminates all internal padding.
struct Order {
  OrderId id;          // globally unique, monotonically increasing
  Price price;         // fixed-point; set to ±INT64_MAX for MARKET orders
  Timestamp timestamp; // nanoseconds since epoch — determines time priority
                       // within a price level (earlier == higher priority)
  Quantity quantity;   // remaining unfilled quantity
  Side side;           // BID or ASK
  OrderType type;      // LIMIT, MARKET, IOC
                       // 2 bytes implicit trailing padding
};

static_assert(sizeof(Order) == 32, "Order must be exactly 32 bytes");
static_assert(alignof(Order) == 8, "Order must be 8-byte aligned");

} // namespace lob
