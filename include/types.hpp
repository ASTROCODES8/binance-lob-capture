#pragma once

#include <cstdint>
#include <string_view>
#include <array>

namespace binance {

// FIXED_POINT_SCALE is defined in fixed_point.hpp (canonical location)

// Supported venues
enum class Venue : uint8_t {
    Spot,
    Usdm
};

[[nodiscard]] constexpr std::string_view venue_to_string(Venue v) noexcept {
    switch (v) {
        case Venue::Spot: return "spot";
        case Venue::Usdm: return "usdm";
    }
    return "unknown";
}

// Inbound stream kinds
enum class StreamKind : uint8_t {
    DepthDiff,
    Depth5,
    Trade,
    Unknown
};

[[nodiscard]] constexpr std::string_view stream_kind_to_string(StreamKind k) noexcept {
    switch (k) {
        case StreamKind::DepthDiff: return "depth_diff";
        case StreamKind::Depth5:    return "depth5";
        case StreamKind::Trade:     return "trade";
        default:                    return "unknown";
    }
}

// Side representation according to Deliverable B:
// 'B' = bid-side, 'S' = ask-side, 'N' = symmetric / N/A
enum class Side : char {
    Bid = 'B',
    Ask = 'S',
    None = 'N'
};

// Event type representation according to Deliverable B:
// 'D' = depth diff (depthUpdate), 'S' = depth5, 'T' = trade
enum class EventType : char {
    DepthDiff = 'D',
    Depth5    = 'S',
    Trade     = 'T'
};

// Top 5 order book level snapshot
struct BookLevel {
    int64_t price{0};
    int64_t size{0};
};

struct Top5Snapshot {
    std::array<BookLevel, 5> bids{};
    std::array<BookLevel, 5> asks{};
};

// Maximum raw message payload size in SPSC ring buffer slot
constexpr size_t MAX_PAYLOAD_CAPACITY = 16384; // 16 KB covers standard combined stream payloads

// Lock-free ring buffer item from NET thread -> PROCESSOR thread
struct RawMessageItem {
    int64_t recv_tsec{0};
    int32_t recv_tnsec{0};
    uint32_t conn_epoch{0};
    uint64_t conn_seq{0};
    uint32_t shard_id{0};
    int32_t symbol_id{0};      // 1-based index in configured symbols list
    Venue venue{Venue::Spot};
    StreamKind stream_kind{StreamKind::Unknown};
    uint16_t payload_len{0};
    char payload[MAX_PAYLOAD_CAPACITY];
};

} // namespace binance
