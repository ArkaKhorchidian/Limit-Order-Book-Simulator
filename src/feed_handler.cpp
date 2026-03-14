#include "feed_handler.hpp"

#include <cassert>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace lob {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FeedHandler::FeedHandler(MatchingEngine *engine) noexcept : engine_(engine) {
  assert(engine_ != nullptr &&
         "FeedHandler requires a non-null MatchingEngine");
}

// ---------------------------------------------------------------------------
// CSV line parser
// ---------------------------------------------------------------------------
//
// Format: timestamp_ns, event_type, order_id, side, price, quantity
// Whitespace around commas is stripped.  Lines beginning with '#' are comments.
//
// Returns std::nullopt on any parse error rather than throwing, consistent
// with the no-exceptions-on-data-path policy.

std::optional<FeedRecord> FeedHandler::parse_line(std::string_view line) {
  // Strip leading/trailing whitespace and skip comments.
  while (!line.empty() &&
         std::isspace(static_cast<unsigned char>(line.front())))
    line.remove_prefix(1);
  if (line.empty() || line.front() == '#')
    return std::nullopt;

  auto next_field =
      [&](std::string_view &sv) -> std::optional<std::string_view> {
    auto pos = sv.find(',');
    std::string_view field =
        (pos == std::string_view::npos) ? sv : sv.substr(0, pos);
    sv = (pos == std::string_view::npos) ? std::string_view{}
                                         : sv.substr(pos + 1);
    // Trim whitespace from field.
    while (!field.empty() &&
           std::isspace(static_cast<unsigned char>(field.front())))
      field.remove_prefix(1);
    while (!field.empty() &&
           std::isspace(static_cast<unsigned char>(field.back())))
      field.remove_suffix(1);
    if (field.empty())
      return std::nullopt;
    return field;
  };

  auto parse_uint64 = [](std::string_view sv) -> std::optional<uint64_t> {
    uint64_t val{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{})
      return std::nullopt;
    return val;
  };

  auto parse_int64 = [](std::string_view sv) -> std::optional<int64_t> {
    int64_t val{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{})
      return std::nullopt;
    return val;
  };

  auto parse_uint32 = [](std::string_view sv) -> std::optional<uint32_t> {
    uint32_t val{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{})
      return std::nullopt;
    return val;
  };

  std::string_view rem = line;

  auto ts_sv = next_field(rem);
  if (!ts_sv)
    return std::nullopt;
  auto event_sv = next_field(rem);
  if (!event_sv)
    return std::nullopt;
  auto id_sv = next_field(rem);
  if (!id_sv)
    return std::nullopt;
  auto side_sv = next_field(rem);
  if (!side_sv)
    return std::nullopt;
  auto price_sv = next_field(rem);
  if (!price_sv)
    return std::nullopt;
  auto qty_sv = next_field(rem);
  if (!qty_sv)
    return std::nullopt;

  auto ts = parse_uint64(*ts_sv);
  if (!ts)
    return std::nullopt;
  auto id = parse_uint64(*id_sv);
  if (!id)
    return std::nullopt;
  auto px = parse_int64(*price_sv);
  if (!px)
    return std::nullopt;
  auto qty = parse_uint32(*qty_sv);
  if (!qty)
    return std::nullopt;

  Side side = parse_side(*side_sv);

  return FeedRecord{
      .timestamp_ns = *ts,
      .event_type = std::string(*event_sv),
      .order_id = *id,
      .side = side,
      .price = *px,
      .quantity = *qty,
  };
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

int64_t FeedHandler::replay_file(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file.is_open())
    return -1;

  std::string line;
  while (std::getline(file, line)) {
    auto rec = parse_line(line);
    if (!rec) {
      ++records_skipped_;
      continue;
    }
    (void)dispatch(*rec);
    ++records_processed_;
  }
  return static_cast<int64_t>(records_processed_);
}

OrderError FeedHandler::dispatch(const FeedRecord &rec) {
  if (rec.event_type == "ADD") {
    return engine_->submit_limit(rec.order_id, rec.side, rec.price,
                                 rec.quantity, rec.timestamp_ns);
  }
  if (rec.event_type == "CANCEL") {
    return engine_->cancel(rec.order_id);
  }
  if (rec.event_type == "TRADE") {
    // Informational only during replay — the matching engine will have
    // generated the corresponding fills already.
    return OrderError::OK;
  }
  ++records_skipped_;
  return OrderError::OK;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Side FeedHandler::parse_side(std::string_view s) {
  // Case-insensitive comparison for robustness.
  if (s == "BID" || s == "bid" || s == "B" || s == "b")
    return Side::BID;
  return Side::ASK; // default; caller should validate upstream
}

} // namespace lob
