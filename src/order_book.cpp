#include "order_book.hpp"

#include <cassert>
#include <numeric>

namespace lob {

// ---------------------------------------------------------------------------
// add_limit_order
// ---------------------------------------------------------------------------

OrderError OrderBook::add_limit_order(Order order) {
  if (order.quantity == 0)
    return OrderError::INVALID_QUANTITY;
  if (order.price <= 0)
    return OrderError::INVALID_PRICE;
  if (order_index_.contains(order.id))
    return OrderError::DUPLICATE_ID;

  if (order.side == Side::BID) {
    insert_bid(std::move(order));
  } else {
    insert_ask(std::move(order));
  }
  return OrderError::OK;
}

// ---------------------------------------------------------------------------
// cancel_order
// ---------------------------------------------------------------------------

OrderError OrderBook::cancel_order(OrderId id) {
  auto idx_it = order_index_.find(id);
  if (idx_it == order_index_.end())
    return OrderError::NOT_FOUND;

  const OrderLocation &loc = idx_it->second;

  if (loc.side == Side::BID) {
    auto map_it = bids_.find(loc.price);
    assert(map_it != bids_.end() &&
           "cancel index points to non-existent price level");
    PriceLevel &level = map_it->second;
    level.erase(loc.iter); // O(1) — list iterator erase
    if (level.empty())
      prune_empty_bid_level(map_it);
  } else {
    auto map_it = asks_.find(loc.price);
    assert(map_it != asks_.end() &&
           "cancel index points to non-existent price level");
    PriceLevel &level = map_it->second;
    level.erase(loc.iter);
    if (level.empty())
      prune_empty_ask_level(map_it);
  }

  order_index_.erase(idx_it);
  return OrderError::OK;
}

// ---------------------------------------------------------------------------
// reduce_order
// ---------------------------------------------------------------------------

OrderError OrderBook::reduce_order(OrderId id, Quantity reduce_by) {
  auto idx_it = order_index_.find(id);
  if (idx_it == order_index_.end())
    return OrderError::NOT_FOUND;

  Order &order = *idx_it->second.iter;
  if (reduce_by >= order.quantity) {
    // Fully cancel rather than leave a zero-qty order in the book.
    return cancel_order(id);
  }
  order.quantity -= reduce_by;
  return OrderError::OK;
}

// ---------------------------------------------------------------------------
// Top-of-book queries
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
  if (bids_.empty())
    return std::nullopt;
  return best_bid_it_->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  if (asks_.empty())
    return std::nullopt;
  return best_ask_it_->first;
}

const Order *OrderBook::top_bid_order() const noexcept {
  if (bids_.empty())
    return nullptr;
  return &best_bid_it_->second.front();
}

const Order *OrderBook::top_ask_order() const noexcept {
  if (asks_.empty())
    return nullptr;
  return &best_ask_it_->second.front();
}

Order *OrderBook::top_bid_order() noexcept {
  if (bids_.empty())
    return nullptr;
  return &best_bid_it_->second.front();
}

Order *OrderBook::top_ask_order() noexcept {
  if (asks_.empty())
    return nullptr;
  return &best_ask_it_->second.front();
}

void OrderBook::pop_top_bid() noexcept {
  if (bids_.empty())
    return;
  PriceLevel &level = best_bid_it_->second;
  order_index_.erase(level.front().id);
  level.pop_front();
  if (level.empty())
    prune_empty_bid_level(best_bid_it_);
}

void OrderBook::pop_top_ask() noexcept {
  if (asks_.empty())
    return;
  PriceLevel &level = best_ask_it_->second;
  order_index_.erase(level.front().id);
  level.pop_front();
  if (level.empty())
    prune_empty_ask_level(best_ask_it_);
}

// ---------------------------------------------------------------------------
// Depth queries
// ---------------------------------------------------------------------------

Quantity OrderBook::total_qty_at(Side side, Price price) const noexcept {
  const PriceLevel *level = find_level(side, price);
  if (!level)
    return 0;
  return static_cast<Quantity>(std::accumulate(
      level->begin(), level->end(), Quantity{0},
      [](Quantity acc, const Order &o) { return acc + o.quantity; }));
}

std::size_t OrderBook::order_count_at(Side side, Price price) const noexcept {
  const PriceLevel *level = find_level(side, price);
  return level ? level->size() : 0;
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

void OrderBook::visit_bids(LevelVisitor visitor) const {
  for (const auto &[price, level] : bids_) {
    Quantity total = std::accumulate(
        level.begin(), level.end(), Quantity{0},
        [](Quantity acc, const Order &o) { return acc + o.quantity; });
    visitor(price, total, level.size());
  }
}

void OrderBook::visit_asks(LevelVisitor visitor) const {
  for (const auto &[price, level] : asks_) {
    Quantity total = std::accumulate(
        level.begin(), level.end(), Quantity{0},
        [](Quantity acc, const Order &o) { return acc + o.quantity; });
    visitor(price, total, level.size());
  }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void OrderBook::insert_bid(Order order) {
  const Price price = order.price;
  const OrderId id = order.id;

  // operator[] default-constructs PriceLevel if not present — O(log n).
  // Acceptable: add-order is not on the critical matching path (matching
  // only touches already-resting orders).
  PriceLevel &level = bids_[price];
  level.push_back(std::move(order));

  // Record location for O(1) future cancel.
  order_index_.emplace(id,
                       OrderLocation{Side::BID, price, std::prev(level.end())});

  // Update cached best-bid iterator if this price beats the current best
  // or the book was previously empty.
  if (bids_.size() == 1 || price > best_bid_it_->first) {
    best_bid_it_ = bids_.begin(); // begin() == highest price under std::greater
  }
}

void OrderBook::insert_ask(Order order) {
  const Price price = order.price;
  const OrderId id = order.id;

  PriceLevel &level = asks_[price];
  level.push_back(std::move(order));

  order_index_.emplace(id,
                       OrderLocation{Side::ASK, price, std::prev(level.end())});

  if (asks_.size() == 1 || price < best_ask_it_->first) {
    best_ask_it_ = asks_.begin(); // begin() == lowest price under std::less
  }
}

void OrderBook::prune_empty_bid_level(BidMap::iterator it) noexcept {
  bids_.erase(it);
  // Unconditionally reset to begin(): correct whether the erased level was
  // the best or not, and handles the empty-book case (begin() == end()).
  best_bid_it_ = bids_.begin();
}

void OrderBook::prune_empty_ask_level(AskMap::iterator it) noexcept {
  asks_.erase(it);
  best_ask_it_ = asks_.begin();
}

const OrderBook::PriceLevel *OrderBook::find_level(Side side,
                                                   Price price) const noexcept {
  if (side == Side::BID) {
    auto it = bids_.find(price);
    return it != bids_.end() ? &it->second : nullptr;
  } else {
    auto it = asks_.find(price);
    return it != asks_.end() ? &it->second : nullptr;
  }
}

} // namespace lob
