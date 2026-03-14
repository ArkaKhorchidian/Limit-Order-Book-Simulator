#include "matching_engine.hpp"
#include "types.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace lob;

// ---------------------------------------------------------------------------
// Test fixture: fresh engine for each test case.
// ---------------------------------------------------------------------------

class LOBTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.set_trade_callback(
        [this](const Trade &t) { trades_.push_back(t); });
  }

  MatchingEngine engine_;
  std::vector<Trade> trades_;

  // Helpers for cleaner test code.
  Timestamp ts_ = 1'700'000'000'000ULL; // arbitrary base timestamp (ns)
  OrderId next_id_ = 1;

  OrderError add_bid(Price price, Quantity qty) {
    return engine_.submit_limit(next_id_++, Side::BID, price, qty, ts_++);
  }
  OrderError add_ask(Price price, Quantity qty) {
    return engine_.submit_limit(next_id_++, Side::ASK, price, qty, ts_++);
  }
  OrderError market_buy(Quantity qty) {
    return engine_.submit_market(next_id_++, Side::BID, qty, ts_++);
  }
  OrderError market_sell(Quantity qty) {
    return engine_.submit_market(next_id_++, Side::ASK, qty, ts_++);
  }
};

// ---------------------------------------------------------------------------
// Basic book construction
// ---------------------------------------------------------------------------

TEST_F(LOBTest, AddBidUpdatesBestBid) {
  const Price p = price_from_double(100.05);
  add_bid(p, 100);
  ASSERT_TRUE(engine_.best_bid().has_value());
  EXPECT_EQ(engine_.best_bid().value(), p);
  EXPECT_FALSE(engine_.best_ask().has_value());
}

TEST_F(LOBTest, AddAskUpdatesBestAsk) {
  const Price p = price_from_double(100.10);
  add_ask(p, 200);
  ASSERT_TRUE(engine_.best_ask().has_value());
  EXPECT_EQ(engine_.best_ask().value(), p);
  EXPECT_FALSE(engine_.best_bid().has_value());
}

TEST_F(LOBTest, MultipleBidLevelsBestIsHighest) {
  add_bid(price_from_double(99.00), 100);
  add_bid(price_from_double(100.00), 50);
  add_bid(price_from_double(98.50), 200);

  EXPECT_EQ(engine_.best_bid().value(), price_from_double(100.00));
}

TEST_F(LOBTest, MultipleAskLevelsBestIsLowest) {
  add_ask(price_from_double(101.00), 100);
  add_ask(price_from_double(100.50), 50);
  add_ask(price_from_double(102.00), 200);

  EXPECT_EQ(engine_.best_ask().value(), price_from_double(100.50));
}

// ---------------------------------------------------------------------------
// Exact match → both orders consumed, one trade emitted
// ---------------------------------------------------------------------------

TEST_F(LOBTest, ExactMatchConsumesOrders) {
  const Price p = price_from_double(100.00);
  add_ask(p, 100); // resting ask
  add_bid(p, 100); // aggressive bid → crosses

  ASSERT_EQ(trades_.size(), 1u);
  EXPECT_EQ(trades_[0].price, p);
  EXPECT_EQ(trades_[0].qty, 100u);

  // Both sides should be empty.
  EXPECT_FALSE(engine_.best_bid().has_value());
  EXPECT_FALSE(engine_.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Partial fill → residual resting order remains
// ---------------------------------------------------------------------------

TEST_F(LOBTest, PartialFillLeavesResidual) {
  const Price ask_p = price_from_double(100.00);
  add_ask(ask_p, 200); // resting: 200
  add_bid(ask_p, 75);  // aggressive bid for 75

  ASSERT_EQ(trades_.size(), 1u);
  EXPECT_EQ(trades_[0].qty, 75u);

  // 125 should still rest on the ask side.
  EXPECT_FALSE(engine_.best_bid().has_value());
  ASSERT_TRUE(engine_.best_ask().has_value());
  EXPECT_EQ(engine_.book().total_qty_at(Side::ASK, ask_p), 125u);
}

// ---------------------------------------------------------------------------
// Cancel order
// ---------------------------------------------------------------------------

TEST_F(LOBTest, CancelRemovesOrderFromBook) {
  const Price p = price_from_double(99.50);
  const OrderId id = next_id_;
  add_bid(p, 100);

  EXPECT_EQ(engine_.cancel(id), OrderError::OK);
  EXPECT_FALSE(engine_.best_bid().has_value());
}

TEST_F(LOBTest, CancelUnknownIdReturnsError) {
  EXPECT_EQ(engine_.cancel(99999u), OrderError::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Market order walks multiple levels
// ---------------------------------------------------------------------------

TEST_F(LOBTest, MarketOrderWalksMultipleLevels) {
  add_ask(price_from_double(100.00), 50);
  add_ask(price_from_double(100.50), 50);
  add_ask(price_from_double(101.00), 50);

  market_buy(120); // should consume 50 @ 100.00, 50 @ 100.50, 20 @ 101.00

  ASSERT_EQ(trades_.size(), 3u);
  EXPECT_EQ(trades_[0].qty, 50u);
  EXPECT_EQ(trades_[1].qty, 50u);
  EXPECT_EQ(trades_[2].qty, 20u);

  // 30 should remain at 101.00.
  EXPECT_EQ(engine_.book().total_qty_at(Side::ASK, price_from_double(101.00)),
            30u);
}

// ---------------------------------------------------------------------------
// Price-time priority: earlier order at same price fills first
// ---------------------------------------------------------------------------

TEST_F(LOBTest, PriceTimePriorityEarlierFillsFirst) {
  const Price p = price_from_double(100.00);
  const OrderId first_id = next_id_;
  const OrderId second_id = next_id_ + 1;

  add_ask(p, 50); // first_id  — should fill first
  add_ask(p, 50); // second_id — should fill second

  add_bid(p, 50); // aggressive bid — fills 50 from first_id

  ASSERT_EQ(trades_.size(), 1u);
  EXPECT_EQ(trades_[0].passive_id, first_id);

  // second_id should still be resting.
  EXPECT_EQ(engine_.book().order_count_at(Side::ASK, p), 1u);
  EXPECT_EQ(engine_.book().total_qty_at(Side::ASK, p), 50u);
}

// ---------------------------------------------------------------------------
// Market order on empty book
// ---------------------------------------------------------------------------

TEST_F(LOBTest, MarketOrderEmptyBookProducesNoTrade) {
  const OrderError err = market_buy(100);
  // No crash, no trade, OK return (unfilled market is silently discarded).
  EXPECT_EQ(err, OrderError::OK);
  EXPECT_TRUE(trades_.empty());
}

// ---------------------------------------------------------------------------
// Duplicate order id
// ---------------------------------------------------------------------------

TEST_F(LOBTest, DuplicateOrderIdRejected) {
  const Price p = price_from_double(100.00);
  // Manually submit two orders with the same id bypassing the helper.
  const OrderId dup_id = 42;
  const auto r1 = engine_.submit_limit(dup_id, Side::BID, p, 100, ts_);
  const auto r2 = engine_.submit_limit(dup_id, Side::BID, p, 100, ts_);
  EXPECT_EQ(r1, OrderError::OK);
  EXPECT_EQ(r2, OrderError::DUPLICATE_ID);
}

// ---------------------------------------------------------------------------
// Invalid inputs
// ---------------------------------------------------------------------------

TEST_F(LOBTest, ZeroQuantityRejected) {
  const auto err =
      engine_.submit_limit(1, Side::BID, price_from_double(100.0), 0, ts_);
  EXPECT_EQ(err, OrderError::INVALID_QUANTITY);
}

TEST_F(LOBTest, NegativePriceLimitRejected) {
  const auto err = engine_.submit_limit(1, Side::BID, -1, 100, ts_);
  EXPECT_EQ(err, OrderError::INVALID_PRICE);
}
