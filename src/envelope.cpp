#include "envelope.hpp"
#include <cctype>

namespace lob {

EnvelopeResult parse_envelope(std::string_view raw_msg) noexcept {
    EnvelopeResult result;

    if (raw_msg.empty()) {
        result.status = EnvelopeStatus::EmptyInput;
        return result;
    }

    // Optimization: Strip UTF-8 BOM if present (e.g. from Windows tools or PowerShell)
    if (raw_msg.size() >= 3 &&
        static_cast<unsigned char>(raw_msg[0]) == 0xEF &&
        static_cast<unsigned char>(raw_msg[1]) == 0xBB &&
        static_cast<unsigned char>(raw_msg[2]) == 0xBF) {
        raw_msg.remove_prefix(3);
    }

    // 1. Locate "stream" key
    const size_t stream_pos = raw_msg.find("\"stream\"");
    if (stream_pos == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingStreamKey;
        return result;
    }

    const size_t colon_pos = raw_msg.find(':', stream_pos + 8);
    if (colon_pos == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingStreamKey;
        return result;
    }

    const size_t val_start = raw_msg.find('\"', colon_pos + 1);
    if (val_start == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingStreamKey;
        return result;
    }

    const size_t val_end = raw_msg.find('\"', val_start + 1);
    if (val_end == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingStreamKey;
        return result;
    }

    const std::string_view stream_name = raw_msg.substr(val_start + 1, val_end - val_start - 1);

    // 2. Split stream_name by '@' into symbol and suffix
    const size_t at_pos = stream_name.find('@');
    if (at_pos == std::string_view::npos || at_pos == 0) {
        result.status = EnvelopeStatus::MissingAtSeparator;
        return result;
    }

    const std::string_view raw_symbol = stream_name.substr(0, at_pos);
    const std::string_view stream_suffix = stream_name.substr(at_pos + 1);

    if (raw_symbol.size() > result.symbol.size() - 1) {
        result.status = EnvelopeStatus::SymbolTooLong;
        return result;
    }

    // Convert symbol to uppercase in fixed buffer (no allocation)
    for (size_t i = 0; i < raw_symbol.size(); ++i) {
        result.symbol[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(raw_symbol[i])));
    }
    result.symbol_len = static_cast<uint8_t>(raw_symbol.size());

    // 3. Classify StreamKind from suffix using fast initial character dispatch
    if (!stream_suffix.empty()) {
        if (stream_suffix[0] == 't') {
            if (stream_suffix == "trade") {
                result.stream_kind = binance::StreamKind::Trade;
            } else {
                result.status = EnvelopeStatus::UnknownStreamSuffix;
                return result;
            }
        } else if (stream_suffix[0] == 'd') {
            if (stream_suffix == "depth@100ms" || stream_suffix == "depth") {
                result.stream_kind = binance::StreamKind::DepthDiff;
            } else if (stream_suffix == "depth5@100ms" || stream_suffix == "depth5") {
                result.stream_kind = binance::StreamKind::Depth5;
            } else {
                result.status = EnvelopeStatus::UnknownStreamSuffix;
                return result;
            }
        } else {
            result.status = EnvelopeStatus::UnknownStreamSuffix;
            return result;
        }
    } else {
        result.status = EnvelopeStatus::UnknownStreamSuffix;
        return result;
    }

    // 4. Locate "data" key
    const size_t data_pos = raw_msg.find("\"data\"");
    if (data_pos == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingDataKey;
        return result;
    }

    const size_t data_colon = raw_msg.find(':', data_pos + 6);
    if (data_colon == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingDataKey;
        return result;
    }

    const size_t obj_start = raw_msg.find('{', data_colon + 1);
    if (obj_start == std::string_view::npos) {
        result.status = EnvelopeStatus::MissingDataKey;
        return result;
    }

    // 5. Extract JSON object slice with quote-aware brace matching
    int depth = 0;
    bool in_quotes = false;
    bool escaped = false;
    size_t obj_end = std::string_view::npos;

    for (size_t i = obj_start; i < raw_msg.size(); ++i) {
        char c = raw_msg[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            if (in_quotes) {
                escaped = true;
            }
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes) {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    obj_end = i;
                    break;
                }
            }
        }
    }

    if (depth != 0 || obj_end == std::string_view::npos) {
        result.status = EnvelopeStatus::MalformedBraces;
        return result;
    }

    result.data_slice = raw_msg.substr(obj_start, obj_end - obj_start + 1);
    result.status = EnvelopeStatus::Success;
    return result;
}

} // namespace lob
