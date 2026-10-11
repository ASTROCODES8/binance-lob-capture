#pragma once

#include "types.hpp"
#include <string_view>
#include <cstdint>
#include <array>

namespace lob {

enum class EnvelopeStatus : uint8_t {
    Success = 0,
    EmptyInput,
    MissingStreamKey,
    MissingAtSeparator,
    UnknownStreamSuffix,
    SymbolTooLong,
    MissingDataKey,
    MalformedBraces
};

[[nodiscard]] constexpr std::string_view status_to_string(EnvelopeStatus status) noexcept {
    switch (status) {
        case EnvelopeStatus::Success:             return "Success";
        case EnvelopeStatus::EmptyInput:          return "EmptyInput";
        case EnvelopeStatus::MissingStreamKey:    return "MissingStreamKey";
        case EnvelopeStatus::MissingAtSeparator:  return "MissingAtSeparator";
        case EnvelopeStatus::UnknownStreamSuffix: return "UnknownStreamSuffix";
        case EnvelopeStatus::SymbolTooLong:       return "SymbolTooLong";
        case EnvelopeStatus::MissingDataKey:      return "MissingDataKey";
        case EnvelopeStatus::MalformedBraces:     return "MalformedBraces";
    }
    return "Unknown";
}

struct EnvelopeResult {
    EnvelopeStatus status{EnvelopeStatus::EmptyInput};
    binance::StreamKind stream_kind{binance::StreamKind::Unknown};
    std::array<char, 16> symbol{};
    uint8_t symbol_len{0};
    std::string_view data_slice{};

    [[nodiscard]] std::string_view symbol_view() const noexcept {
        return std::string_view(symbol.data(), symbol_len);
    }

    [[nodiscard]] bool ok() const noexcept {
        return status == EnvelopeStatus::Success;
    }
};

/**
 * @brief Parses Binance combined stream envelope without JSON AST allocations.
 * 
 * Extracts:
 * - Stream kind: determined by stream suffix (depth@100ms -> DepthDiff, depth5@100ms -> Depth5, trade -> Trade)
 * - Symbol: converted to uppercase in fixed buffer (no allocation)
 * - Data slice: exact string_view of the inner Binance "data" JSON payload object
 * 
 * @param raw_msg Raw WebSocket text message.
 * @return EnvelopeResult containing parsed components or specific error code.
 */
[[nodiscard]] EnvelopeResult parse_envelope(std::string_view raw_msg) noexcept;

} // namespace lob
