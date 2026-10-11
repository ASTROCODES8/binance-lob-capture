#include "processor.hpp"
#include <chrono>

namespace lob {

Processor::Processor(std::string output_dir,
                     std::vector<std::string> symbols,
                     binance::Venue venue,
                     bool is_replay)
    : output_dir_(std::move(output_dir))
    , configured_symbols_(std::move(symbols))
    , venue_(venue)
    , is_replay_(is_replay) {
    line_buffer_.reserve(8192);

    for (size_t i = 0; i < configured_symbols_.size(); ++i) {
        SymbolContext ctx;
        ctx.id = static_cast<int32_t>(i + 1); // 1-based index per locked decisions
        ctx.symbol_name = configured_symbols_[i];
        ctx.sync_sm = SymbolSyncStateMachine(venue_);
        symbol_contexts_.push_back(std::move(ctx));
    }
}

Processor::~Processor() {
    close();
}

SymbolContext* Processor::find_symbol(std::string_view sym) noexcept {
    for (auto& ctx : symbol_contexts_) {
        if (ctx.symbol_name == sym) {
            return &ctx;
        }
    }
    return nullptr;
}

void Processor::init_files_for_symbol(SymbolContext& ctx, int64_t epoch_sec) {
    if (!is_replay_) {
        std::string md_path = build_csv_filename(output_dir_, venue_to_string(venue_), ctx.symbol_name, epoch_sec, false);
        if (ctx.md_file.open(md_path)) {
            ctx.md_file.write(MARKET_DATA_HEADER);
        }
    }
    std::string ob_path = build_csv_filename(output_dir_, venue_to_string(venue_), ctx.symbol_name, epoch_sec, true);
    if (ctx.ob_file.open(ob_path)) {
        ctx.ob_file.write(ORDERBOOK_HEADER);
    }
    ctx.files_initialized = true;
}

void Processor::on_reconnect(uint32_t new_epoch) {
    metrics_.reconnects.fetch_add(1, std::memory_order_relaxed);
    for (auto& ctx : symbol_contexts_) {
        ctx.sync_sm.on_reconnect();
        ctx.book.clear();
    }
    (void)new_epoch;
}

void Processor::on_ring_overflow() {
    metrics_.ring_drops.fetch_add(1, std::memory_order_relaxed);
    for (auto& ctx : symbol_contexts_) {
        ctx.sync_sm.on_ring_overflow();
        ctx.book.clear();
    }
}

void Processor::on_message(std::string_view raw,
                           int64_t recv_ns,
                           uint32_t shard,
                           uint32_t epoch,
                           uint64_t seq) {
    const auto start_proc = std::chrono::steady_clock::now();
    struct LatencyGuard {
        LatencyHistogram& hist;
        std::chrono::steady_clock::time_point start;
        ~LatencyGuard() {
            const auto end = std::chrono::steady_clock::now();
            uint64_t dur_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            hist.record(dur_ns);
        }
    } latency_guard{metrics_.latency, start_proc};

    int64_t tsec = recv_ns / 1'000'000'000LL;
    int32_t tnsec = static_cast<int32_t>(recv_ns % 1'000'000'000LL);
    if (tnsec < 0) {
        tnsec += 1'000'000'000;
        tsec -= 1;
    }

    // 1. Zero-allocation envelope slice
    EnvelopeResult env = parse_envelope(raw);
    if (!env.ok()) {
        metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    metrics_.msgs_processed.fetch_add(1, std::memory_order_relaxed);

    SymbolContext* ctx_ptr = find_symbol(env.symbol_view());
    if (ctx_ptr == nullptr) {
        return; // Unconfigured symbol
    }
    SymbolContext& ctx = *ctx_ptr;

    if (!ctx.files_initialized) {
        init_files_for_symbol(ctx, tsec);
    }

    // 2. Write Deliverable A (Market Data Dump) row
    line_buffer_.clear();
    append_int(line_buffer_, tsec); line_buffer_.push_back(',');
    append_int(line_buffer_, tnsec); line_buffer_.push_back(',');
    line_buffer_.append(venue_to_string(venue_)); line_buffer_.push_back(',');
    line_buffer_.append(stream_kind_to_string(env.stream_kind)); line_buffer_.push_back(',');
    append_int(line_buffer_, shard); line_buffer_.push_back(',');
    append_int(line_buffer_, epoch); line_buffer_.push_back(',');
    append_int(line_buffer_, seq); line_buffer_.push_back(',');
    line_buffer_.append(ctx.symbol_name); line_buffer_.push_back(',');
    escape_rfc4180(env.data_slice, line_buffer_);
    line_buffer_.push_back('\n');

    ctx.md_file.write(line_buffer_);
    ctx.md_file.increment_rows();
    metrics_.rows_written.fetch_add(1, std::memory_order_relaxed);

    // 3. Parse data payload on-demand & update sync state / LOB
    process_payload(ctx, tsec, tnsec, env.stream_kind, env.data_slice);
}

void Processor::on_replay_message(int64_t recv_ns,
                                  uint32_t shard,
                                  uint32_t epoch,
                                  uint64_t seq,
                                  std::string_view symbol,
                                  binance::StreamKind stream_kind,
                                  std::string_view payload_json) {
    const auto start_proc = std::chrono::steady_clock::now();
    struct LatencyGuard {
        LatencyHistogram& hist;
        std::chrono::steady_clock::time_point start;
        ~LatencyGuard() {
            const auto end = std::chrono::steady_clock::now();
            uint64_t dur_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            hist.record(dur_ns);
        }
    } latency_guard{metrics_.latency, start_proc};

    int64_t tsec = recv_ns / 1'000'000'000LL;
    int32_t tnsec = static_cast<int32_t>(recv_ns % 1'000'000'000LL);
    if (tnsec < 0) {
        tnsec += 1'000'000'000;
        tsec -= 1;
    }
    metrics_.msgs_processed.fetch_add(1, std::memory_order_relaxed);

    SymbolContext* ctx_ptr = find_symbol(symbol);
    if (ctx_ptr == nullptr) {
        return; // Unconfigured symbol
    }
    SymbolContext& ctx = *ctx_ptr;

    if (!ctx.files_initialized) {
        init_files_for_symbol(ctx, tsec);
    }

    process_payload(ctx, tsec, tnsec, stream_kind, payload_json);

    (void)shard;
    (void)epoch;
    (void)seq;
}

void Processor::process_payload(SymbolContext& ctx,
                                int64_t tsec,
                                int32_t tnsec,
                                binance::StreamKind stream_kind,
                                std::string_view data_slice) {
    if (stream_kind == binance::StreamKind::DepthDiff) {
        if (!json_parser_.parse_diff(data_slice, diff_buf_)) {
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        SyncAction action = ctx.sync_sm.on_diff(diff_buf_.U, diff_buf_.u);
        if (action == SyncAction::Drop) {
            return;
        }
        if (action == SyncAction::Resync) {
            ctx.book.clear();
            metrics_.gaps.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Action == Apply
        bool touched_bids = !diff_buf_.bids.empty();
        bool touched_asks = !diff_buf_.asks.empty();
        char side = 'N';
        if (touched_bids && !touched_asks) side = 'B';
        else if (!touched_bids && touched_asks) side = 'S';

        ctx.book.apply_diff(diff_buf_.bids, diff_buf_.asks);

        if (ctx.book.crossed()) {
            ctx.book.clear();
            ctx.sync_sm.reset();
            metrics_.gaps.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        write_orderbook_row(ctx, tsec, tnsec, 'D', side);

    } else if (stream_kind == binance::StreamKind::Depth5) {
        if (!json_parser_.parse_depth5(data_slice, d5_buf_)) {
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ctx.sync_sm.on_depth5(d5_buf_.last_update_id);
        ctx.book.replace_top5_from_depth5(d5_buf_.bids, d5_buf_.asks);

        write_orderbook_row(ctx, tsec, tnsec, 'S', 'N');

    } else if (stream_kind == binance::StreamKind::Trade) {
        if (!json_parser_.parse_trade(data_slice, trade_buf_)) {
            metrics_.parse_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        SyncAction action = ctx.sync_sm.on_trade();
        if (action == SyncAction::Drop) {
            return;
        }

        // Aggressor side: m=true (buyer maker) -> seller aggressor ('S'), else 'B'
        char side = trade_buf_.is_buyer_maker ? 'S' : 'B';
        write_orderbook_row(ctx, tsec, tnsec, 'T', side);
    }
}

void Processor::write_orderbook_row(SymbolContext& ctx,
                                    int64_t tsec,
                                    int32_t tnsec,
                                    char type,
                                    char side) {
    ctx.ob_seq_no++;
    BookSnapshot snap = ctx.book.top5();

    line_buffer_.clear();
    append_int(line_buffer_, tsec); line_buffer_.push_back(',');
    append_int(line_buffer_, tnsec); line_buffer_.push_back(',');
    append_int(line_buffer_, ctx.ob_seq_no); line_buffer_.push_back(',');
    append_int(line_buffer_, ctx.id); line_buffer_.push_back(',');
    line_buffer_.push_back(type); line_buffer_.push_back(',');
    line_buffer_.push_back(side); line_buffer_.push_back(',');

    // Top 5 bid prices
    for (size_t i = 0; i < 5; ++i) {
        append_int(line_buffer_, snap.bid_prices[i]);
        line_buffer_.push_back(',');
    }

    // Top 5 bid sizes
    for (size_t i = 0; i < 5; ++i) {
        append_int(line_buffer_, snap.bid_sizes[i]);
        line_buffer_.push_back(',');
    }

    // Top 5 ask prices
    for (size_t i = 0; i < 5; ++i) {
        append_int(line_buffer_, snap.ask_prices[i]);
        line_buffer_.push_back(',');
    }

    // Top 5 ask sizes
    for (size_t i = 0; i < 5; ++i) {
        append_int(line_buffer_, snap.ask_sizes[i]);
        if (i < 4) {
            line_buffer_.push_back(',');
        }
    }
    line_buffer_.push_back('\n');

    ctx.ob_file.write(line_buffer_);
    ctx.ob_file.increment_rows();
    metrics_.rows_written.fetch_add(1, std::memory_order_relaxed);
}

void Processor::flush() {
    for (auto& ctx : symbol_contexts_) {
        ctx.md_file.flush();
        ctx.ob_file.flush();
    }
}

void Processor::close() {
    flush();
    for (auto& ctx : symbol_contexts_) {
        ctx.md_file.close();
        ctx.ob_file.close();
    }
}

} // namespace lob
