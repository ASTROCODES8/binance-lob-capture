#pragma once

#include "types.hpp"
#include "csv.hpp"
#include "envelope.hpp"
#include "lob.hpp"
#include "sync.hpp"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <string_view>

namespace lob {

// Fixed-bucket latency histogram for microsecond/nanosecond observability
class LatencyHistogram {
public:
    // Buckets: [0-500ns], [500ns-1us], [1-2us], [2-5us], [5-10us], [10-25us],
    //          [25-50us], [50-100us], [100-250us], [250-500us], [500us-1ms], [1-5ms], [>5ms]
    static constexpr size_t NUM_BUCKETS = 13;
    static constexpr uint64_t BUCKET_THRESHOLDS[NUM_BUCKETS - 1] = {
        500,        // 500 ns
        1'000,      // 1 us
        2'000,      // 2 us
        5'000,      // 5 us
        10'000,     // 10 us
        25'000,     // 25 us
        50'000,     // 50 us
        100'000,    // 100 us
        250'000,    // 250 us
        500'000,    // 500 us
        1'000'000,  // 1 ms
        5'000'000   // 5 ms
    };

    LatencyHistogram() {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }

    void record(uint64_t latency_ns) noexcept {
        max_ns_.store(std::max(max_ns_.load(std::memory_order_relaxed), latency_ns), std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);

        for (size_t i = 0; i < NUM_BUCKETS - 1; ++i) {
            if (latency_ns <= BUCKET_THRESHOLDS[i]) {
                buckets_[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        buckets_[NUM_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t max_ns() const noexcept { return max_ns_.load(std::memory_order_relaxed); }

    [[nodiscard]] uint64_t percentile_ns(double p) const noexcept {
        uint64_t total = count_.load(std::memory_order_relaxed);
        if (total == 0) return 0;

        uint64_t target = static_cast<uint64_t>(total * p);
        uint64_t accum = 0;

        for (size_t i = 0; i < NUM_BUCKETS - 1; ++i) {
            accum += buckets_[i].load(std::memory_order_relaxed);
            if (accum >= target) {
                return BUCKET_THRESHOLDS[i];
            }
        }
        return max_ns();
    }

    [[nodiscard]] uint64_t p50_ns() const noexcept { return percentile_ns(0.50); }
    [[nodiscard]] uint64_t p99_ns() const noexcept { return percentile_ns(0.99); }

private:
    std::array<std::atomic<uint64_t>, NUM_BUCKETS> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> max_ns_{0};
};

// Thread-safe metrics struct (atomic counters)
struct ProcessorMetrics {
    std::atomic<uint64_t> msgs_processed{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> gaps{0};
    std::atomic<uint64_t> reconnects{0};
    std::atomic<uint64_t> ring_drops{0};
    std::atomic<uint64_t> rows_written{0};
    LatencyHistogram latency;
};

struct SymbolContext {
    int32_t id{1};              // 1-based index in configured symbols list
    std::string symbol_name;     // Uppercase (e.g. "BTCUSDT")
    OrderBook book;
    SymbolSyncStateMachine sync_sm;
    BufferedFile md_file;
    BufferedFile ob_file;
    uint64_t ob_seq_no{0};      // Monotonic application sequence per emitted row
    bool files_initialized{false};
};

/**
 * @brief Processor owning order books, synchronization state, and file output.
 * 
 * Reused for both live WebSocket capture and offline replay mode.
 * Guarantees zero heap allocation on the hot path via pre-allocated buffers.
 */
class Processor {
public:
    Processor(std::string output_dir,
              std::vector<std::string> symbols,
              binance::Venue venue = binance::Venue::Spot,
              bool is_replay = false);

    ~Processor();

    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    /**
     * @brief Hot-path message entry point for live WebSocket frames.
     */
    void on_message(std::string_view raw,
                    int64_t recv_ns,
                    uint32_t shard,
                    uint32_t epoch,
                    uint64_t seq);

    /**
     * @brief Replay entry point from parsed market_data CSV rows.
     *        Feeds the exact same OrderBook and sync state machine.
     */
    void on_replay_message(int64_t recv_ns,
                           uint32_t shard,
                           uint32_t epoch,
                           uint64_t seq,
                           std::string_view symbol,
                           binance::StreamKind stream_kind,
                           std::string_view payload_json);

    void on_reconnect(uint32_t new_epoch);
    void on_ring_overflow();

    void flush();
    void close();

    [[nodiscard]] const ProcessorMetrics& metrics() const noexcept { return metrics_; }
    SymbolContext* find_symbol(std::string_view sym) noexcept;

private:
    void init_files_for_symbol(SymbolContext& ctx, int64_t epoch_sec);
    void process_payload(SymbolContext& ctx,
                         int64_t tsec,
                         int32_t tnsec,
                         binance::StreamKind stream_kind,
                         std::string_view data_slice);
    void write_orderbook_row(SymbolContext& ctx,
                             int64_t tsec,
                             int32_t tnsec,
                             char type,
                             char side);

    std::string output_dir_;
    std::vector<std::string> configured_symbols_;
    binance::Venue venue_{binance::Venue::Spot};
    bool is_replay_{false};
    std::vector<SymbolContext> symbol_contexts_;
    ProcessorMetrics metrics_;

    // =========================================================================
    // Reusable buffers for ZERO hot-path allocation
    // =========================================================================
    std::string line_buffer_;          // Reused for CSV row serialization
    JsonPayloadParser json_parser_;    // Reused simdjson parser and padded buffer
    DiffPayload diff_buf_;             // Reused diff payload structs
    Depth5Payload d5_buf_;             // Reused depth5 payload structs
    TradePayload trade_buf_;           // Reused trade payload structs
};

} // namespace lob
