#include "envelope.hpp"

namespace lob {

ParsedEnvelope parse_envelope(std::string_view raw_msg) noexcept {
    ParsedEnvelope out{};

    if (raw_msg.empty()) {
        out.status = EnvelopeStatus::EmptyInput;
        return out;
    }

    // 1. Locate "stream" key
    constexpr std::string_view STREAM_KEY = "\"stream\"";
    size_t stream_key_pos = raw_msg.find(STREAM_KEY);
    if (stream_key_pos == std::string_view::npos) {
        out.status = EnvelopeStatus::MissingStream;
        return out;
    }

    size_t colon_pos = raw_msg.find(':', stream_key_pos + STREAM_KEY.size());
    if (colon_pos == std::string_view::npos) {
        out.status = EnvelopeStatus::MalformedStream;
        return out;
    }

    // Find opening quote of stream value
    size_t stream_val_start = raw_msg.find('"', colon_pos + 1);
    if (stream_val_start == std::string_view::npos) {
        out.status = EnvelopeStatus::MalformedStream;
        return out;
    }
    ++stream_val_start; // Move past opening quote

    // Find closing quote of stream value
    size_t stream_val_end = raw_msg.find('"', stream_val_start);
    if (stream_val_end == std::string_view::npos) {
        out.status = EnvelopeStatus::MalformedStream;
        return out;
    }

    std::string_view stream_name = raw_msg.substr(stream_val_start, stream_val_end - stream_val_start);

    // 2. Extract symbol and stream kind from stream_name (e.g. "btcusdt@depth@100ms")
    size_t at_pos = stream_name.find('@');
    if (at_pos == std::string_view::npos || at_pos == 0) {
        out.status = EnvelopeStatus::MalformedStream;
        return out;
    }

    std::string_view raw_symbol = stream_name.substr(0, at_pos);
    std::string_view suffix = stream_name.substr(at_pos + 1);

    if (raw_symbol.size() >= sizeof(out.symbol_buf)) {
        out.status = EnvelopeStatus::MalformedStream;
        return out;
    }

    // Convert symbol to uppercase into fixed buffer
    for (size_t i = 0; i < raw_symbol.size(); ++i) {
        char c = raw_symbol[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out.symbol_buf[i] = c;
    }
    out.symbol_len = raw_symbol.size();
    out.symbol_buf[out.symbol_len] = '\0';

    // Identify stream kind from suffix
    // Binance stream suffixes:
    // depth5@100ms -> Depth5
    // depth@100ms  -> DepthDiff
    // trade        -> Trade
    if (suffix.rfind("depth5", 0) == 0) {
        out.stream_kind = StreamKind::Depth5;
    } else if (suffix.rfind("depth", 0) == 0) {
        out.stream_kind = StreamKind::DepthDiff;
    } else if (suffix.rfind("trade", 0) == 0) {
        out.stream_kind = StreamKind::Trade;
    } else {
        out.status = EnvelopeStatus::UnknownStreamKind;
        return out;
    }

    // 3. Locate "data" key
    constexpr std::string_view DATA_KEY = "\"data\"";
    size_t data_key_pos = raw_msg.find(DATA_KEY);
    if (data_key_pos == std::string_view::npos) {
        out.status = EnvelopeStatus::MissingData;
        return out;
    }

    colon_pos = raw_msg.find(':', data_key_pos + DATA_KEY.size());
    if (colon_pos == std::string_view::npos) {
        out.status = EnvelopeStatus::MalformedData;
        return out;
    }

    // Skip whitespace after colon to find start of data object
    size_t data_start = colon_pos + 1;
    while (data_start < raw_msg.size() &&
           (raw_msg[data_start] == ' ' || raw_msg[data_start] == '\t' ||
            raw_msg[data_start] == '\r' || raw_msg[data_start] == '\n')) {
        ++data_start;
    }

    if (data_start >= raw_msg.size() || raw_msg[data_start] != '{') {
        out.status = EnvelopeStatus::MalformedData;
        return out;
    }

    // Slice balanced JSON object by counting braces
    int depth = 0;
    bool in_string = false;
    bool escape_next = false;
    size_t data_end = std::string_view::npos;

    for (size_t i = data_start; i < raw_msg.size(); ++i) {
        char c = raw_msg[i];
        if (in_string) {
            if (escape_next) {
                escape_next = false;
            } else if (c == '\\') {
                escape_next = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else {
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    data_end = i;
                    break;
                }
            }
        }
    }

    if (data_end == std::string_view::npos || depth != 0) {
        out.status = EnvelopeStatus::MalformedData;
        return out;
    }

    out.data_slice = raw_msg.substr(data_start, data_end - data_start + 1);
    out.status = EnvelopeStatus::Ok;
    return out;
}

} // namespace lob
