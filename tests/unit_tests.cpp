#include "../src/decimal.hpp"
#include "../src/csv.hpp"
#include "../src/envelope.hpp"
#include "../src/lob.hpp"
#include "../src/sync.hpp"
#include "../src/processor.hpp"
#include "../src/replay.hpp"
#include "../src/spsc_ring.hpp"
#include "../src/shutdown.hpp"
#include "../src/net.hpp"
#include <iostream>
#include <thread>
#include <fstream>
#include <filesystem>
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

    // Optimization check: Date cache hit and rollover across day boundary
    format_utc_date(1736985599LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2025-01-15") == 0,
               "date cache day 1 fast path");
    format_utc_date(1736985600LL, date_buf, sizeof(date_buf));
    TEST_CHECK(std::strcmp(date_buf, "2025-01-16") == 0,
               "date cache rollover on midnight boundary");
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

void run_envelope_tests() {
    // 45. Real depth5 line from fixtures
    const std::string_view depth5_fixture =
        "{\"stream\":\"btcusdt@depth5@100ms\",\"data\":{\"lastUpdateId\":101279355790,\"bids\":[[\"83144.00000000\",\"0.24177000\"]],\"asks\":[[\"83144.01000000\",\"12.05520000\"]]}}";
    auto r1 = parse_envelope(depth5_fixture);
    TEST_CHECK(r1.ok() && r1.stream_kind == binance::StreamKind::Depth5 &&
               r1.symbol_view() == "BTCUSDT" &&
               r1.data_slice.find("\"lastUpdateId\":101279355790") != std::string_view::npos,
               "45: parse real depth5 envelope and extract data slice");

    // 46. Real depthUpdate line from fixtures
    const std::string_view diff_fixture =
        "{\"stream\":\"btcusdt@depth@100ms\",\"data\":{\"e\":\"depthUpdate\",\"E\":1791722412814,\"s\":\"BTCUSDT\",\"U\":101279355763,\"u\":101279355790,\"b\":[],\"a\":[]}}";
    auto r2 = parse_envelope(diff_fixture);
    TEST_CHECK(r2.ok() && r2.stream_kind == binance::StreamKind::DepthDiff &&
               r2.symbol_view() == "BTCUSDT" &&
               r2.data_slice.find("\"e\":\"depthUpdate\"") != std::string_view::npos,
               "46: parse real depthUpdate envelope");

    // 47. Real trade line from fixtures
    const std::string_view trade_fixture =
        "{\"stream\":\"btcusdt@trade\",\"data\":{\"e\":\"trade\",\"E\":1791722413645,\"s\":\"BTCUSDT\",\"t\":6752803196,\"p\":\"83144.00000000\",\"q\":\"0.00197000\",\"T\":1791722413645,\"m\":true,\"M\":true}}";
    auto r3 = parse_envelope(trade_fixture);
    TEST_CHECK(r3.ok() && r3.stream_kind == binance::StreamKind::Trade &&
               r3.symbol_view() == "BTCUSDT" &&
               r3.data_slice.find("\"t\":6752803196") != std::string_view::npos,
               "47: parse real trade envelope");

    // 48. Different symbol (e.g. ethusdt) correctly converted to uppercase
    const std::string_view eth_fixture =
        "{\"stream\":\"ethusdt@depth@100ms\",\"data\":{\"e\":\"depthUpdate\"}}";
    auto r4 = parse_envelope(eth_fixture);
    TEST_CHECK(r4.ok() && r4.symbol_view() == "ETHUSDT" &&
               r4.stream_kind == binance::StreamKind::DepthDiff,
               "48: parse alternate symbol ETHUSDT in uppercase");

    // 49. Malformed case 1: empty string
    auto r5 = parse_envelope("");
    TEST_CHECK(!r5.ok() && r5.status == EnvelopeStatus::EmptyInput,
               "49: reject empty envelope");

    // 50. Malformed case 2: missing "stream" key
    auto r6 = parse_envelope("{\"wrong_key\":\"btcusdt@trade\",\"data\":{}}");
    TEST_CHECK(!r6.ok() && r6.status == EnvelopeStatus::MissingStreamKey,
               "50: reject envelope missing stream key");

    // 51. Malformed case 3: missing '@' separator in stream name
    auto r7 = parse_envelope("{\"stream\":\"btcusdt_trade\",\"data\":{}}");
    TEST_CHECK(!r7.ok() && r7.status == EnvelopeStatus::MissingAtSeparator,
               "51: reject stream missing @ separator");

    // 52. Malformed case 4: unknown stream suffix
    auto r8 = parse_envelope("{\"stream\":\"btcusdt@kline_1m\",\"data\":{}}");
    TEST_CHECK(!r8.ok() && r8.status == EnvelopeStatus::UnknownStreamSuffix,
               "52: reject unknown stream suffix");

    // 53. Malformed case 5: missing "data" key
    auto r9 = parse_envelope("{\"stream\":\"btcusdt@trade\"}");
    TEST_CHECK(!r9.ok() && r9.status == EnvelopeStatus::MissingDataKey,
               "53: reject envelope missing data key");

    // 54. Malformed case 6: unclosed braces in data
    auto r10 = parse_envelope("{\"stream\":\"btcusdt@trade\",\"data\":{\"unclosed\":123");
    TEST_CHECK(!r10.ok() && r10.status == EnvelopeStatus::MalformedBraces,
               "54: reject unclosed braces in data object");

    // 55. Data with escaped quotes and braces inside string literal
    const std::string_view complex_fixture =
        "{\"stream\":\"btcusdt@trade\",\"data\":{\"msg\":\"has {brace} and \\\"quote\\\"\"}}";
    auto r11 = parse_envelope(complex_fixture);
    TEST_CHECK(r11.ok() && r11.data_slice == "{\"msg\":\"has {brace} and \\\"quote\\\"\"}",
               "55: quote-aware brace matching inside data payload");

    // Optimization check: UTF-8 BOM stripped transparently
    const std::string_view bom_fixture =
        "\xEF\xBB\xBF{\"stream\":\"btcusdt@trade\",\"data\":{\"e\":\"trade\"}}";
    auto r_bom = parse_envelope(bom_fixture);
    TEST_CHECK(r_bom.ok() && r_bom.symbol_view() == "BTCUSDT" && r_bom.stream_kind == binance::StreamKind::Trade,
               "BOM stripped transparently from envelope");
}

void run_lob_tests() {
    OrderBook ob;

    // 56. Order of levels (bids descending, asks ascending)
    std::vector<LevelUpdate> bids1 = {{10000000000LL, 100000000LL}, {10500000000LL, 200000000LL}, {9500000000LL, 300000000LL}};
    std::vector<LevelUpdate> asks1 = {{11000000000LL, 150000000LL}, {10800000000LL, 250000000LL}, {11500000000LL, 350000000LL}};
    ob.apply_diff(bids1, asks1);
    TEST_CHECK(ob.best_bid() == 10500000000LL && ob.best_ask() == 10800000000LL,
               "56: best bid (descending) and best ask (ascending) order");

    // 57. Top-5 extraction and zero-padding
    BookSnapshot snap = ob.top5();
    TEST_CHECK(snap.bid_prices[0] == 10500000000LL && snap.bid_prices[1] == 10000000000LL &&
               snap.bid_prices[2] == 9500000000LL && snap.bid_prices[3] == 0 && snap.bid_prices[4] == 0 &&
               snap.bid_sizes[0] == 200000000LL && snap.bid_sizes[3] == 0,
               "57: top-5 bid prices and sizes with zero padding");

    TEST_CHECK(snap.ask_prices[0] == 10800000000LL && snap.ask_prices[1] == 11000000000LL &&
               snap.ask_prices[2] == 11500000000LL && snap.ask_prices[3] == 0 &&
               snap.ask_sizes[0] == 250000000LL && snap.ask_sizes[3] == 0,
               "58: top-5 ask prices and sizes with zero padding");

    // 59. Level replacement
    std::vector<LevelUpdate> rep_bids = {{10000000000LL, 500000000LL}};
    ob.apply_diff(rep_bids, {});
    snap = ob.top5();
    TEST_CHECK(snap.bid_sizes[1] == 500000000LL,
               "59: level replacement updates existing quantity");

    // 60. Level removal (qty == 0)
    std::vector<LevelUpdate> rem_bids = {{10000000000LL, 0LL}};
    ob.apply_diff(rem_bids, {});
    TEST_CHECK(ob.bid_depth() == 2 && ob.bids().find(10000000000LL) == ob.bids().end(),
               "60: qty 0 removes price level from order book");

    // 61. Crossed book detection
    TEST_CHECK(!ob.crossed(), "61: normal book is uncrossed (best_bid < best_ask)");
    std::vector<LevelUpdate> cross_bid = {{10900000000LL, 100000000LL}};
    ob.apply_diff(cross_bid, {});
    TEST_CHECK(ob.crossed(), "62: crossed book detected (best_bid >= best_ask)");

    // Restore uncrossed state
    ob.apply_diff({{10900000000LL, 0LL}}, {});
    TEST_CHECK(!ob.crossed(), "63: uncrossed restored after level removal");

    // 64. Depth5 re-anchor
    // Current bids: 105 (qty 200M), 95 (qty 300M).
    // Add 104 (qty 50M), 102 (qty 50M).
    ob.apply_diff({{10400000000LL, 50000000LL}, {10200000000LL, 50000000LL}}, {});
    // Now bids: 105, 104, 102, 95.
    // Replace top from depth5 with bids: 105 (new qty 1B), 103 (qty 2B) -> worst is 103.
    // Every level >= 103 (105, 104) should be erased, and 105 (1B), 103 (2B) inserted.
    // 102 and 95 (< 103) should remain!
    std::vector<LevelUpdate> d5_bids = {{10500000000LL, 1000000000LL}, {10300000000LL, 2000000000LL}};
    ob.replace_top5_from_depth5(d5_bids, {});
    TEST_CHECK(ob.is_anchored(), "64: order book anchored by depth5");
    TEST_CHECK(ob.bids().find(10400000000LL) == ob.bids().end(),
               "65: depth5 re-anchor erased stale level 104 (>= worst depth5)");
    TEST_CHECK(ob.bids().find(10200000000LL) != ob.bids().end() &&
               ob.bids().find(9500000000LL) != ob.bids().end(),
               "66: depth5 re-anchor preserved deeper levels (< worst depth5)");

    // 67. Clear book
    ob.clear();
    TEST_CHECK(ob.bid_depth() == 0 && ob.ask_depth() == 0 && !ob.is_anchored() && ob.best_bid() == 0,
               "67: clear() resets order book completely");

    // 68. JsonPayloadParser diff parsing
    JsonPayloadParser parser;
    const std::string_view diff_payload =
        "{\"e\":\"depthUpdate\",\"E\":1791722412814,\"s\":\"BTCUSDT\",\"U\":101279355763,\"u\":101279355790,\"b\":[[\"83133.69000000\",\"0.31508000\"],[\"83131.18000000\",\"0.00000000\"]],\"a\":[[\"83156.48000000\",\"0.06012000\"]]}";
    DiffPayload dp;
    TEST_CHECK(parser.parse_diff(diff_payload, dp),
               "68: JsonPayloadParser parse_diff succeeds");
    TEST_CHECK(dp.U == 101279355763ULL && dp.u == 101279355790ULL && dp.bids.size() == 2 &&
               dp.bids[0].price == 8313369000000LL && dp.bids[0].qty == 31508000LL &&
               dp.bids[1].qty == 0LL,
               "69: JsonPayloadParser extracted scaled diff updates accurately");

    // 70. JsonPayloadParser depth5 parsing
    const std::string_view depth5_payload =
        "{\"lastUpdateId\":101279355790,\"bids\":[[\"83144.00000000\",\"0.24177000\"],[\"83143.99000000\",\"0.00056000\"],[\"83143.80000000\",\"0.00007000\"],[\"83143.63000000\",\"0.00601000\"],[\"83143.57000000\",\"0.00008000\"]],\"asks\":[[\"83144.01000000\",\"12.05520000\"],[\"83144.04000000\",\"0.00200000\"],[\"83144.05000000\",\"0.05007000\"],[\"83144.10000000\",\"0.05000000\"],[\"83144.36000000\",\"0.00400000\"]]}";
    Depth5Payload d5p;
    TEST_CHECK(parser.parse_depth5(depth5_payload, d5p),
               "70: JsonPayloadParser parse_depth5 succeeds");
    TEST_CHECK(d5p.last_update_id == 101279355790ULL && d5p.bids.size() == 5 && d5p.asks.size() == 5 &&
               d5p.bids[0].price == 8314400000000LL && d5p.asks[0].price == 8314401000000LL,
               "71: JsonPayloadParser extracted 5 bids and 5 asks for depth5 snapshot");

    // 72. JsonPayloadParser trade parsing
    const std::string_view trade_payload =
        "{\"e\":\"trade\",\"E\":1791722413645,\"s\":\"BTCUSDT\",\"t\":6752803196,\"p\":\"83144.00000000\",\"q\":\"0.00197000\",\"T\":1791722413645,\"m\":true,\"M\":true}";
    TradePayload tp;
    TEST_CHECK(parser.parse_trade(trade_payload, tp),
               "72: JsonPayloadParser parse_trade succeeds");
    TEST_CHECK(tp.trade_id == 6752803196ULL && tp.price == 8314400000000LL &&
               tp.qty == 197000LL && tp.is_buyer_maker == true,
               "73: JsonPayloadParser extracted trade price, size, and maker side");
}

void run_sync_tests() {
    // 74. Initial state is AwaitingSnapshot; diffs and trades are dropped
    SymbolSyncStateMachine sm;
    TEST_CHECK(sm.state() == SyncState::AwaitingSnapshot && !sm.is_synced(),
               "74: initial state is AwaitingSnapshot");
    TEST_CHECK(sm.on_diff(100, 105) == SyncAction::Drop && sm.dropped_count() == 1,
               "75: unanchored diff dropped while AwaitingSnapshot");
    TEST_CHECK(sm.on_trade() == SyncAction::Drop && sm.dropped_count() == 2,
               "76: unanchored trade dropped while AwaitingSnapshot");

    // 77. Clean snapshot anchor
    TEST_CHECK(sm.on_depth5(105) == SyncAction::Apply && sm.is_synced() && sm.resync_count() == 1,
               "77: depth5 anchors book and transitions to Synced");

    // 78. Stale diff with u <= lastUpdateId is dropped
    TEST_CHECK(sm.on_diff(95, 105) == SyncAction::Drop && sm.dropped_count() == 3,
               "78: stale diff with u <= lastUpdateId dropped");

    // 79. First valid bridging diff (U <= lastUpdateId+1 <= u) applied
    TEST_CHECK(sm.on_diff(100, 110) == SyncAction::Apply && sm.prev_u() == 110,
               "79: first diff bridging snapshot applied successfully");

    // 80. Subsequent continuous diffs (U == prev_u + 1) applied
    TEST_CHECK(sm.on_diff(111, 120) == SyncAction::Apply && sm.prev_u() == 120,
               "80: subsequent continuous diff U == prev_u + 1 applied");
    TEST_CHECK(sm.on_diff(121, 130) == SyncAction::Apply && sm.prev_u() == 130,
               "81: continuous diff chain maintained");
    TEST_CHECK(sm.on_trade() == SyncAction::Apply,
               "82: trade applied when Synced");

    // 83. Sequence gap detection: expected 131, received 135
    TEST_CHECK(sm.on_diff(135, 140) == SyncAction::Resync && sm.state() == SyncState::Invalid &&
               sm.gap_count() == 1,
               "83: gap in U detects missed updates, triggers Resync and Invalid state");

    // 84. Diffs and trades dropped while Invalid
    TEST_CHECK(sm.on_diff(141, 145) == SyncAction::Drop,
               "84: diff dropped while in Invalid state");
    TEST_CHECK(sm.on_trade() == SyncAction::Drop,
               "85: trade dropped while in Invalid state");

    // 86. Resync recovery on new depth5 snapshot
    TEST_CHECK(sm.on_depth5(150) == SyncAction::Apply && sm.is_synced() && sm.resync_count() == 2,
               "86: recovery from Invalid on next depth5 snapshot");
    TEST_CHECK(sm.on_diff(148, 155) == SyncAction::Apply && sm.prev_u() == 155,
               "87: valid bridging diff after recovery applied");

    // 88. Duplicate / out-of-order diff detection: received 148 <= 155
    TEST_CHECK(sm.on_diff(148, 155) == SyncAction::Resync && sm.state() == SyncState::Invalid &&
               sm.gap_count() == 2,
               "88: duplicate/out-of-order diff triggers Resync and Invalid state");

    // 89. Reconnect handling increments epoch and resets to AwaitingSnapshot
    TEST_CHECK(sm.on_reconnect() == SyncAction::Resync && sm.conn_epoch() == 1 &&
               sm.state() == SyncState::AwaitingSnapshot,
               "89: on_reconnect increments conn_epoch and resets state machine");

    // 90. Ring overflow handling forces Invalid state
    sm.on_depth5(200); // re-anchor
    sm.on_diff(200, 210); // bridge
    TEST_CHECK(sm.is_synced(), "90: state machine re-synced after reconnect");
    TEST_CHECK(sm.on_ring_overflow() == SyncAction::Resync && sm.state() == SyncState::Invalid &&
               sm.counters().ring_overflows == 1,
               "91: on_ring_overflow marks book Invalid and triggers Resync");

    // 92. First diff gap after snapshot (U > lastUpdateId + 1)
    sm.on_depth5(300);
    TEST_CHECK(sm.on_diff(305, 310) == SyncAction::Resync && sm.state() == SyncState::Invalid &&
               sm.gap_count() == 3,
               "92: gap on first diff after snapshot triggers Resync");

    // 93. USD-M futures pu check hook
    SymbolSyncStateMachine usdm_sm(binance::Venue::Usdm);
    usdm_sm.on_depth5(500);
    usdm_sm.on_diff(500, 510, 500); // bridge
    TEST_CHECK(usdm_sm.on_diff(511, 520, 510) == SyncAction::Apply,
               "93: USD-M diff with matching pu applied");
    TEST_CHECK(usdm_sm.on_diff(521, 530, 999) == SyncAction::Resync && usdm_sm.state() == SyncState::Invalid,
               "94: USD-M diff with mismatched pu detected and triggers Resync");
}

void run_processor_fixture_tests() {
    std::string fixture_path = "fixtures/raw_spot_btcusdt.txt";
    if (!std::filesystem::exists(fixture_path)) {
        fixture_path = "../fixtures/raw_spot_btcusdt.txt";
    }
    if (!std::filesystem::exists(fixture_path)) {
        fixture_path = "../../fixtures/raw_spot_btcusdt.txt";
    }
    TEST_CHECK(std::filesystem::exists(fixture_path),
               "95: fixture raw_spot_btcusdt.txt exists");

    std::filesystem::create_directories("output");

    Processor proc("output", {"BTCUSDT"}, binance::Venue::Spot);

    std::ifstream file(fixture_path);
    TEST_CHECK(file.is_open(), "96: opened fixture raw_spot_btcusdt.txt");

    std::string line;
    uint64_t msg_count = 0;
    const int64_t base_recv_ns = 1736985600000000000LL; // 2025-01-16 00:00:00 UTC

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        int64_t recv_ns = base_recv_ns + static_cast<int64_t>(msg_count) * 100'000'000LL;
        proc.on_message(line, recv_ns, 1, 0, msg_count + 1);
        msg_count++;
    }

    proc.flush();
    proc.close();

    const auto& metrics = proc.metrics();

    TEST_CHECK(metrics.msgs_processed.load() == msg_count && msg_count == 300,
               "97: processor processed all 300 fixture messages");
    TEST_CHECK(metrics.parse_errors.load() == 0,
               "98: zero parse errors encountered on real Binance traffic");
    TEST_CHECK(metrics.rows_written.load() > 0,
               "99: market data and orderbook rows written successfully");
    TEST_CHECK(metrics.latency.count() == msg_count,
               "100: latency histogram recorded for every message");
    TEST_CHECK(metrics.latency.max_ns() > 0,
               "101: latency max_ns recorded and positive");
    TEST_CHECK(metrics.latency.p50_ns() > 0,
               "102: latency p50_ns calculated successfully");
    TEST_CHECK(metrics.latency.p99_ns() > 0,
               "103: latency p99_ns calculated successfully");

    std::string md_file_path = "output/market_data_spot_BTCUSDT_2025-01-16.csv";
    std::string ob_file_path = "output/market_data_spot_BTCUSDT_2025-01-16_orderbook.csv";

    TEST_CHECK(std::filesystem::exists(md_file_path),
               "104: market_data CSV created on disk");
    TEST_CHECK(std::filesystem::exists(ob_file_path),
               "105: orderbook CSV created on disk");

    std::ifstream ob_file(ob_file_path);
    TEST_CHECK(ob_file.is_open(), "106: orderbook CSV opened for inspection");

    std::string ob_header_line;
    std::string ob_row2_line;
    if (std::getline(ob_file, ob_header_line) && std::getline(ob_file, ob_row2_line)) {
        if (!ob_header_line.empty() && ob_header_line.back() == '\r') ob_header_line.pop_back();
        if (!ob_row2_line.empty() && ob_row2_line.back() == '\r') ob_row2_line.pop_back();

        size_t h_cols = count_csv_columns(ob_header_line);
        size_t r2_cols = count_csv_columns(ob_row2_line);

        TEST_CHECK(h_cols == 26, "107: orderbook CSV header has exactly 26 columns");
        TEST_CHECK(r2_cols == 26, "108: orderbook CSV row 2 has exactly 26 columns");

        std::stringstream ss(ob_row2_line);
        std::string tsec_str, tnsec_str, seq_str, id_str, type_str, side_str;
        std::getline(ss, tsec_str, ',');
        std::getline(ss, tnsec_str, ',');
        std::getline(ss, seq_str, ',');
        std::getline(ss, id_str, ',');
        std::getline(ss, type_str, ',');
        std::getline(ss, side_str, ',');

        TEST_CHECK(tsec_str == "1736985600", "109: row 2 tsec matches start epoch");
        TEST_CHECK(tnsec_str == "0", "110: row 2 tnsec matches start epoch");
        TEST_CHECK(seq_str == "1", "111: row 2 seqNo initialized to 1");
        TEST_CHECK(id_str == "1", "112: row 2 id initialized to 1 for BTCUSDT");
        TEST_CHECK(type_str == "S", "113: row 2 type is 'S' (initial snapshot)");
        TEST_CHECK(side_str == "N", "114: row 2 side is 'N' (depth snapshot)");
    } else {
        TEST_CHECK(false, "Failed to read header or row 2 from orderbook CSV");
    }

    std::ifstream md_file(md_file_path);
    std::string md_header_line, md_row2_line;
    if (std::getline(md_file, md_header_line) && std::getline(md_file, md_row2_line)) {
        if (!md_header_line.empty() && md_header_line.back() == '\r') md_header_line.pop_back();
        if (!md_row2_line.empty() && md_row2_line.back() == '\r') md_row2_line.pop_back();

        size_t md_cols = count_csv_columns(md_row2_line);
        TEST_CHECK(md_cols == 9, "115: market data CSV row 2 has exactly 9 columns");
    }
}

void run_replay_golden_tests() {
    std::string md_path = "output/market_data_spot_BTCUSDT_2025-01-16.csv";
    if (!std::filesystem::exists(md_path)) {
        md_path = "../output/market_data_spot_BTCUSDT_2025-01-16.csv";
    }
    TEST_CHECK(std::filesystem::exists(md_path), "116: input market_data CSV for replay exists");

    // 1. Replay run 1
    std::filesystem::create_directories("output_replay1");
    ReplayConfig cfg1;
    cfg1.csv_path = md_path;
    cfg1.output_dir = "output_replay1";
    cfg1.symbols = {"BTCUSDT"};
    cfg1.venue = binance::Venue::Spot;
    cfg1.bench = true;

    Processor proc1("output_replay1", {"BTCUSDT"}, binance::Venue::Spot, true /* is_replay */);
    BenchmarkStats stats1 = run_replay(cfg1, proc1);

    TEST_CHECK(stats1.total_messages == 300, "117: replay read exactly 300 messages");
    TEST_CHECK(stats1.total_errors == 0, "118: replay completed with zero parse errors");
    TEST_CHECK(stats1.rows_written > 0, "119: replay generated orderbook rows");
    TEST_CHECK(stats1.msgs_per_sec > 0.0, "120: replay throughput benchmark calculated");
    TEST_CHECK(stats1.p50_ns > 0, "121: replay p50 latency calculated");

    // 2. Replay run 2 into output_replay2 to verify 100% determinism
    std::filesystem::create_directories("output_replay2");
    ReplayConfig cfg2;
    cfg2.csv_path = md_path;
    cfg2.output_dir = "output_replay2";
    cfg2.symbols = {"BTCUSDT"};
    cfg2.venue = binance::Venue::Spot;
    cfg2.bench = false;

    Processor proc2("output_replay2", {"BTCUSDT"}, binance::Venue::Spot, true /* is_replay */);
    BenchmarkStats stats2 = run_replay(cfg2, proc2);

    TEST_CHECK(stats1.rows_written == stats2.rows_written,
               "122: replay row counts identical between runs");

    // Compare generated files byte-for-byte
    std::string ob_path1 = "output_replay1/market_data_spot_BTCUSDT_2025-01-16_orderbook.csv";
    std::string ob_path2 = "output_replay2/market_data_spot_BTCUSDT_2025-01-16_orderbook.csv";
    TEST_CHECK(std::filesystem::exists(ob_path1), "123: replay 1 orderbook CSV exists");
    TEST_CHECK(std::filesystem::exists(ob_path2), "124: replay 2 orderbook CSV exists");

    std::ifstream f1(ob_path1, std::ios::binary);
    std::ifstream f2(ob_path2, std::ios::binary);
    bool identical = true;
    size_t bytes_compared = 0;
    while (f1 && f2) {
        char c1 = 0, c2 = 0;
        f1.get(c1);
        f2.get(c2);
        if (f1 && f2) {
            bytes_compared++;
            if (c1 != c2) {
                identical = false;
                break;
            }
        }
    }
    TEST_CHECK(identical && bytes_compared > 0 && f1.eof() && f2.eof(),
               "125: replay 1 and replay 2 outputs are 100% byte-for-byte identical");

    // 3. Compare with live run orderbook output
    std::string live_ob = "output/market_data_spot_BTCUSDT_2025-01-16_orderbook.csv";
    if (std::filesystem::exists(live_ob)) {
        std::ifstream f_live(live_ob, std::ios::binary);
        std::ifstream f_rep(ob_path1, std::ios::binary);
        bool live_match = true;
        while (f_live && f_rep) {
            char cl = 0, cr = 0;
            f_live.get(cl);
            f_rep.get(cr);
            if (f_live && f_rep) {
                if (cl != cr) {
                    live_match = false;
                    break;
                }
            }
        }
        TEST_CHECK(live_match && f_live.eof() && f_rep.eof(),
                   "126: replay orderbook matches live capture orderbook byte-for-byte");
    }

    // 4. Golden file check if samples/golden_orderbook_spot_BTCUSDT.csv exists
    std::string golden_path = "samples/golden_orderbook_spot_BTCUSDT.csv";
    if (!std::filesystem::exists(golden_path)) {
        golden_path = "../samples/golden_orderbook_spot_BTCUSDT.csv";
    }
    if (std::filesystem::exists(golden_path)) {
        std::ifstream f_gold(golden_path, std::ios::binary);
        std::ifstream f_rep(ob_path1, std::ios::binary);
        bool golden_match = true;
        while (f_gold && f_rep) {
            char cg = 0, cr = 0;
            f_gold.get(cg);
            f_rep.get(cr);
            if (f_gold && f_rep) {
                if (cg != cr) {
                    golden_match = false;
                    break;
                }
            }
        }
        TEST_CHECK(golden_match && f_gold.eof() && f_rep.eof(),
                   "127: replay output matches committed golden file");
    }
}

void run_spsc_and_shutdown_tests() {
    // 128. Basic SPSC ring operations and capacity
    SpscRingBuffer<int, 8> ring;
    TEST_CHECK(ring.capacity() == 8, "128: ring capacity matches power of 2");
    TEST_CHECK(ring.empty(), "129: ring initially empty");
    TEST_CHECK(ring.size() == 0, "130: ring initial size is 0");

    // 131. Push and peek / pop
    TEST_CHECK(ring.try_push(42), "131: try_push succeeds");
    TEST_CHECK(!ring.empty(), "132: ring not empty after push");
    TEST_CHECK(ring.size() == 1, "133: ring size is 1");
    const int* p = ring.peek();
    TEST_CHECK(p != nullptr && *p == 42, "134: peek reads item without removing");
    ring.pop();
    TEST_CHECK(ring.empty(), "135: ring empty after pop");

    // 136. Zero-copy prepare_write and commit_write
    int* w = ring.prepare_write();
    TEST_CHECK(w != nullptr, "136: prepare_write returns valid slot pointer");
    *w = 100;
    ring.commit_write();
    TEST_CHECK(ring.size() == 1, "137: commit_write publishes slot to consumer");
    int val = 0;
    TEST_CHECK(ring.try_pop(val) && val == 100, "138: try_pop retrieves published value");

    // 139. Overflow policy: never silent, counts and sets gap flag
    SpscRingBuffer<int, 4> small_ring;
    for (int i = 0; i < 4; ++i) {
        TEST_CHECK(small_ring.try_push(i), "139: fill small ring to capacity");
    }
    TEST_CHECK(small_ring.overflow_count() == 0, "140: zero overflows while space available");
    TEST_CHECK(!small_ring.check_and_clear_gap(), "141: gap flag not set initially");

    // 5th push must trigger overflow
    TEST_CHECK(!small_ring.try_push(99), "142: push to full ring fails");
    TEST_CHECK(small_ring.overflow_count() == 1, "143: overflow counter incremented");
    TEST_CHECK(small_ring.check_and_clear_gap(), "144: gap flag asserted on overflow");
    TEST_CHECK(!small_ring.check_and_clear_gap(), "145: gap flag atomically cleared");

    // 146. Shutdown handler state
    reset_stop();
    TEST_CHECK(!is_stop_requested(), "146: shutdown flag initially false");
    request_stop();
    TEST_CHECK(is_stop_requested(), "147: request_stop sets stop flag to true");
    reset_stop();
    TEST_CHECK(!is_stop_requested(), "148: reset_stop restores stop flag to false");

    // 149. Concurrent SPSC producer-consumer thread test
    struct Msg {
        uint64_t seq;
        uint64_t sum;
    };
    constexpr size_t THREAD_MSG_COUNT = 100'000;
    auto concurrent_ring = std::make_unique<SpscRingBuffer<Msg, 1024>>();
    std::atomic<bool> thread_error{false};
    std::atomic<size_t> consumed{0};

    std::thread consumer([&]() {
        uint64_t expected = 1;
        while (expected <= THREAD_MSG_COUNT) {
            const Msg* m = concurrent_ring->peek();
            if (m) {
                if (m->seq != expected || m->sum != (expected * 3)) {
                    thread_error.store(true);
                }
                concurrent_ring->pop();
                expected++;
                consumed.fetch_add(1);
            }
        }
    });

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= THREAD_MSG_COUNT; ++i) {
            Msg m{i, i * 3};
            while (!concurrent_ring->try_push(m)) {
                // spin
            }
        }
    });

    producer.join();
    consumer.join();

    TEST_CHECK(!thread_error.load(), "149: concurrent SPSC thread transfer maintained exact ordering and checksum");
    TEST_CHECK(consumed.load() == THREAD_MSG_COUNT, "150: all concurrent messages consumed");
}

void run_net_client_tests() {
    // 151. Single symbol stream path
    std::string path1 = NetClient::build_combined_stream_path(binance::Venue::Spot, {"BTCUSDT"});
    TEST_CHECK(path1 == "/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade",
               "151: single symbol combined stream URL path");

    // 152. Multiple symbols stream path
    std::string path2 = NetClient::build_combined_stream_path(binance::Venue::Spot, {"BTCUSDT", "ETHUSDT"});
    TEST_CHECK(path2 == "/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade/ethusdt@depth@100ms/ethusdt@depth5@100ms/ethusdt@trade",
               "152: multiple symbols combined stream URL path");

    // 153. Case normalization in URL path
    std::string path3 = NetClient::build_combined_stream_path(binance::Venue::Spot, {"bnbusdt"});
    TEST_CHECK(path3 == "/stream?streams=bnbusdt@depth@100ms/bnbusdt@depth5@100ms/bnbusdt@trade",
               "153: case normalization produces lowercase stream keys");
}

int main() {
    std::cout << "Running Binance LOB capture unit tests...\n";

    run_decimal_tests();
    run_rfc4180_tests();
    run_date_and_filename_tests();
    run_csv_header_tests();
    run_formatting_and_buffered_file_tests();
    run_envelope_tests();
    run_lob_tests();
    run_sync_tests();
    run_processor_fixture_tests();
    run_replay_golden_tests();
    run_spsc_and_shutdown_tests();
    run_net_client_tests();

    std::cout << "Tests completed. Passed: " << g_tests_passed
              << ", Failed: " << g_tests_failed << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
