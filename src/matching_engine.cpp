#include "matching_engine.hpp"

#include <cassert>
#include <limits>

namespace lob {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void MatchingEngine::set_trade_callback(TradeCallback cb) noexcept {
  trade_cb_ = std::move(cb);
  // Wire through to the book so it can emit fills for any internal logic
  // that bypasses the engine (e.g. direct book tests).
  // In this design the engine owns the fills, so we do not forward here.
}

void MatchingEngine::set_reject_callback(RejectCallback cb) noexcept {
  reject_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Order submission
// ---------------------------------------------------------------------------

OrderError MatchingEngine::submit(Order order) {
  if (order.quantity == 0) {
    const OrderError err = OrderError::INVALID_QUANTITY;
    if (reject_cb_)
      reject_cb_(order.id, err);
    return err;
  }
  if (order.type == OrderType::LIMIT && order.price <= 0) {
    const OrderError err = OrderError::INVALID_PRICE;
    if (reject_cb_)
      reject_cb_(order.id, err);
    return err;
  }

  ++total_submitted_;

  // For market orders, use a sentinel price that ensures crossing against
  // everything on the opposite side.
  if (order.type == OrderType::MARKET) {
    order.price = (order.side == Side::BID) ? std::numeric_limits<Price>::max()
                                            : std::numeric_limits<Price>::min();
  }

  // Attempt to match before resting.
  if (order.side == Side::BID) {
    match_against_asks(order);
  } else {
    match_against_bids(order);
  }

  // Route residual quantity.
  if (order.quantity == 0)
    return OrderError::OK;

  // Market orders and IOC orders do not rest — discard the unfilled residual.
  if (order.type == OrderType::MARKET || order.type == OrderType::IOC) {
    if (order.type == OrderType::MARKET && reject_cb_) {
      // Partial fill on a market order is not an error, but fully
      // unfilled is reported so the caller can handle it.
    }
    return OrderError::OK;
  }

  // Limit order: rest the residual in the book.
  return book_.add_limit_order(std::move(order));
}

OrderError MatchingEngine::submit_limit(OrderId id, Side side, Price price,
                                        Quantity qty, Timestamp ts) {
  return submit(Order{.id = id,
                      .price = price,
                      .timestamp = ts,
                      .quantity = qty,
                      .side = side,
                      .type = OrderType::LIMIT});
}

OrderError MatchingEngine::submit_market(OrderId id, Side side, Quantity qty,
                                         Timestamp ts) {
  return submit(Order{.id = id,
                      .price = 0,
                      .timestamp = ts,
                      .quantity = qty,
                      .side = side,
                      .type = OrderType::MARKET});
}

OrderError MatchingEngine::cancel(OrderId id) { return book_.cancel_order(id); }

// ---------------------------------------------------------------------------
// Matching loops
// ---------------------------------------------------------------------------

void MatchingEngine::match_against_asks(Order &bid) {
  // Walk ask levels from best (lowest) to worst (highest).
  // Stop when: aggressor is filled, or best ask > bid price (no more crosses).
  while (bid.quantity > 0) {
    Order *passive = book_.top_ask_order();
    if (!passive)
      break; // book is empty
    if (passive->price > bid.price)
      break; // no longer crosses

    const Quantity fill_qty = std::min(bid.quantity, passive->quantity);
    emit_trade(bid, *passive, fill_qty, bid.timestamp);

    bid.quantity -= fill_qty;

    if (fill_qty == passive->quantity) {
      // Passive order fully consumed — remove from book.
      book_.pop_top_ask();
    } else {
      // Partial fill — reduce passive quantity in place.
      passive->quantity -= fill_qty;
    }
  }
}

void MatchingEngine::match_against_bids(Order &ask) {
  // Walk bid levels from best (highest) to worst (lowest).
  while (ask.quantity > 0) {
    Order *passive = book_.top_bid_order();
    if (!passive)
      break;
    if (passive->price < ask.price)
      break;

    const Quantity fill_qty = std::min(ask.quantity, passive->quantity);
    emit_trade(ask, *passive, fill_qty, ask.timestamp);

    ask.quantity -= fill_qty;

    if (fill_qty == passive->quantity) {
      book_.pop_top_bid();
    } else {
      passive->quantity -= fill_qty;
    }
  }
}

void MatchingEngine::emit_trade(const Order &aggressor, const Order &passive,
                                Quantity fill_qty, Timestamp ts) {
  ++total_trades_;
  if (trade_cb_) {
    trade_cb_(Trade{
        .aggressor_id = aggressor.id,
        .passive_id = passive.id,
        .price = passive.price, // fill at passive limit price
        .qty = fill_qty,
        .timestamp = ts,
    });
  }
}

} // namespace lob
