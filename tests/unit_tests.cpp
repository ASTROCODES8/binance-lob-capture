#include "../src/decimal.hpp"
#include "../src/csv.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

using namespace lob;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CHECK(cond, desc) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] (" << desc << ") at " << __FILE__ << ":" << __LINE__ << "\n"; \
            g_tests_failed++; \
        } else { \
            g_tests_passed++; \
        } \
    } while (0)

// Helper: minimal RFC 4180 parser to verify unescaping round-trip
std::string unescape_rfc4180(std::string_view escaped) {
    if (escaped.size() < 2 || escaped.front() != '"' || escaped.back() != '"') {
        return std::string(escaped);
    }
    std::string unescaped;
    unescaped.reserve(escaped.size());
    for (size_t i = 1; i < escaped.size() - 1; ++i) {
        if (escaped[i] == '"' && i + 1 < escaped.size() - 1 && escaped[i + 1] == '"') {
            unescaped.push_back('"');
            ++i; // Skip doubled quote
        } else {
            unescaped.push_back(escaped[i]);
        }
    }
    return unescaped;
}

// Count comma-separated columns in a CSV line
size_t count_csv_columns(std::string_view line) {
    if (line.empty()) return 0;
    size_t count = 1;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ',' && !in_quotes) {
            count++;
        }
    }
    return count;
}

void run_decimal_tests() {
    int64_t val = 0;

    // 1. Standard spot price
    TEST_CHECK(parse_scaled("67123.45", val) && val == 6712345000000LL,
               "1: parse standard spot price 67123.45");

    // 2. Smallest discrete tick (1 satoshi)
    TEST_CHECK(parse_scaled("0.00000001", val) && val == 1LL,
               "2: parse 0.00000001 (1 satoshi)");

    // 3. Zero integer
    TEST_CHECK(parse_scaled("0", val) && val == 0LL,
               "3: parse 0");

    // 4. Plain integer without dot
    TEST_CHECK(parse_scaled("12345", val) && val == 1234500000000LL,
               "4: parse 12345 integer without dot");

    // 5. Zero with all decimals
    TEST_CHECK(parse_scaled("0.00000000", val) && val == 0LL,
               "5: parse 0.00000000");

    // 6. Truncation beyond 8 decimal places
    TEST_CHECK(parse_scaled("1.123456789", val) && val == 112345678LL,
               "6: truncate excess decimals beyond 8 places");

    // 7. Sub-satoshi excess decimals truncated to zero
    TEST_CHECK(parse_scaled("0.000000009", val) && val == 0LL,
               "7: truncate sub-satoshi fraction to zero");

    // 8. Enclosing quotes stripped
    TEST_CHECK(parse_scaled("\"67123.45\"", val) && val == 6712345000000LL,
               "8: parse quoted decimal string");

    // 9. Negative number parsed
    TEST_CHECK(parse_scaled("-12.34", val) && val == -1234000000LL,
               "9: parse negative decimal");

    // 10. Fraction without leading zero
    TEST_CHECK(parse_scaled(".5", val) && val == 50000000LL,
               "10: parse .5 fraction");

    // 11. Large valid price
    TEST_CHECK(parse_scaled("99999.99999999", val) && val == 9999999999999LL,
               "11: parse large valid price");

    // 12. Reject empty string
    TEST_CHECK(!parse_scaled("", val),
               "12: reject empty string");

    // 13. Reject bare minus
    TEST_CHECK(!parse_scaled("-", val),
               "13: reject bare minus");

    // 14. Reject bare plus
    TEST_CHECK(!parse_scaled("+", val),
               "14: reject bare plus");

    // 15. Reject bare dot
    TEST_CHECK(!parse_scaled(".", val),
               "15: reject bare dot");

    // 16. Reject alphabetic characters
    TEST_CHECK(!parse_scaled("abc", val),
               "16: reject non-numeric string");

    // 17. Reject alphanumeric mixed
    TEST_CHECK(!parse_scaled("123a.45", val),
               "17: reject mixed alphanumeric");

    // 18. Reject multiple dots
    TEST_CHECK(!parse_scaled("12.34.56", val),
               "18: reject multiple dots");

    // 19. Reject extreme overflow
    TEST_CHECK(!parse_scaled("999999999999999999999999999.99", val),
               "19: reject integer overflow");

    // 20. format_scaled positive
    char fmt_buf[32];
    size_t fmt_len = format_scaled(6712345000000LL, fmt_buf, sizeof(fmt_buf));
    TEST_CHECK(fmt_len > 0 && std::string(fmt_buf) == "67123.45000000",
               "20: format_scaled positive 67123.45000000");

    // 21. format_scaled zero
    fmt_len = format_scaled(0LL, fmt_buf, sizeof(fmt_buf));
    TEST_CHECK(fmt_len > 0 && std::string(fmt_buf) == "0.00000000",
               "21: format_scaled zero 0.00000000");
}

void run_rfc4180_tests() {
    // 22. Simple JSON payload escaping
    std::string out1;
    escape_rfc4180("{\"e\":\"trade\"}", out1);
    TEST_CHECK(out1 == "\"{\"\"e\"\":\"\"trade\"\"}\"",
               "22: escape_rfc4180 simple JSON");

    // 23. Doubled inner quotes
    std::string out2;
    escape_rfc4180("{\"p\":\"67123.45\",\"q\":\"0.1\"}", out2);
    TEST_CHECK(out2 == "\"{\"\"p\"\":\"\"67123.45\"\",\"\"q\"\":\"\"0.1\"\"}\"",
               "23: escape_rfc4180 price-quantity JSON");

    // 24. Plain text without quotes
    std::string out3;
    escape_rfc4180("plain_text", out3);
    TEST_CHECK(out3 == "\"plain_text\"",
               "24: escape_rfc4180 plain string");

    // 25. String containing commas
    std::string out4;
    escape_rfc4180("a,b,c", out4);
    TEST_CHECK(out4 == "\"a,b,c\"",
               "25: escape_rfc4180 comma-separated values");

    // 26. Round-trip unescaping matches original input
    const std::string raw_json = "{\"stream\":\"btcusdt@trade\",\"data\":{\"e\":\"trade\",\"s\":\"BTCUSDT\",\"p\":\"67123.45\"}}";
    std::string escaped;
    escape_rfc4180(raw_json, escaped);
    std::string round_trip = unescape_rfc4180(escaped);
    TEST_CHECK(round_trip == raw_json,
               "26: RFC 4180 escape/unescape round-trip");
}

void run_date_and_filename_tests() {
    char date_buf[16];

    // 27. Epoch 0 -> 1970-01-01
    format_utc_date(0, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "1970-01-01") == 0,
               "27: epoch 0 date is 1970-01-01");

    // 28. Y2K leap day (951782400) -> 2000-02-29
    format_utc_date(951782400LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2000-02-29") == 0,
               "28: Y2K leap day 2000-02-29");

    // 29. 2024 leap day midnight (1709164800) -> 2024-02-29
    format_utc_date(1709164800LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2024-02-29") == 0,
               "29: 2024 leap day midnight 2024-02-29");

    // 30. 2024 leap day 23:59:59 boundary (1709251199) -> 2024-02-29
    format_utc_date(1709251199LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2024-02-29") == 0,
               "30: 2024 leap day 23:59:59 boundary");

    // 31. 2024 leap day rollover +1s -> 2024-03-01
    format_utc_date(1709251200LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2024-03-01") == 0,
               "31: rollover to 2024-03-01 00:00:00");

    // 32. 2025-01-15 midnight (1736899200) -> 2025-01-15
    format_utc_date(1736899200LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2025-01-15") == 0,
               "32: 2025-01-15 date check");

    // 33. 2100 century non-leap year (4107542400) -> 2100-03-01
    format_utc_date(4107542400LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2100-03-01") == 0,
               "33: 2100 non-leap year 2100-03-01");

    // 34. Market data filename generator
    std::string fn1 = build_csv_filename("", "spot", "BTCUSDT", 1736899200LL, false);
    TEST_CHECK(fn1 == "market_data_spot_BTCUSDT_2025-01-15.csv",
               "34: market data filename builder");

    // 35. Orderbook filename generator
    std::string fn2 = build_csv_filename("", "spot", "BTCUSDT", 1736899200LL, true);
    TEST_CHECK(fn2 == "market_data_spot_BTCUSDT_2025-01-15_orderbook.csv",
               "35: orderbook filename builder");

    // 36. Output directory path handling
    std::string fn3 = build_csv_filename("output", "spot", "BTCUSDT", 1736899200LL, false);
    TEST_CHECK(fn3 == "output/market_data_spot_BTCUSDT_2025-01-15.csv",
               "36: filename builder with output directory");
}

void run_csv_header_tests() {
    // 37. Exact market data header matching PDF contract
    const std::string expected_md_header =
        "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json\n";
    TEST_CHECK(std::string(MARKET_DATA_HEADER) == expected_md_header,
               "37: exact market data header match");

    // 38. Exact orderbook header matching PDF contract
    const std::string expected_ob_header =
        "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4\n";
    TEST_CHECK(std::string(ORDERBOOK_HEADER) == expected_ob_header,
               "38: exact orderbook header match");

    // 39. Orderbook header column count = 26 columns
    std::string_view ob_sv(ORDERBOOK_HEADER);
    if (!ob_sv.empty() && ob_sv.back() == '\n') {
        ob_sv.remove_suffix(1);
    }
    size_t col_count = count_csv_columns(ob_sv);
    TEST_CHECK(col_count == 26,
               "39: orderbook header has exactly 26 columns");

    // 40. Verify 26 data columns decomposition: 6 metadata + 20 price/size levels
    // Prefix: tsec,tnsec,seqNo,id,type,side (6)
    // Levels: 5 bids + 5 bid_sizes + 5 asks + 5 ask_sizes = 20
    TEST_CHECK(col_count == 6 + 20,
               "40: orderbook columns decompose to 6 metadata + 20 depth levels");
}

void run_formatting_and_buffered_file_tests() {
    // 41. append_int positive int64
    std::string s1;
    append_int(s1, 1234567890123LL);
    TEST_CHECK(s1 == "1234567890123",
               "41: append_int positive int64");

    // 42. append_int negative int64
    std::string s2;
    append_int(s2, -987654321LL);
    TEST_CHECK(s2 == "-987654321",
               "42: append_int negative int64");

    // 43. BufferedFile write & flush
    {
        BufferedFile bf("test_buffered_file.tmp");
        TEST_CHECK(bf.is_open(), "43: BufferedFile successfully opened");
        bf.write("test_row_data\n");
        bf.increment_rows();
        bf.flush();
    }

    // 44. BufferedFile RAII and row count verification
    {
        BufferedFile bf("test_buffered_file.tmp");
        bf.write("row1\n");
        bf.increment_rows();
        bf.write("row2\n");
        bf.increment_rows();
        bf.close();
        TEST_CHECK(bf.rows_written() == 2,
                   "44: BufferedFile tracks rows correctly across RAII close");
    }
    std::remove("test_buffered_file.tmp");
}

int main() {
    std::cout << "Running Binance LOB capture unit tests...\n";

    run_decimal_tests();
    run_rfc4180_tests();
    run_date_and_filename_tests();
    run_csv_header_tests();
    run_formatting_and_buffered_file_tests();

    std::cout << "Tests completed. Passed: " << g_tests_passed
              << ", Failed: " << g_tests_failed << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
