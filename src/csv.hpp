#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <vector>

namespace lob {

// Exact headers mandated by the PDF specification
inline constexpr const char* MARKET_DATA_HEADER =
    "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json\n";

inline constexpr const char* ORDERBOOK_HEADER =
    "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4\n";

/**
 * @brief RFC 4180 escaper for JSON payload that appends into a reusable buffer.
 *        Avoids per-message std::string heap allocations on the hot path.
 * 
 * Rules:
 * - Wraps output in leading and trailing double quotes.
 * - Escapes every internal '"' as '""'.
 */
void escape_rfc4180(std::string_view input, std::string& out);

/**
 * @brief Computes UTC Date (YYYY-MM-DD) from epoch seconds using Hinnant's civil day algorithm.
 *        Pure integer arithmetic; zero OS locks or allocations.
 */
bool format_utc_date(int64_t tsec, char* out_buf, size_t out_len) noexcept;

/**
 * @brief Generates standard output CSV filename:
 *        market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv
 *        or orderbook filename if is_orderbook is true.
 */
std::string build_csv_filename(std::string_view output_dir,
                               std::string_view venue,
                               std::string_view symbol,
                               int64_t epoch_sec,
                               bool is_orderbook);

/**
 * @brief Fast integer formatting appends directly into a std::string buffer.
 */
template <typename T>
void append_int(std::string& out, T val);

/**
 * @brief RAII High-Performance Buffered File Writer
 * 
 * - Opens in binary mode ("wb") with LF newlines only.
 * - Employs a 1 MB internal buffer to minimize syscall overhead.
 * - Flushes when buffer exceeds threshold, periodic 1s timeout, or on destruction.
 * - Tracks written bytes and row counts.
 * - Checks fwrite return values; ensures write errors / disk-full are not silently ignored.
 */
class BufferedFile {
public:
    static constexpr size_t BUFFER_CAPACITY = 1024 * 1024; // 1 MB

    BufferedFile() = default;
    explicit BufferedFile(const std::string& path);
    ~BufferedFile();

    BufferedFile(const BufferedFile&) = delete;
    BufferedFile& operator=(const BufferedFile&) = delete;

    BufferedFile(BufferedFile&& other) noexcept;
    BufferedFile& operator=(BufferedFile&& other) noexcept;

    bool open(const std::string& path);
    void close();

    bool write(std::string_view data);
    bool flush();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return total_bytes_written_; }
    [[nodiscard]] uint64_t rows_written() const noexcept { return rows_written_; }

    void increment_rows() noexcept { ++rows_written_; }

private:
    std::FILE* file_{nullptr};
    std::vector<char> buffer_;
    size_t buffer_used_{0};
    uint64_t total_bytes_written_{0};
    uint64_t rows_written_{0};
    std::chrono::steady_clock::time_point last_flush_time_{};
};

} // namespace lob
