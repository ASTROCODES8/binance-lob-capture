#pragma once

#include "types.hpp"
#include "processor.hpp"
#include <string>
#include <vector>
#include <string_view>
#include <fstream>
#include <cstdint>

namespace lob {

inline constexpr size_t MAX_SYMBOLS = 20;
inline constexpr size_t MAX_MESSAGE_BYTES = 65536; // 64 KB per specification limits

struct ReplayConfig {
    std::string csv_path;
    std::string output_dir{"output"};
    std::vector<std::string> symbols{"BTCUSDT"};
    binance::Venue venue{binance::Venue::Spot};
    uint64_t duration_sec{0}; // 0 = unlimited / full file
    bool bench{false};
};

struct BenchmarkStats {
    uint64_t total_messages{0};
    uint64_t total_errors{0};
    uint64_t total_bytes{0};
    double elapsed_sec{0.0};
    double msgs_per_sec{0.0};
    double ns_per_msg{0.0};
    uint64_t p50_ns{0};
    uint64_t p99_ns{0};
    uint64_t max_ns{0};
    uint64_t rows_written{0};
};

/**
 * @brief High-speed zero-allocation RFC 4180 CSV record reader for market_data_*.csv.
 *        Correctly parses double-quoted payloads, escaped internal quotes, and newlines.
 */
class CsvReplayReader {
public:
    explicit CsvReplayReader(const std::string& path);
    ~CsvReplayReader() = default;

    CsvReplayReader(const CsvReplayReader&) = delete;
    CsvReplayReader& operator=(const CsvReplayReader&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    struct Row {
        int64_t recv_tsec{0};
        int32_t recv_tnsec{0};
        binance::Venue venue{binance::Venue::Spot};
        binance::StreamKind stream_kind{binance::StreamKind::Unknown};
        uint32_t shard_id{0};
        uint32_t conn_epoch{0};
        uint64_t conn_seq{0};
        std::string_view symbol;
        std::string_view payload_json;
    };

    /**
     * @brief Reads next row, unescaping RFC 4180 quotes into reusable internal buffer.
     * @return true if row parsed, false on EOF or unrecoverable error.
     */
    bool next_row(Row& out_row);

    [[nodiscard]] uint64_t rows_read() const noexcept { return rows_read_; }

private:
    std::ifstream file_;
    std::string line_buffer_;
    std::string unescaped_payload_;
    std::string symbol_buffer_;
    uint64_t rows_read_{0};
    bool header_skipped_{false};
};

/**
 * @brief Drives replay from an existing market_data CSV through Processor without network code.
 */
BenchmarkStats run_replay(const ReplayConfig& config, Processor& processor);

} // namespace lob
