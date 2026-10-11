#pragma once

#include <cstdint>
#include <string_view>
#include <cstddef>

namespace lob {

constexpr int64_t PRICE_SCALE = 100'000'000LL; // 1e8

/**
 * @brief Parses a decimal string representation into an int64_t scaled by 1e8.
 * 
 * Rules & Guarantees:
 * - Deterministic pure-integer math. Zero double / float.
 * - No dynamic allocations, no strtod / atof / sscanf.
 * - Leading '+' or '-' supported; bare signs or empty strings return false.
 * - Digits beyond 8 fractional places are truncated. (Justification: In crypto/HFT,
 *   1e-8 is 1 satoshi, the minimum discrete tick for BTC on Binance. Truncation
 *   ensures sub-satoshi fractions do not artificially inflate the discrete tick count).
 * - Overflow detection: prevents undefined integer overflow on extreme values.
 * 
 * @param str Input decimal string view.
 * @param out Output scaled int64_t value.
 * @return true if valid and parsed successfully, false otherwise.
 */
bool parse_scaled(std::string_view str, int64_t& out) noexcept;

/**
 * @brief Formats a scaled int64_t (1e8) into decimal string form (e.g. 123.45000000).
 * 
 * @param val Scaled integer value.
 * @param dst Destination buffer.
 * @param dst_len Capacity of destination buffer (must be >= 32).
 * @return Number of characters written, or 0 on buffer overflow.
 */
size_t format_scaled(int64_t val, char* dst, size_t dst_len) noexcept;

} // namespace lob
