#include "../include/fixed_point.hpp"
#include "../include/time_utils.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <string>

using namespace binance;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            g_tests_failed++; \
        } else { \
            g_tests_passed++; \
        } \
    } while (0)

void test_fixed_point_parsing() {
    int64_t val = 0;

    // Standard price strings
    TEST_ASSERT(parse_fixed_point_1e8("27123.45000000", val));
    TEST_ASSERT(val == 2712345000000LL);

    // Partial decimal places
    TEST_ASSERT(parse_fixed_point_1e8("100.5", val));
    TEST_ASSERT(val == 10050000000LL);

    // Integers without decimals
    TEST_ASSERT(parse_fixed_point_1e8("500", val));
    TEST_ASSERT(val == 50000000000LL);

    TEST_ASSERT(parse_fixed_point_1e8("0", val));
    TEST_ASSERT(val == 0LL);

    // Smallest tick (1 satoshi)
    TEST_ASSERT(parse_fixed_point_1e8("0.00000001", val));
    TEST_ASSERT(val == 1LL);

    // Fraction only
    TEST_ASSERT(parse_fixed_point_1e8(".00000001", val));
    TEST_ASSERT(val == 1LL);

    // Quoted strings (typical in Binance JSON)
    TEST_ASSERT(parse_fixed_point_1e8("\"43250.75\"", val));
    TEST_ASSERT(val == 4325075000000LL);

    // Negative numbers
    TEST_ASSERT(parse_fixed_point_1e8("-12.34", val));
    TEST_ASSERT(val == -1234000000LL);

    // Precision > 8 decimal places (truncated, not crashing)
    TEST_ASSERT(parse_fixed_point_1e8("1.123456789", val));
    TEST_ASSERT(val == 112345678LL);

    // Invalid formats
    TEST_ASSERT(!parse_fixed_point_1e8("", val));
    TEST_ASSERT(!parse_fixed_point_1e8("abc", val));
    TEST_ASSERT(!parse_fixed_point_1e8(".", val));
    TEST_ASSERT(!parse_fixed_point_1e8("12.34.56", val));
    TEST_ASSERT(!parse_fixed_point_1e8("--5", val));
}

void test_format_integers() {
    char buf[32];

    size_t len = format_int64(0, buf);
    buf[len] = '\0';
    TEST_ASSERT(std::string(buf) == "0");

    len = format_int64(1234567890LL, buf);
    buf[len] = '\0';
    TEST_ASSERT(std::string(buf) == "1234567890");

    len = format_int64(-987654321LL, buf);
    buf[len] = '\0';
    TEST_ASSERT(std::string(buf) == "-987654321");

    len = format_uint64(18446744073709551615ULL, buf);
    buf[len] = '\0';
    TEST_ASSERT(std::string(buf) == "18446744073709551615");
}

void test_time_and_date_utils() {
    char date_buf[16];

    // Epoch 0: 1970-01-01
    TEST_ASSERT(format_utc_date_from_epoch(0, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "1970-01-01") == 0);

    // Y2K leap day: 2000-02-29 00:00:00 UTC
    TEST_ASSERT(format_utc_date_from_epoch(951782400LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2000-02-29") == 0);

    // 2024-02-29 00:00:00 UTC
    TEST_ASSERT(format_utc_date_from_epoch(1709164800LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2024-02-29") == 0);

    // 2024-02-29 23:59:59 UTC (last second of leap day)
    TEST_ASSERT(format_utc_date_from_epoch(1709251199LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2024-02-29") == 0);

    // 2024-03-01 00:00:00 UTC (one second after leap day ends)
    TEST_ASSERT(format_utc_date_from_epoch(1709251200LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2024-03-01") == 0);

    // 2025-01-15 00:00:00 UTC
    TEST_ASSERT(format_utc_date_from_epoch(1736899200LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2025-01-15") == 0);

    // 2100-03-01 00:00:00 UTC (2100 is NOT a leap year: divisible by 100 but not 400)
    TEST_ASSERT(format_utc_date_from_epoch(4107542400LL, date_buf, sizeof(date_buf)));
    TEST_ASSERT(std::strcmp(date_buf, "2100-03-01") == 0);

    // Split epoch ns
    int64_t test_ns = 1736899200123456789LL;
    int64_t tsec = 0;
    int32_t tnsec = 0;
    split_epoch_ns(test_ns, tsec, tnsec);
    TEST_ASSERT(tsec == 1736899200LL);
    TEST_ASSERT(tnsec == 123456789);
    TEST_ASSERT(tsec * 1'000'000'000LL + tnsec == test_ns);
}

int main() {
    std::cout << "Running test_fixed_point and time_utils tests...\n";

    test_fixed_point_parsing();
    test_format_integers();
    test_time_and_date_utils();

    std::cout << "Tests completed. Passed: " << g_tests_passed 
              << ", Failed: " << g_tests_failed << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
