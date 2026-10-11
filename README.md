# Binance WebSocket Capture & Local Order Book (LOB)

High-performance, low-latency C++17 market data capture and deterministic order book maintenance engine for Binance WebSocket streams.

---

## 1. Overview & Architecture

The application connects to Binance combined WebSocket market data streams, records an immutable market data audit log (`market_data_*.csv`), maintains a synchronized local order book (LOB), and emits top-5 order book snapshots (`*_orderbook.csv`) in chronological order.

### Threading & Concurrency Model
- **NET Thread**: Asynchronous I/O powered by Boost.Asio and Boost.Beast TLS. Reads frames from the socket, takes a wall-clock nanosecond timestamp immediately upon read completion, slices the JSON envelope (`"stream"` and `"data"`), and enqueues messages into a lock-free Single-Producer Single-Consumer (SPSC) ring buffer. Zero locks or file operations on this thread.
- **PROCESSOR Thread**: Single consumer thread owning the Local Order Book (LOB) state, sequence validators, and CSV writers. Pops raw records from the SPSC ring, parses payloads on-demand via `simdjson`, updates book levels, and streams output via 1 MB buffered file writers.
- **Offline Replay Mode (`--replay`)**: Reads an existing `market_data_*.csv` file via a dedicated zero-allocation RFC 4180 parser and feeds the exact same `Processor` instance. There is strictly **zero network code or socket interaction on this path**. Guarantees 100% deterministic, byte-identical order book output. Limits: maximum 20 symbols, maximum 64 KB message size.

---

### Command Line Interface (CLI)
```text
Usage: binance_capture [OPTIONS]

Options:
  --venue <spot|usdm>        Target exchange venue (default: spot)
  --symbols <sym1,sym2,...>  Comma-separated symbols (e.g. BTCUSDT, max 20)
  --output-dir <path>        Directory to store output CSV files (default: output)
  --duration <seconds>       Capture/replay duration in seconds (0 = unlimited)
  --replay <path>            Replay market data CSV without network access
  --bench                    Replay at maximum speed and output performance metrics
  --host <hostname>          WebSocket host override (default: data-stream.binance.vision)
  --help, -h                 Display help message
```

---

## 2. Design Trade-Offs

### Container Choice: `std::map` vs Flat Sorted Vector
In limit order book maintenance, selecting the underlying data structure involves balancing pointer overhead against cache locality:

1. **`std::map<int64_t, int64_t>` (Red-Black Tree)**:
   - **Complexity**: $O(\log N)$ point lookups, updates, insertions, and erasures. Top-5 level extraction is amortized $O(1)$ ($O(5)$ forward iteration from `.begin()`).
   - **Trade-offs**: Node-based structure introduces pointer overhead (24–32 bytes per node for parent/child pointers and color bits) and potential heap fragmentation. However, inserting or erasing price levels never invalidates unaffected node iterators or incurs memory shift penalties.
2. **Flat Sorted Vector (`std::vector<PriceLevel>`)**:
   - **Complexity**: Lookups are $O(\log N)$ via binary search (`std::lower_bound`), and top-5 extraction is contiguous $O(1)$. However, inserting a new level or removing an exhausted level requires an $O(N)$ memory move (`memmove`).
   - **Trade-offs**: Optimal CPU cache-line prefetching, but memory move costs grow significantly with deep books ($N > 2000$).

**Design Choice**: For Binance Spot 100ms differential streams, updates contain modest level counts (10–50 updates per event). `std::map` provides deterministic $O(\log N)$ delta modifications and clean top-5 re-anchoring via `replace_top5_from_depth5` without $O(N)$ shift penalties.

### Implemented Low-Latency Optimizations
1. **Thread-Local Civil Date Caching (`src/csv.cpp`)**:
   - Successive ticks on the same day ($tsec / 86400 == \text{cached\_day}$) bypass civil date division entirely.
   - Formats date string once per day and performs a single 128-bit vector copy for all subsequent rows, saving ~15 arithmetic cycles per message.
2. **UTF-8 BOM Stripper Pre-flight (`src/envelope.cpp`)**:
   - Inspects the first 3 bytes for `\xEF\xBB\xBF` and advances the `std::string_view` prefix in $O(1)$ time, guaranteeing compatibility with files saved by Windows PowerShell / Notepad.
3. **Fast Stream Kind Character Dispatch (`src/envelope.cpp`)**:
   - Uses initial-character branch partitioning (`'t'` for trade, `'d'` for depth/depth5) rather than multiple sequential string comparisons.
4. **Single Reused `simdjson` Parser (`src/lob.cpp`)**:
   - Single reused `simdjson::dom::parser` with pre-allocated 64 KB padded buffer eliminates per-message heap allocation and internal tape rebuilding.

---

## 3. Policies & Specifications

### Timestamp Policy
- Wall-clock time is captured via `std::chrono::system_clock::now()` immediately after the WebSocket frame read completes.
- Recorded as `recv_tsec` (int64 seconds since Unix epoch) and `recv_tnsec` (int32 nanosecond remainder in `[0, 999,999,999]`).
- Both market data audit rows and order book snapshot rows share this exact receive timestamp.
- UTC dates (`YYYY-MM-DD`) are computed purely via integer civil calendar arithmetic (Howard Hinnant's algorithm), with zero calls to mutex-locked `gmtime`/`localtime`.

### Fixed-Point Integer Scaling Policy
- All prices and quantities are scaled by **$10^8$ ($100,000,000$ ticks per unit)**.
- Parsed directly from decimal strings into 64-bit integers using hand-crafted integer accumulation with zero binary floating-point (`double`/`float`) arithmetic, avoiding IEEE 754 precision drift.
- Digits beyond 8 fractional places are truncated to conform to discrete discrete Bitcoin tick quanta (1 satoshi).

### Reconnect & Gap Handling
- **Startup**: Differential updates (`depthUpdate`) are dropped until the first partial top-of-book snapshot (`depth5`) arrives to anchor the book.
- **Top-5 Re-anchoring**: Upon receiving `depth5`, all levels at-or-better than the 5th (worst) rung are purged, and the 5 new bid/ask levels are inserted.
- **Sequence Continuity**: For Binance Spot diff updates, continuity strictly requires $U = \text{prev\_u} + 1$.
- **Invalidation**: If a sequence gap is detected, a crossed book condition occurs ($P_{\text{best\_bid}} \ge P_{\text{best\_ask}}$), or the SPSC ring buffer overflows, the book is invalidated, cleared, and waits for the next `depth5` to re-anchor.
- **Reconnect**: On WebSocket reconnect, `conn_epoch` increments, `conn_seq` resets to 0, and the order book is purged.

---

## 4. CSV Schema Contracts

### Deliverable A: Market Data Dump (`market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv`)
```csv
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

### Deliverable B: Order Book Snapshots (`market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>_orderbook.csv`)
Exactly 26 columns per row (25 commas):
```csv
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

---

## 5. Build & Dependencies

### Prerequisites
- **Linux (Ubuntu 22.04)**: `cmake ninja-build g++-12 libssl-dev libboost-all-dev`
- **Windows**: Visual Studio 2022 (MSVC 19.44+), CMake 3.16+, vcpkg

### Reproducible Linux Build
```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -O2"
cmake --build build --parallel
```

### Windows Build (MSVC)
```powershell
cmake -B build
cmake --build build --config Release
```

### Running Tests
```powershell
.\build\Release\unit_tests.exe
```

---

## 6. How to Submit on GitHub

```bash
git init
git add .
git commit -m "Initial submission: Binance WebSocket capture + LOB"
git remote add origin https://github.com/<your-username>/binance-lob-capture.git
git branch -M main
git push -u origin main
git tag -a v1.0.0 -m "Submission v1.0.0"
git push origin v1.0.0
```
