#include "replay.hpp"
#include <charconv>
#include <chrono>
#include <iostream>

namespace lob {

CsvReplayReader::CsvReplayReader(const std::string& path)
    : file_(path, std::ios::in | std::ios::binary) {
    line_buffer_.reserve(MAX_MESSAGE_BYTES + 1024);
    unescaped_payload_.reserve(MAX_MESSAGE_BYTES + 1024);
    symbol_buffer_.reserve(32);
}

bool CsvReplayReader::next_row(Row& out_row) {
    if (!file_.is_open()) return false;

    while (std::getline(file_, line_buffer_)) {
        if (line_buffer_.empty()) continue;

        // Strip UTF-8 BOM if present on first line
        if (!header_skipped_ && line_buffer_.size() >= 3 &&
            static_cast<uint8_t>(line_buffer_[0]) == 0xEF &&
            static_cast<uint8_t>(line_buffer_[1]) == 0xBB &&
            static_cast<uint8_t>(line_buffer_[2]) == 0xBF) {
            line_buffer_.erase(0, 3);
        }

        // Skip CSV header
        if (!header_skipped_) {
            header_skipped_ = true;
            if (line_buffer_.rfind("recv_tsec", 0) == 0) {
                continue;
            }
        }

        if (!line_buffer_.empty() && line_buffer_.back() == '\r') {
            line_buffer_.pop_back();
        }

        // Handle multiline RFC 4180 quotes: if quote count is odd, read until closed
        size_t quotes = 0;
        for (char c : line_buffer_) if (c == '"') quotes++;
        std::string continuation;
        while ((quotes % 2) != 0 && std::getline(file_, continuation)) {
            if (!continuation.empty() && continuation.back() == '\r') {
                continuation.pop_back();
            }
            for (char c : continuation) if (c == '"') quotes++;
            line_buffer_.push_back('\n');
            line_buffer_.append(continuation);
        }

        // Locate first 8 commas for fixed metadata fields
        size_t c1 = line_buffer_.find(',');
        if (c1 == std::string::npos) continue;
        size_t c2 = line_buffer_.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        size_t c3 = line_buffer_.find(',', c2 + 1);
        if (c3 == std::string::npos) continue;
        size_t c4 = line_buffer_.find(',', c3 + 1);
        if (c4 == std::string::npos) continue;
        size_t c5 = line_buffer_.find(',', c4 + 1);
        if (c5 == std::string::npos) continue;
        size_t c6 = line_buffer_.find(',', c5 + 1);
        if (c6 == std::string::npos) continue;
        size_t c7 = line_buffer_.find(',', c6 + 1);
        if (c7 == std::string::npos) continue;
        size_t c8 = line_buffer_.find(',', c7 + 1);
        if (c8 == std::string::npos) continue;

        // 0: recv_tsec
        int64_t tsec = 0;
        std::from_chars(line_buffer_.data(), line_buffer_.data() + c1, tsec);

        // 1: recv_tnsec
        int32_t tnsec = 0;
        std::from_chars(line_buffer_.data() + c1 + 1, line_buffer_.data() + c2, tnsec);

        // 2: venue
        std::string_view venue_str(line_buffer_.data() + c2 + 1, c3 - (c2 + 1));

        // 3: stream_kind
        std::string_view stream_str(line_buffer_.data() + c3 + 1, c4 - (c3 + 1));

        // 4: shard_id
        uint32_t shard = 0;
        std::from_chars(line_buffer_.data() + c4 + 1, line_buffer_.data() + c5, shard);

        // 5: conn_epoch
        uint32_t epoch = 0;
        std::from_chars(line_buffer_.data() + c5 + 1, line_buffer_.data() + c6, epoch);

        // 6: conn_seq
        uint64_t seq = 0;
        std::from_chars(line_buffer_.data() + c6 + 1, line_buffer_.data() + c7, seq);

        // 7: symbol
        std::string_view symbol_view(line_buffer_.data() + c7 + 1, c8 - (c7 + 1));
        symbol_buffer_.assign(symbol_view.data(), symbol_view.size());

        // 8: payload_json
        std::string_view raw_payload(line_buffer_.data() + c8 + 1, line_buffer_.size() - (c8 + 1));

        // RFC 4180 unescaping of double quotes
        unescaped_payload_.clear();
        if (raw_payload.size() >= 2 && raw_payload.front() == '"' && raw_payload.back() == '"') {
            const char* p = raw_payload.data() + 1;
            const char* end = raw_payload.data() + raw_payload.size() - 1;
            while (p < end) {
                if (*p == '"' && (p + 1 < end) && *(p + 1) == '"') {
                    unescaped_payload_.push_back('"');
                    p += 2;
                } else {
                    unescaped_payload_.push_back(*p);
                    ++p;
                }
            }
        } else {
            unescaped_payload_.assign(raw_payload.data(), raw_payload.size());
        }

        // Limit check
        if (unescaped_payload_.size() > MAX_MESSAGE_BYTES) {
            std::cerr << "Warning: skipping message exceeding 64 KB limit ("
                      << unescaped_payload_.size() << " bytes)\n";
            continue;
        }

        out_row.recv_tsec = tsec;
        out_row.recv_tnsec = tnsec;
        out_row.venue = binance::string_to_venue(venue_str);
        out_row.stream_kind = binance::string_to_stream_kind(stream_str);
        out_row.shard_id = shard;
        out_row.conn_epoch = epoch;
        out_row.conn_seq = seq;
        out_row.symbol = symbol_buffer_;
        out_row.payload_json = unescaped_payload_;

        rows_read_++;
        return true;
    }

    return false;
}

BenchmarkStats run_replay(const ReplayConfig& config, Processor& processor) {
    BenchmarkStats stats{};
    CsvReplayReader reader(config.csv_path);
    if (!reader.is_open()) {
        std::cerr << "Error: failed to open replay file: " << config.csv_path << "\n";
        return stats;
    }

    CsvReplayReader::Row row;
    auto start_time = std::chrono::steady_clock::now();
    int64_t start_sim_time = -1;

    while (reader.next_row(row)) {
        if (start_sim_time < 0) {
            start_sim_time = row.recv_tsec;
        }

        if (config.duration_sec > 0 && (row.recv_tsec - start_sim_time) >= static_cast<int64_t>(config.duration_sec)) {
            break;
        }

        int64_t recv_ns = row.recv_tsec * 1'000'000'000LL + row.recv_tnsec;
        processor.on_replay_message(recv_ns, row.shard_id, row.conn_epoch, row.conn_seq,
                                    row.symbol, row.stream_kind, row.payload_json);

        stats.total_messages++;
        stats.total_bytes += row.payload_json.size();
    }

    processor.flush();
    processor.close();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    stats.elapsed_sec = static_cast<double>(elapsed_ns) / 1e9;
    if (stats.elapsed_sec > 0.0) {
        stats.msgs_per_sec = static_cast<double>(stats.total_messages) / stats.elapsed_sec;
    }
    if (stats.total_messages > 0) {
        stats.ns_per_msg = static_cast<double>(elapsed_ns) / static_cast<double>(stats.total_messages);
    }
    const auto& m = processor.metrics();
    stats.total_errors = m.parse_errors.load();
    stats.rows_written = m.rows_written.load();
    stats.p50_ns = m.latency.p50_ns();
    stats.p99_ns = m.latency.p99_ns();
    stats.max_ns = m.latency.max_ns();

    return stats;
}

} // namespace lob
