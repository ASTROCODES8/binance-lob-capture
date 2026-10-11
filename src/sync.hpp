#pragma once

#include "types.hpp"
#include <cstdint>
#include <string_view>

namespace lob {

enum class SyncState : uint8_t {
    AwaitingSnapshot,
    Synced,
    Invalid
};

[[nodiscard]] constexpr std::string_view sync_state_to_string(SyncState s) noexcept {
    switch (s) {
        case SyncState::AwaitingSnapshot: return "AwaitingSnapshot";
        case SyncState::Synced:           return "Synced";
        case SyncState::Invalid:          return "Invalid";
    }
    return "Unknown";
}

enum class SyncAction : uint8_t {
    Apply,  // Apply update to order book and emit snapshot row
    Drop,   // Drop update (stale or awaiting initial snapshot)
    Resync  // State transitioned to Invalid / book must be cleared, wait for depth5
};

[[nodiscard]] constexpr std::string_view sync_action_to_string(SyncAction a) noexcept {
    switch (a) {
        case SyncAction::Apply:  return "Apply";
        case SyncAction::Drop:   return "Drop";
        case SyncAction::Resync: return "Resync";
    }
    return "Unknown";
}

struct SyncCounters {
    uint64_t gaps{0};
    uint64_t dropped{0};
    uint64_t resyncs{0};
    uint64_t ring_overflows{0};
};

/**
 * @brief Per-symbol synchronization state machine.
 * 
 * Enforces Binance Spot synchronization rules:
 * 1. Initial State: AwaitingSnapshot. Diffs are dropped until first depth5 snapshot arrives.
 * 2. On depth5(lastUpdateId): Anchors book, transitions to Synced.
 * 3. Diffs with u <= lastUpdateId are discarded as older than snapshot.
 * 4. First applied diff must satisfy: U <= lastUpdateId + 1 <= u.
 * 5. Subsequent continuous diffs strictly require: U == prev_u + 1.
 * 6. Sequence gaps, crossed books, or SPSC ring overflows transition to Invalid and trigger Resync.
 * 7. On reconnect: increments conn_epoch, resets sequence state to AwaitingSnapshot.
 */
class SymbolSyncStateMachine {
public:
    explicit SymbolSyncStateMachine(binance::Venue venue = binance::Venue::Spot) noexcept;

    // Inbound event handlers
    SyncAction on_diff(uint64_t U, uint64_t u, uint64_t pu = 0) noexcept;
    SyncAction on_depth5(uint64_t last_update_id) noexcept;
    SyncAction on_trade() noexcept;
    SyncAction on_reconnect() noexcept;
    SyncAction on_ring_overflow() noexcept;

    [[nodiscard]] SyncState state() const noexcept { return state_; }
    [[nodiscard]] bool is_synced() const noexcept { return state_ == SyncState::Synced; }
    [[nodiscard]] uint32_t conn_epoch() const noexcept { return conn_epoch_; }
    [[nodiscard]] uint64_t prev_u() const noexcept { return prev_u_; }
    [[nodiscard]] uint64_t last_update_id() const noexcept { return last_update_id_; }

    [[nodiscard]] const SyncCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] uint64_t gap_count() const noexcept { return counters_.gaps; }
    [[nodiscard]] uint64_t dropped_count() const noexcept { return counters_.dropped; }
    [[nodiscard]] uint64_t resync_count() const noexcept { return counters_.resyncs; }

    void reset() noexcept;

private:
    binance::Venue venue_{binance::Venue::Spot};
    SyncState state_{SyncState::AwaitingSnapshot};
    uint32_t conn_epoch_{0};
    uint64_t last_update_id_{0};
    uint64_t prev_u_{0};
    bool first_diff_after_snapshot_{false};
    SyncCounters counters_{};
};

} // namespace lob
