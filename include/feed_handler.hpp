#pragma once

#include "matching_engine.hpp"
#include "types.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace lob {

// ---------------------------------------------------------------------------
// FeedHandler: L2 market data feed parser and replayer.
//
// Parses the CSV feed format:
//   timestamp_ns, event_type, order_id, side, price, quantity
//
// Supported event types: ADD, CANCEL, TRADE (informational only on replay).
//
// In replay mode the handler drives a MatchingEngine and can optionally
// compare the final book state against a known-good snapshot — useful for
// regression testing against recorded sessions.
//
// Stretch goal: NASDAQ ITCH 5.0 binary parser (see parse_itch.hpp when added).
// ---------------------------------------------------------------------------

struct FeedRecord {
  Timestamp timestamp_ns;
  std::string event_type; // "ADD", "CANCEL", "TRADE"
  OrderId order_id;
  Side side;
  Price price;
  Quantity quantity;
};

class FeedHandler {
public:
  // -----------------------------------------------------------------------
  // Construction
  // -----------------------------------------------------------------------

  // engine: non-owning pointer; must outlive this FeedHandler.
  explicit FeedHandler(MatchingEngine *engine) noexcept;
  ~FeedHandler() = default;

  FeedHandler(const FeedHandler &) = delete;
  FeedHandler &operator=(const FeedHandler &) = delete;

  // -----------------------------------------------------------------------
  // Parsing
  // -----------------------------------------------------------------------

  // Parse a single CSV record from a line string.
  // Returns std::nullopt on parse error (bad format, unknown event type).
  [[nodiscard]] static std::optional<FeedRecord>
  parse_line(std::string_view line);

  // -----------------------------------------------------------------------
  // Replay
  // -----------------------------------------------------------------------

  // Read `path`, dispatch each record to the engine in timestamp order.
  // Returns the number of records processed, or -1 on file-open error.
  [[nodiscard]] int64_t replay_file(const std::filesystem::path &path);

  // Dispatch a single pre-parsed record.
  // Returns OK or the error code from the engine.
  [[nodiscard]] OrderError dispatch(const FeedRecord &rec);

  // -----------------------------------------------------------------------
  // Statistics
  // -----------------------------------------------------------------------

  [[nodiscard]] uint64_t records_processed() const noexcept {
    return records_processed_;
  }
  [[nodiscard]] uint64_t records_skipped() const noexcept {
    return records_skipped_;
  }

private:
  MatchingEngine *engine_;
  uint64_t records_processed_ = 0;
  uint64_t records_skipped_ = 0;

  // Parse helpers.
  [[nodiscard]] static Side parse_side(std::string_view s);
};

} // namespace lob
