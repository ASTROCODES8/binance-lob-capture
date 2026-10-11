#pragma once

#include <string_view>
#include <cstdint>
#include <cstddef>

namespace lob {

enum class StreamKind : uint8_t {
    DepthDiff, // <symbol>@depth@100ms
    Depth5,    // <symbol>@depth5@100ms
    Trade,     // <symbol>@trade
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

enum class EnvelopeStatus : uint8_t {
    Ok = 0,
    EmptyInput,
    MissingStream,
    MissingData,
    MalformedStream,
    UnknownStreamKind,
    MalformedData
};

[[nodiscard]] constexpr std::string_view envelope_status_to_string(EnvelopeStatus s) noexcept {
    switch (s) {
        case EnvelopeStatus::Ok:                return "Ok";
        case EnvelopeStatus::EmptyInput:        return "EmptyInput";
        case EnvelopeStatus::MissingStream:     return "MissingStream";
        case EnvelopeStatus::MissingData:       return "MissingData";
        case EnvelopeStatus::MalformedStream:   return "MalformedStream";
        case EnvelopeStatus::UnknownStreamKind: return "UnknownStreamKind";
        case EnvelopeStatus::MalformedData:     return "MalformedData";
    }
    return "UnknownStatus";
}

struct ParsedEnvelope {
    EnvelopeStatus status{EnvelopeStatus::EmptyInput};
    StreamKind stream_kind{StreamKind::Unknown};
    char symbol_buf[16]{};
    size_t symbol_len{0};
    std::string_view data_slice{};

    [[nodiscard]] std::string_view symbol() const noexcept {
        return std::string_view(symbol_buf, symbol_len);
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return status == EnvelopeStatus::Ok;
    }
};

/**
 * @brief High-speed zero-allocation Binance combined-stream envelope parser.
 * 
 * Given raw JSON such as: {"stream":"btcusdt@trade","data":{"e":"trade",...}}
 * Extracts:
 * 1. Stream kind (DepthDiff, Depth5, Trade) from stream name suffix.
 * 2. Symbol converted to uppercase in a fixed stack buffer (e.g. "BTCUSDT").
 * 3. Exact raw data object slice as a std::string_view (e.g. {"e":"trade",...}).
 * 
 * Zero heap allocations, noexceptions, no full JSON DOM parse.
 * Returns error enum for all malformed cases (counted, zero UB).
 */
[[nodiscard]] ParsedEnvelope parse_envelope(std::string_view raw_msg) noexcept;

} // namespace lob
