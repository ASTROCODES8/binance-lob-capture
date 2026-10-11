#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace binance {

/**
 * @brief Returns current wall-clock time in nanoseconds since Unix epoch.
 */
[[nodiscard]] inline int64_t get_wall_clock_ns() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
}

/**
 * @brief Splits total nanoseconds into seconds and nanosecond remainder.
 * 
 * @param total_ns Wall-clock nanoseconds since Unix epoch.
 * @param out_tsec Output seconds (int64).
 * @param out_tnsec Output nanosecond remainder [0, 999_999_999] (int32).
 */
inline void split_epoch_ns(int64_t total_ns, int64_t& out_tsec, int32_t& out_tnsec) noexcept {
    constexpr int64_t ONE_BILLION = 1'000'000'000LL;
    out_tsec = total_ns / ONE_BILLION;
    int64_t rem = total_ns % ONE_BILLION;
    if (rem < 0) {
        rem += ONE_BILLION;
        out_tsec -= 1;
    }
    out_tnsec = static_cast<int32_t>(rem);
}

/**
 * @brief Computes UTC Date (YYYY-MM-DD) from epoch seconds using Howard Hinnant's
 *        civil date algorithm.
 * 
 * Guarantees:
 * - Deterministic, purely integer arithmetic (no localtime / gmtime calls).
 * - Zero allocations, thread-safe, no locks.
 * 
 * @param tsec Unix epoch seconds.
 * @param out_buf Buffer of at least 11 bytes capacity (for "YYYY-MM-DD\0").
 * @param out_len Buffer capacity.
 * @return true if successfully formatted, false if buffer is too small.
 */
inline bool format_utc_date_from_epoch(int64_t tsec, char* out_buf, size_t out_len) noexcept {
    if (out_len < 11) {
        return false;
    }

    // Days since 1970-01-01
    int64_t days = tsec / 86400LL;
    if (tsec < 0 && (tsec % 86400LL != 0)) {
        days -= 1;
    }

    // Shift epoch from 1970-01-01 to 0000-03-01
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(days - era * 146097);
    const uint64_t yoe = (doe - doe / 1024 + doe / 1461 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint32_t d = static_cast<uint32_t>(doy - (153 * mp + 2) / 5 + 1);
    const uint32_t m = static_cast<uint32_t>(mp < 10 ? mp + 3 : mp - 9);
    const int64_t year = y + (m <= 2 ? 1 : 0);

    // Format YYYY-MM-DD
    // Year: 4 digits
    auto uyear = static_cast<uint32_t>(year);
    out_buf[0] = static_cast<char>('0' + (uyear / 1000) % 10);
    out_buf[1] = static_cast<char>('0' + (uyear / 100) % 10);
    out_buf[2] = static_cast<char>('0' + (uyear / 10) % 10);
    out_buf[3] = static_cast<char>('0' + uyear % 10);
    out_buf[4] = '-';

    // Month: 2 digits
    out_buf[5] = static_cast<char>('0' + (m / 10));
    out_buf[6] = static_cast<char>('0' + (m % 10));
    out_buf[7] = '-';

    // Day: 2 digits
    out_buf[8] = static_cast<char>('0' + (d / 10));
    out_buf[9] = static_cast<char>('0' + (d % 10));
    out_buf[10] = '\0';

    return true;
}

/**
 * @brief Helper returning std::string for initialization / file name generation.
 */
inline std::string get_utc_date_string(int64_t tsec) {
    char buf[12];
    if (format_utc_date_from_epoch(tsec, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "1970-01-01";
}

} // namespace binance
