#include "decimal.hpp"
#include <limits>
#include <charconv>

namespace lob {

namespace {

constexpr int64_t POW10[9] = {
    1LL,
    10LL,
    100LL,
    1'000LL,
    10'000LL,
    100'000LL,
    1'000'000LL,
    10'000'000LL,
    100'000'000LL // 1e8
};

// Maximum integer part that can be multiplied by 1e8 without overflowing int64_t
constexpr int64_t MAX_INT_PART = std::numeric_limits<int64_t>::max() / PRICE_SCALE;

} // anonymous namespace

bool parse_scaled(std::string_view str, int64_t& out) noexcept {
    // Strip optional enclosing quotes
    if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
        str.remove_prefix(1);
        str.remove_suffix(1);
    }

    if (str.empty()) {
        return false;
    }

    size_t idx = 0;
    const size_t len = str.size();

    bool negative = false;
    if (str[idx] == '-') {
        negative = true;
        ++idx;
    } else if (str[idx] == '+') {
        ++idx;
    }

    if (idx >= len) {
        return false; // Bare sign "-" or "+"
    }

    int64_t int_part = 0;
    bool has_digits = false;

    // Parse integer component
    while (idx < len && str[idx] >= '0' && str[idx] <= '9') {
        has_digits = true;
        int digit = str[idx] - '0';
        if (int_part > (MAX_INT_PART - digit) / 10) {
            return false; // Overflow in integer part
        }
        int_part = int_part * 10 + digit;
        ++idx;
    }

    int64_t frac_part = 0;
    int frac_count = 0;

    // Parse fractional component if decimal point is present
    if (idx < len && str[idx] == '.') {
        ++idx;
        while (idx < len && str[idx] >= '0' && str[idx] <= '9') {
            has_digits = true;
            if (frac_count < 8) {
                frac_part = frac_part * 10 + (str[idx] - '0');
                ++frac_count;
            }
            // Truncate any subsequent fractional digits (>8)
            ++idx;
        }
    }

    // Must contain at least one valid digit and consumed the entire input
    if (!has_digits || idx != len) {
        return false;
    }

    // Scale fraction to 8 decimal places
    if (frac_count < 8) {
        frac_part *= POW10[8 - frac_count];
    }

    int64_t total = int_part * PRICE_SCALE + frac_part;
    out = negative ? -total : total;
    return true;
}

size_t format_scaled(int64_t val, char* dst, size_t dst_len) noexcept {
    if (dst == nullptr || dst_len < 32) {
        return 0;
    }

    char* ptr = dst;
    if (val < 0) {
        *ptr++ = '-';
        val = -val;
    }

    int64_t int_part = val / PRICE_SCALE;
    int64_t frac_part = val % PRICE_SCALE;

    auto [p1, ec1] = std::to_chars(ptr, dst + dst_len, int_part);
    if (ec1 != std::errc()) {
        return 0;
    }
    ptr = p1;

    *ptr++ = '.';

    // Format 8-digit fractional part with zero-padding
    for (int i = 7; i >= 0; --i) {
        int64_t divisor = POW10[i];
        int digit = static_cast<int>(frac_part / divisor);
        frac_part %= divisor;
        *ptr++ = static_cast<char>('0' + digit);
    }

    *ptr = '\0';
    return static_cast<size_t>(ptr - dst);
}

} // namespace lob
