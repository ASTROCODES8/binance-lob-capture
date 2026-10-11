#include "csv.hpp"
#include <charconv>
#include <cstring>
#include <system_error>
#include <iostream>

namespace lob {

void escape_rfc4180(std::string_view input, std::string& out) {
    // Count quotes to pre-allocate capacity
    size_t extra_quotes = 0;
    for (char c : input) {
        if (c == '"') {
            ++extra_quotes;
        }
    }

    const size_t needed = input.size() + extra_quotes + 2; // +2 for surrounding quotes
    out.reserve(out.size() + needed);

    out.push_back('"');
    for (char c : input) {
        if (c == '"') {
            out.push_back('"');
            out.push_back('"');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

namespace {
// Thread-local day epoch cache: avoids re-running civil division when successive ticks occur on the same day
thread_local int64_t tl_cached_days = -9999999999LL;
thread_local char tl_cached_date[12] = {0};
} // anonymous namespace

bool format_utc_date(int64_t tsec, char* out_buf, size_t out_len) noexcept {
    if (out_buf == nullptr || out_len < 11) {
        return false;
    }

    // Days since 1970-01-01
    int64_t days = tsec / 86400LL;
    if (tsec < 0 && (tsec % 86400LL != 0)) {
        days -= 1;
    }

    // Fast path: if day epoch matches cached value, copy pre-formatted date string
    if (days == tl_cached_days) {
        std::memcpy(out_buf, tl_cached_date, 11);
        return true;
    }

    // Shift epoch from 1970-01-01 to 0000-03-01
    const int64_t shifted_days = days + 719468;
    const int64_t era = (shifted_days >= 0 ? shifted_days : shifted_days - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(shifted_days - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint32_t d = static_cast<uint32_t>(doy - (153 * mp + 2) / 5 + 1);
    const uint32_t m = static_cast<uint32_t>(mp < 10 ? mp + 3 : mp - 9);
    const int64_t year = y + (m <= 2 ? 1 : 0);

    // Format YYYY-MM-DD into thread-local cache
    auto uyear = static_cast<uint32_t>(year);
    tl_cached_date[0] = static_cast<char>('0' + (uyear / 1000) % 10);
    tl_cached_date[1] = static_cast<char>('0' + (uyear / 100) % 10);
    tl_cached_date[2] = static_cast<char>('0' + (uyear / 10) % 10);
    tl_cached_date[3] = static_cast<char>('0' + uyear % 10);
    tl_cached_date[4] = '-';

    tl_cached_date[5] = static_cast<char>('0' + (m / 10));
    tl_cached_date[6] = static_cast<char>('0' + (m % 10));
    tl_cached_date[7] = '-';

    tl_cached_date[8] = static_cast<char>('0' + (d / 10));
    tl_cached_date[9] = static_cast<char>('0' + (d % 10));
    tl_cached_date[10] = '\0';

    tl_cached_days = days;
    std::memcpy(out_buf, tl_cached_date, 11);
    return true;
}

std::string build_csv_filename(std::string_view output_dir,
                               std::string_view venue,
                               std::string_view symbol,
                               int64_t epoch_sec,
                               bool is_orderbook) {
    char date_buf[16];
    format_utc_date(epoch_sec, date_buf, sizeof(date_buf));

    std::string path;
    if (!output_dir.empty()) {
        path.append(output_dir);
        if (path.back() != '/' && path.back() != '\\') {
            path.push_back('/');
        }
    }
    path.append("market_data_");
    path.append(venue);
    path.push_back('_');
    path.append(symbol);
    path.push_back('_');
    path.append(date_buf);
    if (is_orderbook) {
        path.append("_orderbook");
    }
    path.append(".csv");
    return path;
}

template <typename T>
void append_int(std::string& out, T val) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
    if (ec == std::errc()) {
        out.append(buf, static_cast<size_t>(ptr - buf));
    }
}

// Explicit template instantiations for commonly used integer types
template void append_int<int32_t>(std::string&, int32_t);
template void append_int<uint32_t>(std::string&, uint32_t);
template void append_int<int64_t>(std::string&, int64_t);
template void append_int<uint64_t>(std::string&, uint64_t);

BufferedFile::BufferedFile(const std::string& path) {
    open(path);
}

BufferedFile::~BufferedFile() {
    close();
}

BufferedFile::BufferedFile(BufferedFile&& other) noexcept
    : file_(other.file_)
    , buffer_(std::move(other.buffer_))
    , buffer_used_(other.buffer_used_)
    , total_bytes_written_(other.total_bytes_written_)
    , rows_written_(other.rows_written_)
    , last_flush_time_(other.last_flush_time_) {
    other.file_ = nullptr;
    other.buffer_used_ = 0;
}

BufferedFile& BufferedFile::operator=(BufferedFile&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        buffer_ = std::move(other.buffer_);
        buffer_used_ = other.buffer_used_;
        total_bytes_written_ = other.total_bytes_written_;
        rows_written_ = other.rows_written_;
        last_flush_time_ = other.last_flush_time_;
        other.file_ = nullptr;
        other.buffer_used_ = 0;
    }
    return *this;
}

bool BufferedFile::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        std::cerr << "Failed to open output file: " << path << "\n";
        return false;
    }
    buffer_.resize(BUFFER_CAPACITY);
    buffer_used_ = 0;
    total_bytes_written_ = 0;
    rows_written_ = 0;
    last_flush_time_ = std::chrono::steady_clock::now();
    return true;
}

void BufferedFile::close() {
    if (file_) {
        flush();
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool BufferedFile::write(std::string_view data) {
    if (!file_) {
        return false;
    }

    // Direct write for payloads larger than the buffer capacity
    if (data.size() >= BUFFER_CAPACITY) {
        if (!flush()) {
            return false;
        }
        size_t written = std::fwrite(data.data(), 1, data.size(), file_);
        if (written != data.size()) {
            std::cerr << "BufferedFile write error: expected " << data.size()
                      << " but wrote " << written << " bytes (disk full?)\n";
            return false;
        }
        total_bytes_written_ += written;
        return true;
    }

    // Flush if incoming chunk does not fit in remaining buffer
    if (buffer_used_ + data.size() > BUFFER_CAPACITY) {
        if (!flush()) {
            return false;
        }
    }

    std::memcpy(buffer_.data() + buffer_used_, data.data(), data.size());
    buffer_used_ += data.size();

    // Check 1-second flush interval
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush_time_ >= std::chrono::seconds(1)) {
        return flush();
    }

    return true;
}

bool BufferedFile::flush() {
    if (!file_) {
        return false;
    }

    if (buffer_used_ > 0) {
        size_t written = std::fwrite(buffer_.data(), 1, buffer_used_, file_);
        if (written != buffer_used_) {
            std::cerr << "BufferedFile flush error: expected " << buffer_used_
                      << " but wrote " << written << " bytes (disk full?)\n";
            return false;
        }
        total_bytes_written_ += written;
        buffer_used_ = 0;
    }

    std::fflush(file_);
    last_flush_time_ = std::chrono::steady_clock::now();
    return true;
}

} // namespace lob
