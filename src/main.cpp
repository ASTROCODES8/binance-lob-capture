#include "replay.hpp"
#include "processor.hpp"
#include "net.hpp"
#include "spsc_ring.hpp"
#include "shutdown.hpp"
#include "types.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <thread>
#include <chrono>

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
    std::string host;
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
    if (host.empty()) {
        host = (venue == binance::Venue::Usdm) ? "fstream.binance.com" : "data-stream.binance.vision";
    }

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

    // Live Capture Mode (Multi-Threaded: NET Thread + PROCESSOR Thread)
    lob::install_signal_handler();
    std::filesystem::create_directories(output_dir);

    std::cout << "Starting Binance LOB Live Capture Engine...\n"
              << "Venue:      " << binance::venue_to_string(venue) << "\n"
              << "Host:       " << host << "\n"
              << "Symbols ("  << symbols.size() << "): ";
    for (size_t i = 0; i < symbols.size(); ++i) {
        std::cout << symbols[i] << (i + 1 < symbols.size() ? ", " : "\n");
    }
    std::cout << "Output Dir: " << output_dir << "\n"
              << "Duration:   " << (duration_sec == 0 ? "Unlimited (Press Ctrl+C to stop)" : std::to_string(duration_sec) + "s") << "\n\n";

    auto ring = std::make_unique<lob::SpscRingBuffer<lob::RingSlot, 2048>>();
    lob::Processor processor(output_dir, symbols, venue, false /* live capture */);

    lob::NetClientConfig net_config;
    net_config.venue = venue;
    net_config.host = host;
    net_config.port = (venue == binance::Venue::Usdm) ? "443" : "9443";
    net_config.symbols = symbols;
    net_config.shard_id = 1;

    lob::NetClient net_client(net_config, *ring, processor);

    // Optional duration watchdog timer
    std::thread timer_thread;
    if (duration_sec > 0) {
        timer_thread = std::thread([duration_sec]() {
            auto start_time = std::chrono::steady_clock::now();
            while (!lob::is_stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= static_cast<int64_t>(duration_sec)) {
                    std::cout << "\n[MAIN] Duration limit (" << duration_sec << "s) reached. Stopping capture...\n";
                    lob::request_stop();
                    break;
                }
            }
        });
    }

    // 1. Launch NET Thread
    std::thread net_thread([&net_client]() {
        net_client.run();
    });

    // 2. Launch PROCESSOR Thread
    std::thread proc_thread([&ring, &processor]() {
        while (!lob::is_stop_requested() || !ring->empty()) {
            if (ring->check_and_clear_gap()) {
                processor.on_ring_overflow();
            }

            const lob::RingSlot* slot = ring->peek();
            if (slot) {
                std::string_view raw(slot->data, slot->len);
                processor.on_message(raw, slot->recv_ns, slot->shard, slot->epoch, slot->seq);
                ring->pop();
            } else {
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #endif
            }
        }
    });

    // Wait for shutdown signal (Ctrl+C or timer)
    if (timer_thread.joinable()) timer_thread.join();
    if (net_thread.joinable()) net_thread.join();
    if (proc_thread.joinable()) proc_thread.join();

    std::cout << "\n[MAIN] Performing final buffer flush...\n";
    processor.flush();
    processor.close();

    const auto& m = processor.metrics();
    const auto& nm = net_client.metrics();
    std::cout << "\n================= SESSION SUMMARY =================\n"
              << "Network Messages Received: " << nm.messages_received.load() << "\n"
              << "Network Bytes Received:    " << nm.bytes_received.load() << "\n"
              << "Network Reconnects:        " << nm.reconnects.load() << "\n"
              << "Oversized Frame Drops:     " << nm.oversized_drops.load() << "\n"
              << "Ring Overflow Drops:       " << nm.ring_overflow_drops.load() << "\n"
              << "Processor Rows Written:    " << m.rows_written.load() << "\n"
              << "Parse Errors:              " << m.parse_errors.load() << "\n"
              << "Sequence Gaps:             " << m.gaps.load() << "\n"
              << "p50 Processing Latency:    " << m.latency.p50_ns() << " ns\n"
              << "p99 Processing Latency:    " << m.latency.p99_ns() << " ns\n"
              << "Max Processing Latency:    " << m.latency.max_ns() << " ns\n"
              << "===================================================\n";

    return 0;
}
