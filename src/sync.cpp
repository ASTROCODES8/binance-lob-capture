#include "sync.hpp"

namespace lob {

SymbolSyncStateMachine::SymbolSyncStateMachine(binance::Venue venue) noexcept
    : venue_(venue) {
}

void SymbolSyncStateMachine::reset() noexcept {
    state_ = SyncState::AwaitingSnapshot;
    last_update_id_ = 0;
    prev_u_ = 0;
    first_diff_after_snapshot_ = false;
}

SyncAction SymbolSyncStateMachine::on_depth5(uint64_t last_update_id) noexcept {
    if (state_ != SyncState::Synced) {
        last_update_id_ = last_update_id;
        first_diff_after_snapshot_ = true;
        prev_u_ = last_update_id;
        state_ = SyncState::Synced;
        ++counters_.resyncs;
    } else if (first_diff_after_snapshot_) {
        last_update_id_ = last_update_id;
        prev_u_ = last_update_id;
    }
    return SyncAction::Apply;
}

SyncAction SymbolSyncStateMachine::on_diff(uint64_t U, uint64_t u, uint64_t pu) noexcept {
    if (state_ == SyncState::AwaitingSnapshot || state_ == SyncState::Invalid) {
        ++counters_.dropped;
        return SyncAction::Drop;
    }

    // State is Synced
    if (first_diff_after_snapshot_) {
        // Drop any diff that ended before or at the snapshot update ID
        if (u <= last_update_id_) {
            ++counters_.dropped;
            return SyncAction::Drop;
        }

        // First applied diff must bridge the snapshot: U <= last_update_id + 1 <= u
        if (U <= last_update_id_ + 1 && last_update_id_ + 1 <= u) {
            first_diff_after_snapshot_ = false;
            prev_u_ = u;
            return SyncAction::Apply;
        } else {
            // Sequence gap or out-of-order diff detected
            state_ = SyncState::Invalid;
            ++counters_.gaps;
            return SyncAction::Resync;
        }
    }

    // Optional USD-M futures hook: pu check
    if (venue_ == binance::Venue::Usdm) {
        if (pu != prev_u_) {
            state_ = SyncState::Invalid;
            ++counters_.gaps;
            return SyncAction::Resync;
        }
    } else {
        // Binance Spot continuity: strictly U == prev_u + 1
        if (U != prev_u_ + 1) {
            state_ = SyncState::Invalid;
            ++counters_.gaps;
            return SyncAction::Resync;
        }
    }

    prev_u_ = u;
    return SyncAction::Apply;
}

SyncAction SymbolSyncStateMachine::on_trade() noexcept {
    if (state_ == SyncState::Synced) {
        return SyncAction::Apply;
    }
    ++counters_.dropped;
    return SyncAction::Drop;
}

SyncAction SymbolSyncStateMachine::on_reconnect() noexcept {
    ++conn_epoch_;
    reset();
    return SyncAction::Resync;
}

SyncAction SymbolSyncStateMachine::on_ring_overflow() noexcept {
    state_ = SyncState::Invalid;
    ++counters_.ring_overflows;
    return SyncAction::Resync;
}

} // namespace lob
