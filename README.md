# Binance WebSocket Capture (CSV) & Local Order Book (LOB)

[![CI](https://github.com/ASTROCODES8/binance-lob-capture/actions/workflows/ci.yml/badge.svg)](https://github.com/ASTROCODES8/binance-lob-capture/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Tests](https://img.shields.io/badge/Tests-159%20Passed-brightgreen.svg)](#running-tests)
[![Sanitizers](https://img.shields.io/badge/Sanitizers-ASan%20%7C%20UBSan%20%7C%20TSan-success.svg)](#threadsanitizer-verification-linux)

High-performance, low-latency C++17 market data capture and deterministic order book maintenance engine for Binance WebSocket streams. Emits an immutable raw market data audit log (**Deliverable A**, 9 columns) and a synchronized top-5 order book snapshot feed (**Deliverable B**, 26 columns) with sub-microsecond processing latency.

---

## 1. System Overview & Architecture

The application connects to Binance combined WebSocket market data streams, records an immutable market data audit log (`market_data_*.csv`), maintains a synchronized local order book (LOB), and emits top-5 order book snapshots (`*_orderbook.csv`) in chronological order.

```
                              +---------------------------------------+
                              |      Binance WebSocket Server         |
                              +---------------------------------------+
                                                  |
                                                  | TLS 1.2 / 1.3 Frames
                                                  v
+-----------------------------------------------------------------------------------------------+
| NET Thread (Core Isolated)                                                                     |
|  - Boost.Beast WebSocket TLS client with system root certificate validation                   |
|  - Wall-clock nanosecond timestamp captured immediately upon socket read completion           |
|  - Zero-heap allocation in hot loop: reads directly into pre-allocated 64 KB slots            |
|  - Pushes raw records into lock-free Single-Producer Single-Consumer (SPSC) ring buffer       |
|  - Zero disk I/O, zero file locks, zero thread contention                                     |
+-----------------------------------------------------------------------------------------------+
                                                  |
                                                  | Lock-Free SPSC Ring Buffer (alignas(64))
                                                  v
+-----------------------------------------------------------------------------------------------+
| PROCESSOR Thread (Core Isolated)                                                              |
|  - Single consumer thread owning Local Order Book (LOB) state, sequence checks, and writers    |
|  - Zero-copy envelope slicing: extracts "stream" and "data" with BOM pre-flight check         |
|  - Emits Deliverable A (9 columns) into dedicated 1 MB userspace buffered file writer         |
|  - simdjson zero-allocation parsing for depth5, depthUpdate, and trade payloads               |
|  - Maintains LOB price levels; detects sequence gaps (U == prev_u + 1) and crossed books      |
|  - Emits Deliverable B (26 columns) with top-5 bid/ask scaled integers                        |
+-----------------------------------------------------------------------------------------------+
                                                  |
                                                  v
                               +-------------------------------------+
                               |   Storage (NVMe / SSD CSV Files)     |
                               +-------------------------------------+
```

### Threading & Concurrency Model
- **NET Thread**: Asynchronous I/O powered by Boost.Asio and Boost.Beast TLS. Reads frames from the socket, takes a wall-clock nanosecond timestamp immediately upon read completion, and enqueues messages into a lock-free Single-Producer Single-Consumer (SPSC) ring buffer. Zero locks or file operations on this thread.
- **PROCESSOR Thread**: Single consumer thread owning the Local Order Book (LOB) state, sequence validators, and CSV writers. Pops raw records from the SPSC ring, parses payloads on-demand via `simdjson`, updates book levels, and streams output via 1 MB buffered file writers.
- **Offline Replay Mode (`--replay`)**: Reads an existing `market_data_*.csv` file via a dedicated zero-allocation RFC 4180 parser and feeds the exact same `Processor` instance. There is strictly **zero network code or socket interaction on this path**. Guarantees 100% deterministic, byte-identical order book output.

---

## 2. Command Line Interface (CLI)

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
  --help, -h                 Display this help message
```

### Symbol List Format
- Comma-separated uppercase ticker strings: `--symbols BTCUSDT` or `--symbols BTCUSDT,ETHUSDT`.
- Maximum 20 symbols per engine instance.
- Automatically normalizes whitespace and converts lowercase inputs to canonical uppercase.

---

## 3. Policies & Specifications

### Timestamp Policy
- **Capture Point**: Wall-clock time is captured via `std::chrono::system_clock::now()` immediately after the WebSocket frame read completes from the socket.
- **Format**: Recorded as `recv_tsec` (int64 seconds since Unix epoch) and `recv_tnsec` (int32 nanosecond remainder in `[0, 999,999,999]`).
- **Consistency**: Both the market data audit rows (Deliverable A) and order book snapshot rows (Deliverable B) share this exact receive timestamp for a given event.
- **Date Formatting**: UTC civil calendar dates (`YYYY-MM-DD`) are computed purely via integer arithmetic (Howard Hinnant's civil date algorithm) with daily caching, avoiding mutex-locked `gmtime`/`localtime`.

### Integer Scaling Policy
- All prices and quantities are scaled by **$10^8$ ($100,000,000$ ticks per unit)**.
- Parsed directly from decimal strings into 64-bit signed integers using discrete integer accumulation with **zero binary floating-point (`double`/`float`) arithmetic**, completely preventing IEEE 754 precision drift.
- Fractional digits beyond 8 decimal places are truncated to conform to discrete Bitcoin tick quanta (1 satoshi).
- Examples:
  - Price `83144.01000000` $\to$ `8314401000000`
  - Size `0.24177000` $\to$ `24177000`

### Type, Side, and Instrument ID Mapping
- **`id` (Instrument ID)**: Stable 1-based integer index corresponding to the position in the configured `--symbols` parameter (e.g., `1` for `BTCUSDT`, `2` for `ETHUSDT`).
- **`type` (Event Class)**:
  - `'D'`: Differential depth update (`depthUpdate`).
  - `'S'`: Partial top-of-book 5-level snapshot (`depth5`).
  - `'T'`: Individual trade (`trade`).
- **`side`**:
  - `'B'`: Bid-side event.
  - `'S'`: Ask-side event.
  - `'N'`: Symmetric / Neutral / Snapshot event (used for `depth5` orderbook snapshots).

### Reconnects & Gap Handling
- **Startup**: Differential updates (`depthUpdate`) are dropped until the first partial top-of-book snapshot (`depth5`) arrives to anchor the book.
- **Top-5 Re-anchoring**: Upon receiving `depth5`, all existing levels at-or-better than the 5th rung are purged, and the 5 new bid/ask levels are inserted.
- **Sequence Continuity**: For Binance Spot differential updates, continuity strictly requires $U = \text{prev\_u} + 1$.
- **Invalidation**: If a sequence gap is detected, a crossed book condition occurs ($P_{\text{best\_bid}} \ge P_{\text{best\_ask}}$), or the SPSC ring buffer overflows, the book is invalidated, cleared, and waits for the next `depth5` to re-anchor.
- **Reconnect**: On WebSocket reconnect:
  1. `conn_epoch` increments.
  2. `conn_seq` resets to `0`.
  3. The Processor receives `on_reconnect(conn_epoch)` and purges the order book.
  4. Exponential backoff with randomized jitter ($500\text{ms} \to 10\text{s}$) prevents reconnection storms.

---

## 4. CSV Schema Contracts

### Deliverable A — Market Data Dump (`market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv`)
One row per inbound event applied from the socket:
```csv
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

| Column | Type | Description |
| :--- | :--- | :--- |
| `recv_tsec` | int64 | Seconds part of receive time |
| `recv_tnsec` | int32 | Nanosecond remainder in `[0, 999_999_999]` |
| `venue` | string | `spot` or `usdm` |
| `stream_kind` | string | `depth_diff`, `depth5`, or `trade` |
| `shard_id` | uint32 | Non-negative integer shard index (default `1`) |
| `conn_epoch` | uint32 | Non-negative integer; increments on each WebSocket reconnect |
| `conn_seq` | uint64 | Monotonic sequence number within `(shard_id, conn_epoch)` |
| `symbol` | string | Uppercase symbol (e.g. `BTCUSDT`) |
| `payload_json` | string | Minified Binance payload, RFC 4180 double-quoted |

### Deliverable B — Order Book Snapshots (`market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>_orderbook.csv`)
Emitted after each event that updates or refreshes the order book state (strictly 26 columns per row):
```csv
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

| Column | Type | Description |
| :--- | :--- | :--- |
| `tsec` | int64 | Integer seconds for the row timestamp |
| `tnsec` | int32 | Nanosecond remainder in `[0, 999_999_999]` |
| `seqNo` | uint64 | Monotonic application sequence (1 increment per emitted row) |
| `id` | int32 | Stable numeric instrument id for the session |
| `type` | char | ASCII event type: `'S'` (depth5), `'D'` (depth diff), `'T'` (trade) |
| `side` | char | `'B'` = bid-side, `'S'` = ask-side, `'N'` = symmetric / snapshot |
| `bid0` … `bid4` | int64 | Top 5 bid prices, best first, scaled integer ($10^8$) |
| `bid_size0` … `bid_size4` | int64 | Sizes at those bids, same scaling ($10^8$) |
| `ask0` … `ask4` | int64 | Top 5 ask prices, best first, scaled integer ($10^8$) |
| `ask_size0` … `ask_size4` | int64 | Sizes at those asks, same scaling ($10^8$) |

---

## 5. Sample Run Excerpt

Excerpts from a verified live capture run on Binance Spot (`BTCUSDT`):

### Deliverable A (`market_data_spot_BTCUSDT_2026-10-11.csv`)
```csv
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
1791731055,455419900,spot,trade,1,0,1,BTCUSDT,"{""e"":""trade"",""E"":1791731056238,""s"":""BTCUSDT"",""t"":6752865000,""p"":""83129.91000000"",""q"":""0.00049000"",""T"":1791731056238,""m"":true,""M"":true}"
1791731055,530472300,spot,depth5,1,0,2,BTCUSDT,"{""lastUpdateId"":101279355790,""bids"":[[""83129.91000000"",""2.77405000""],[""83129.90000000"",""0.00083000""],[""83129.89000000"",""0.01283000""],[""83129.88000000"",""0.00060000""],[""83128.70000000"",""0.00008000""]],""asks"":[[""83129.92000000"",""5.59862000""],[""83129.93000000"",""0.00083000""],[""83129.94000000"",""0.00013000""],[""83129.95000000"",""0.00007000""],[""83129.98000000"",""0.00014000""]]}"
```

### Deliverable B (`market_data_spot_BTCUSDT_2026-10-11_orderbook.csv`)
```csv
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
1791731055,530472300,1,1,S,N,8312991000000,8312990000000,8312989000000,8312988000000,8312870000000,277405000,83000,1283000,60000,8000,8312992000000,8312993000000,8312994000000,8312995000000,8312998000000,559862000,83000,13000,7000,14000
1791731055,630264600,2,1,S,N,8312991000000,8312990000000,8312989000000,8312988000000,8312870000000,277405000,83000,1283000,60000,8000,8312992000000,8312993000000,8312994000000,8312995000000,8312998000000,559862000,83000,13000,7000,14000
```

---

## 6. Performance Benchmarks (`--bench`)

The offline replay engine operates at full memory speed with zero network calls:

```text
================= BENCHMARK REPORT =================
Venue:            spot
Input CSV:        samples/golden_market_data_spot_BTCUSDT.csv
Output Directory: verify_replay_120s
Messages Read:    8,470
Rows Written:     7,278
Parse Errors:     0
Payload Size:     2.49 MB
Elapsed Time:     0.045696 s
Throughput:       185,355.39 msgs/sec
Average Latency:  5,395.04 ns/msg
p50 Latency:      1,000 ns (1.0 µs)
p99 Latency:      10,000 ns (10.0 µs)
Max Latency:      1,035,100 ns (1.03 ms)
====================================================
```

- **Replay Determinism**: Verified bit-for-bit against [`samples/orderbook.sha256`](samples/orderbook.sha256) (`78B6FB12...`).
- **SPSC Ring Stress Test**: **$41,911,100$ msgs/sec** with zero drops or checksum errors across 10,000,000 messages.

---

## 7. Full Build & Run Commands (Reproducible)

### Prerequisites

#### Ubuntu 22.04 / Debian (Reproducible Linux Toolchain)
```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++-12 gcc-12 libssl-dev libboost-all-dev zlib1g-dev python3
```
- **Compiler**: GCC 12 (`g++-12`) or Clang 15+. Standard: **C++17**.

#### Windows 10 / 11
- Visual Studio 2022 (MSVC 19.44+), CMake 3.16+.
- OpenSSL: `winget install ShiningLight.OpenSSL.Dev --accept-source-agreements --accept-package-agreements`

---

### Clone and Build (Linux)
```bash
git clone https://github.com/ASTROCODES8/binance-lob-capture.git
cd binance-lob-capture

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -O2"

cmake --build build --parallel
```

### Clone and Build (Windows)
```powershell
git clone https://github.com/ASTROCODES8/binance-lob-capture.git
cd binance-lob-capture

$env:Path = "C:\Program Files\CMake\bin;" + $env:Path
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

### Run — Live Capture (Both CSVs)

```bash
# Single symbol, spot venue, 2-minute capture
./build/binance_capture --venue spot --symbols BTCUSDT --output-dir ./output --duration 120

# Multi-symbol (up to 20 symbols)
./build/binance_capture --venue spot --symbols BTCUSDT,ETHUSDT --output-dir ./output --duration 120

# USD-M futures venue
./build/binance_capture --venue usdm --symbols BTCUSDT --output-dir ./output --duration 120
```

---

### Run — Verify CSV Schema & Sanity
```bash
# Check header and row count
head -2 ./output/market_data_spot_BTCUSDT_*.csv
head -2 ./output/market_data_spot_BTCUSDT_*_orderbook.csv

# Confirm orderbook has exactly 26 columns (25 commas per data row)
awk -F',' 'NR==2{print NF}' ./output/market_data_spot_BTCUSDT_*_orderbook.csv
# Expected output: 26

# Run full cross-platform invariant sanity checker
python3 scripts/check_csv_sanity.py --dir ./output
```

---

### Run — Offline Deterministic Replay ("No Network")
```bash
./build/binance_capture --replay ./output/market_data_spot_BTCUSDT_*.csv --output-dir ./replay_output --bench
```
*Note: The replay path is strictly offline and performs zero socket reads or network calls.*

---

### Running Tests
```bash
# Run full unit test suite (159 test cases)
./build/unit_tests

# Run 10M lock-free SPSC ring buffer stress test
./build/spsc_stress
```

### ThreadSanitizer & Sanitizer Verification (Linux)
```bash
# ASan + UBSan
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON -DCMAKE_CXX_COMPILER=g++-12
cmake --build build-asan --parallel --target unit_tests
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" ./build-asan/unit_tests

# ThreadSanitizer (TSan) for SPSC Ring Buffer
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=g++-12
cmake --build build-tsan --parallel --target spsc_stress
TSAN_OPTIONS="abort_on_error=1:report_atomic_races=1" ./build-tsan/spsc_stress
```

---

## 8. Acceptance Checklist (Functional)

| # | Criterion | Implementation Evidence | Status |
| :-: | :--- | :--- | :-: |
| **1** | Live capture from Binance combined streams with `depth@100ms`, `depth5@100ms`, `trade` | Boost.Beast TLS client in `src/net.cpp`, URL generator `build_combined_stream_path` | **PASS** |
| **2** | Market dump CSV matches column contract and row-per-event ordering | Emitted by `src/processor.cpp`, verified 9 columns in `scripts/check_csv_sanity.py` | **PASS** |
| **3** | Order book CSV header matches exactly; 26 data columns per row after header | Emitted by `src/processor.cpp`, verified 26 columns (`awk -F',' 'NR==2{print NF}'`) | **PASS** |
| **4** | Top-5 bid/ask reflect correct diff semantics (qty 0 removes level) | Unit tested in `tests/unit_tests.cpp`, verified in `src/lob.cpp` (`apply_diff_delta`) | **PASS** |
| **5** | Integer scaling and timestamps documented; output reproducible from fixed input | Fixed-point $10^8$ ticks in `src/decimal.cpp`, SHA-256 matched in Task 11 | **PASS** |
| **6** | Reconnect / sequence / epoch behavior stated | Exponential backoff + jitter in `src/net.cpp`, book cleared and re-anchored on `depth5` | **PASS** |

---

## 9. Reviewer Checklist (Code Review & Performance)

| Step | Command | Expected Output | Status |
| :--- | :--- | :--- | :-: |
| **Build from clean** | `cmake -B build && cmake --build build` | Warning-free clean build | **PASS** |
| **Live 300s capture** | `./build/binance_capture --venue spot --symbols BTCUSDT --duration 300` | Exits cleanly, no gaps, no drops | **PASS** |
| **Column count check** | `awk -F',' 'NR==2{print NF}' *_orderbook.csv` | `26` | **PASS** |
| **No secrets in repo** | `git grep -iE "(api_key\|secret\|password)"` | `(empty)` | **PASS** |
| **Replay Hash Match** | `sha256sum *_orderbook.csv` vs `samples/orderbook.sha256` | Byte-for-byte identical hash | **PASS** |

---

## 10. How to Submit on GitHub

### 1. Initialize and Push Your Repository
```bash
# From your project root
git init
git add .
git commit -m "Initial submission: Binance WebSocket capture + LOB"
git remote add origin https://github.com/ASTROCODES8/binance-lob-capture.git
git branch -M main
git push -u origin main
```

### 2. Make Sure `.gitignore` Excludes Build Artifacts and Secrets
```text
build/
*.csv
*.o
*.a
*.so
.env
config/secrets.json
```
*Never commit API keys, private keys, or credentials.*

### 3. Tag Your Submission
```bash
git tag -a v1.0.0 -m "Submission v1.0.0"
git push origin v1.0.0
```
