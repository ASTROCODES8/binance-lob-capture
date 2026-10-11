#pragma once

#include <cstdint>
#include <string_view>
#include <algorithm>

namespace binance {

// Fixed-point scaling factor: 1e8 (100,000,000) ticks per unit
constexpr int64_t FIXED_POINT_SCALE = 100'000'000LL;

// Precomputed powers of 10 for scaling fractional parts up to 8 decimal places
constexpr int64_t POWERS_OF_TEN_8[9] = {
    1LL,
    10LL,
    100LL,
    1'000LL,
    10'000LL,
    100'000LL,
    1'000'000LL,
    10'000'000LL,
    100'000'000LL // 10^8
};

/**
 * @brief Parses a decimal string (e.g. "123.45678000" or "0.001") into an int64_t
 *        scaled by 1e8 (100,000,000).
 * 
 * Rules:
 * - NO floating-point math (double/float) used anywhere.
 * - Entirely deterministic integer arithmetic.
 * - No dynamic memory allocation, noexcept.
 * - Handles strings with or without quotes, signs, integer-only, or fraction-only.
 * - Up to 8 decimal places preserved; excess fractional digits (>8) are truncated.
 * 
 * @param str Input string view to parse.
 * @param out_val Reference to store the scaled integer result.
 * @return true if successfully parsed, false if string is malformed.
 */
[[nodiscard]] inline bool parse_fixed_point_1e8(std::string_view str, int64_t& out_val) noexcept {
    // Strip optional surrounding double quotes if present
    if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
        str.remove_prefix(1);
        str.remove_suffix(1);
    }

    if (str.empty()) {
        return false;
    }

    size_t idx = 0;
    const size_t len = str.size();

    // Check optional leading sign
    bool negative = false;
    if (str[idx] == '-') {
        negative = true;
        ++idx;
    } else if (str[idx] == '+') {
        ++idx;
    }

    if (idx >= len) {
        return false;
    }

    int64_t int_part = 0;
    bool has_digits = false;

    // Parse integer part
    while (idx < len && str[idx] >= '0' && str[idx] <= '9') {
        has_digits = true;
        int_part = int_part * 10 + (str[idx] - '0');
        ++idx;
    }

    int64_t frac_part = 0;
    int frac_digits = 0;

    // Parse fractional part if dot exists
    if (idx < len && str[idx] == '.') {
        ++idx;
        while (idx < len && str[idx] >= '0' && str[idx] <= '9') {
            has_digits = true;
            if (frac_digits < 8) {
                frac_part = frac_part * 10 + (str[idx] - '0');
                ++frac_digits;
            }
            // Truncate any digits beyond 8
            ++idx;
        }
    }

    if (!has_digits || idx != len) {
        return false; // Trailing garbage or no valid digits
    }

    // Scale fractional part to 8 decimal places
    if (frac_digits < 8) {
        frac_part *= POWERS_OF_TEN_8[8 - frac_digits];
    }

    int64_t total = int_part * FIXED_POINT_SCALE + frac_part;
    out_val = negative ? -total : total;
    return true;
}

/**
 * @brief Fast integer to ASCII conversion directly into buffer (no allocations).
 * 
 * @param val The 64-bit integer.
 * @param dst Destination buffer (must have at least 21 bytes capacity).
 * @return Number of characters written.
 */
inline size_t format_int64(int64_t val, char* dst) noexcept {
    if (val == 0) {
        dst[0] = '0';
        return 1;
    }

    size_t len = 0;
    uint64_t uval = 0;
    if (val < 0) {
        dst[len++] = '-';
        uval = static_cast<uint64_t>(-(val + 1)) + 1; // avoids UB on INT64_MIN
    } else {
        uval = static_cast<uint64_t>(val);
    }

    char temp[24];
    size_t temp_idx = 0;
    while (uval > 0) {
        temp[temp_idx++] = static_cast<char>('0' + (uval % 10));
        uval /= 10;
    }

    while (temp_idx > 0) {
        dst[len++] = temp[--temp_idx];
    }

    return len;
}

/**
 * @brief Fast unsigned integer to ASCII conversion directly into buffer (no allocations).
 */
inline size_t format_uint64(uint64_t val, char* dst) noexcept {
    if (val == 0) {
        dst[0] = '0';
        return 1;
    }

    char temp[24];
    size_t temp_idx = 0;
    while (val > 0) {
        temp[temp_idx++] = static_cast<char>('0' + (val % 10));
        val /= 10;
    }

    size_t len = 0;
    while (temp_idx > 0) {
        dst[len++] = temp[--temp_idx];
    }

    return len;
}

} // namespace binance
