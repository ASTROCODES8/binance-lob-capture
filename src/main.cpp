#include "replay.hpp"
#include "processor.hpp"
#include "types.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <iomanip>

namespace {

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  --venue <spot|usdm>        Target exchange venue (default: spot)\n"
              << "  --symbols <sym1,sym2,...>  Comma-separated symbols (e.g. BTCUSDT, max 20)\n"
              << "  --output-dir <path>        Directory to store output CSV files (default: output)\n"
              << "  --duration <seconds>       Capture/replay duration in seconds (0 = unlimited)\n"
              << "  --replay <path>            Replay market data CSV without network access\n"
              << "  --bench                    Replay at maximum speed and output performance metrics\n"
              << "  --host <hostname>          WebSocket host override (default: data-stream.binance.vision)\n"
              << "  --help, -h                 Display this help message\n";
}

std::vector<std::string> split_and_uppercase_symbols(const std::string& input) {
    std::vector<std::string> symbols;
    std::stringstream ss(input);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        size_t first = item.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = item.find_last_not_of(" \t\r\n");
        std::string s = item.substr(first, (last - first + 1));
        // To uppercase
        for (char& c : s) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        if (!s.empty()) {
            symbols.push_back(s);
        }
    }
    return symbols;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string venue_str = "spot";
    std::string symbols_str;
    std::string output_dir = "output";
    std::string replay_path;
    std::string host = "data-stream.binance.vision";
    uint64_t duration_sec = 0;
    bool bench = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "--venue" && i + 1 < argc) {
            venue_str = argv[++i];
        } else if (arg == "--symbols" && i + 1 < argc) {
            symbols_str = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::stoull(argv[++i]);
        } else if (arg == "--replay" && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (arg == "--bench") {
            bench = true;
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_help(argv[0]);
            return 1;
        }
    }

    binance::Venue venue = binance::string_to_venue(venue_str);

    std::vector<std::string> symbols;
    if (!symbols_str.empty()) {
        symbols = split_and_uppercase_symbols(symbols_str);
    } else {
        symbols.push_back("BTCUSDT");
    }

    // Limit check: max 20 symbols
    if (symbols.size() > lob::MAX_SYMBOLS) {
        std::cerr << "Error: symbol count (" << symbols.size()
                  << ") exceeds maximum allowed limit of " << lob::MAX_SYMBOLS << "\n";
        return 1;
    }

    // Replay mode (offline, zero network)
    if (!replay_path.empty()) {
        if (!std::filesystem::exists(replay_path)) {
            std::cerr << "Error: replay file not found: " << replay_path << "\n";
            return 1;
        }

        std::filesystem::create_directories(output_dir);

        lob::ReplayConfig config;
        config.csv_path = replay_path;
        config.output_dir = output_dir;
        config.symbols = symbols;
        config.venue = venue;
        config.duration_sec = duration_sec;
        config.bench = bench;

        lob::Processor processor(output_dir, symbols, venue, true /* is_replay */);
        lob::BenchmarkStats stats = lob::run_replay(config, processor);

        if (bench) {
            std::cout << "\n================= BENCHMARK REPORT =================\n"
                      << "Venue:            " << binance::venue_to_string(venue) << "\n"
                      << "Input CSV:        " << replay_path << "\n"
                      << "Output Directory: " << output_dir << "\n"
                      << "Messages Read:    " << stats.total_messages << "\n"
                      << "Rows Written:     " << stats.rows_written << "\n"
                      << "Parse Errors:     " << stats.total_errors << "\n"
                      << "Payload Size:     " << std::fixed << std::setprecision(2)
                      << (static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0)) << " MB\n"
                      << "Elapsed Time:     " << std::setprecision(6) << stats.elapsed_sec << " s\n"
                      << "Throughput:       " << std::setprecision(2) << stats.msgs_per_sec << " msgs/sec\n"
                      << "Average Latency:  " << std::setprecision(2) << stats.ns_per_msg << " ns/msg\n"
                      << "p50 Latency:      " << stats.p50_ns << " ns\n"
                      << "p99 Latency:      " << stats.p99_ns << " ns\n"
                      << "Max Latency:      " << stats.max_ns << " ns\n"
                      << "====================================================\n\n";
        } else {
            std::cout << "Replay completed successfully.\n"
                      << "Processed: " << stats.total_messages << " messages, "
                      << "Written: " << stats.rows_written << " rows to orderbook CSV.\n";
        }
        return 0;
    }

    // Live Capture Mode (placeholder for Task 9 Network Thread integration)
    std::cout << "Starting Binance LOB Capture...\n"
              << "Venue:      " << binance::venue_to_string(venue) << "\n"
              << "Host:       " << host << "\n"
              << "Symbols ("  << symbols.size() << "): ";
    for (size_t i = 0; i < symbols.size(); ++i) {
        std::cout << symbols[i] << (i + 1 < symbols.size() ? ", " : "\n");
    }
    std::cout << "Output Dir: " << output_dir << "\n"
              << "Duration:   " << (duration_sec == 0 ? "Unlimited" : std::to_string(duration_sec) + "s") << "\n"
              << "Note: Network client loop connects in Task 9. Use --replay <market_data.csv> for offline replay.\n";

    return 0;
}
