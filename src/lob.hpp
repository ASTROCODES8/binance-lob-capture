#pragma once

#include "types.hpp"
#include "decimal.hpp"
#include <map>
#include <vector>
#include <array>
#include <cstdint>
#include <string_view>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace lob {

struct LevelUpdate {
    int64_t price{0};
    int64_t qty{0};
};

struct BookSnapshot {
    std::array<int64_t, 5> bid_prices{};
    std::array<int64_t, 5> bid_sizes{};
    std::array<int64_t, 5> ask_prices{};
    std::array<int64_t, 5> ask_sizes{};
};

struct DiffPayload {
    uint64_t U{0};
    uint64_t u{0};
    uint64_t pu{0};
    uint64_t E{0};
    std::vector<LevelUpdate> bids;
    std::vector<LevelUpdate> asks;
};

struct Depth5Payload {
    uint64_t last_update_id{0};
    std::vector<LevelUpdate> bids;
    std::vector<LevelUpdate> asks;
};

struct TradePayload {
    int64_t price{0};
    int64_t qty{0};
    bool is_buyer_maker{false};
    uint64_t trade_id{0};
    uint64_t trade_time{0};
};

/**
 * @brief Maintains a local order book (LOB) for a symbol.
 * 
 * - Bids ordered descending (std::greater<int64_t>)
 * - Asks ordered ascending (std::less<int64_t>)
 * - Supports depth diff update semantics (qty == 0 removes price level)
 * - Supports depth5 re-anchoring
 * - Top-5 extraction O(5)
 * - Crossed-book validation
 */
class OrderBook {
public:
    OrderBook() = default;

    // Apply incremental diff updates
    void apply_diff(const std::vector<LevelUpdate>& bids, const std::vector<LevelUpdate>& asks);

    // Re-anchor book from partial depth5 snapshot:
    // erases every level at-or-better than the worst depth5 level, then inserts snapshot levels
    void replace_top5_from_depth5(const std::vector<LevelUpdate>& depth5_bids,
                                  const std::vector<LevelUpdate>& depth5_asks);

    // Extract top 5 levels for each side, zero-padded if fewer than 5 exist
    [[nodiscard]] BookSnapshot top5() const noexcept;

    [[nodiscard]] int64_t best_bid() const noexcept {
        return bids_.empty() ? 0 : bids_.begin()->first;
    }

    [[nodiscard]] int64_t best_ask() const noexcept {
        return asks_.empty() ? 0 : asks_.begin()->first;
    }

    [[nodiscard]] bool crossed() const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return false;
        }
        return bids_.begin()->first >= asks_.begin()->first;
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        is_anchored_ = false;
        prev_u_ = 0;
    }

    [[nodiscard]] bool is_anchored() const noexcept { return is_anchored_; }
    void set_anchored(bool a) noexcept { is_anchored_ = a; }

    [[nodiscard]] uint64_t prev_u() const noexcept { return prev_u_; }
    void set_prev_u(uint64_t u) noexcept { prev_u_ = u; }

    [[nodiscard]] size_t bid_depth() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_depth() const noexcept { return asks_.size(); }

    // Direct accessors for inspection / testing
    const auto& bids() const noexcept { return bids_; }
    const auto& asks() const noexcept { return asks_; }

private:
    std::map<int64_t, int64_t, std::greater<int64_t>> bids_;
    std::map<int64_t, int64_t, std::less<int64_t>> asks_;
    bool is_anchored_{false};
    uint64_t prev_u_{0};
};

/**
 * @brief Reusable JSON payload parser using simdjson without per-message allocations.
 */
class JsonPayloadParser {
public:
    JsonPayloadParser();

    bool parse_diff(std::string_view data_slice, DiffPayload& out);
    bool parse_depth5(std::string_view data_slice, Depth5Payload& out);
    bool parse_trade(std::string_view data_slice, TradePayload& out);

private:
    simdjson::dom::parser parser_;
    std::string padded_buffer_;
};

} // namespace lob
