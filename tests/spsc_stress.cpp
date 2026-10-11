#include "../src/spsc_ring.hpp"
#include "../src/shutdown.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <iomanip>

struct StressMessage {
    uint64_t seq{0};
    uint64_t checksum{0};
};

int main() {
    std::cout << "====================================================\n"
              << "   SPSC Ring Buffer 10M Producer-Consumer Stress Test\n"
              << "====================================================\n";

    constexpr size_t CAPACITY = 65536; // 64K slots
    constexpr uint64_t TOTAL_MESSAGES = 10'000'000ULL;
    constexpr uint64_t CHECKSUM_XOR = 0xDEADBEEFCAFEBABEULL;

    auto ring = std::make_unique<lob::SpscRingBuffer<StressMessage, CAPACITY>>();

    std::atomic<bool> start_flag{false};
    std::atomic<uint64_t> ordering_errors{0};
    std::atomic<uint64_t> checksum_errors{0};
    std::atomic<uint64_t> messages_consumed{0};

    auto t_start = std::chrono::steady_clock::now();

    // Consumer thread
    std::thread consumer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            // Spin wait for start
        }

        uint64_t expected_seq = 1;
        while (expected_seq <= TOTAL_MESSAGES) {
            const StressMessage* msg = ring->peek();
            if (msg) {
                if (msg->seq != expected_seq) {
                    ordering_errors.fetch_add(1, std::memory_order_relaxed);
                }
                if (msg->checksum != (msg->seq ^ CHECKSUM_XOR)) {
                    checksum_errors.fetch_add(1, std::memory_order_relaxed);
                }
                ring->pop();
                expected_seq++;
                messages_consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #endif
            }
        }
    });

    // Producer thread
    std::thread producer([&]() {
        start_flag.store(true, std::memory_order_release);

        for (uint64_t i = 1; i <= TOTAL_MESSAGES; ++i) {
            StressMessage msg;
            msg.seq = i;
            msg.checksum = i ^ CHECKSUM_XOR;

            while (!ring->try_push(msg)) {
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #endif
            }
        }
    });

    producer.join();
    consumer.join();

    auto t_end = std::chrono::steady_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
    double elapsed_sec = static_cast<double>(elapsed_ns) / 1e9;
    double throughput = (elapsed_sec > 0.0) ? (static_cast<double>(TOTAL_MESSAGES) / elapsed_sec) : 0.0;
    double ns_per_msg = (static_cast<double>(elapsed_ns) / static_cast<double>(TOTAL_MESSAGES));

    std::cout << "Messages Produced: " << TOTAL_MESSAGES << "\n"
              << "Messages Consumed: " << messages_consumed.load() << "\n"
              << "Ordering Errors:   " << ordering_errors.load() << "\n"
              << "Checksum Errors:   " << checksum_errors.load() << "\n"
              << "Elapsed Time:      " << std::fixed << std::setprecision(4) << elapsed_sec << " s\n"
              << "Throughput:        " << std::fixed << std::setprecision(2) << throughput << " msgs/sec\n"
              << "Avg Transfer Time: " << std::fixed << std::setprecision(2) << ns_per_msg << " ns/msg\n";

    bool passed = (messages_consumed.load() == TOTAL_MESSAGES) &&
                  (ordering_errors.load() == 0) &&
                  (checksum_errors.load() == 0);

    // Overflow Policy Verification
    std::cout << "\n--- Testing Overflow Policy ---\n";
    auto small_ring = std::make_unique<lob::SpscRingBuffer<uint64_t, 4>>();
    small_ring->try_push(1ULL);
    small_ring->try_push(2ULL);
    small_ring->try_push(3ULL);
    small_ring->try_push(4ULL);

    // 5th push must overflow
    bool pushed = small_ring->try_push(5ULL);
    bool overflow_ok = (!pushed) &&
                       (small_ring->overflow_count() == 1) &&
                       (small_ring->check_and_clear_gap() == true) &&
                       (small_ring->check_and_clear_gap() == false);

    std::cout << "Push when full returned: " << (pushed ? "true (FAIL)" : "false (PASS)") << "\n"
              << "Overflow count:          " << small_ring->overflow_count() << " (Expected: 1)\n"
              << "Gap flag asserted:       " << (overflow_ok ? "PASS" : "FAIL") << "\n"
              << "====================================================\n";

    if (passed && overflow_ok) {
        std::cout << ">> STRESS TEST PASSED SUCCESSFULLY <<\n";
        return 0;
    } else {
        std::cerr << ">> STRESS TEST FAILED <<\n";
        return 1;
    }
}
