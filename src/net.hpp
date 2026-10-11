#pragma once

#include "types.hpp"
#include "spsc_ring.hpp"
#include "processor.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

namespace lob {

struct NetClientConfig {
    binance::Venue venue{binance::Venue::Spot};
    std::string host{"data-stream.binance.vision"};
    std::string port{"9443"};
    std::vector<std::string> symbols{"BTCUSDT"};
    uint32_t shard_id{1};
};

struct NetClientMetrics {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> reconnects{0};
    std::atomic<uint64_t> oversized_drops{0};
    std::atomic<uint64_t> ring_overflow_drops{0};
};

class NetClient {
public:
    NetClient(NetClientConfig config,
              SpscRingBuffer<RingSlot, 2048>& ring,
              Processor& processor);
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    /**
     * @brief Main network thread loop. Runs until is_stop_requested() returns true.
     */
    void run();

    [[nodiscard]] const NetClientMetrics& metrics() const noexcept { return metrics_; }

    [[nodiscard]] static std::string build_combined_stream_path(
        binance::Venue venue, const std::vector<std::string>& symbols);

private:
    NetClientConfig config_;
    SpscRingBuffer<RingSlot, 2048>& ring_;
    Processor& processor_;
    NetClientMetrics metrics_;
    uint32_t conn_epoch_{0};
    uint64_t conn_seq_{0};
};

} // namespace lob
