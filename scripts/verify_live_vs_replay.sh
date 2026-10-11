#!/usr/bin/env bash
# ==============================================================================
# Binance LOB Capture: Live vs Replay Verification (Bash)
# ==============================================================================
set -euo pipefail

DURATION=${1:-120}
SYMBOL=${2:-"BTCUSDT"}
LIVE_DIR="verify_live_120s"
REPLAY_DIR="verify_replay_120s"

# Detect binary path
if [ -f "./build/binance_capture" ]; then
    EXE="./build/binance_capture"
elif [ -f "./build/Release/binance_capture" ]; then
    EXE="./build/Release/binance_capture"
elif [ -f "./build/Release/binance_capture.exe" ]; then
    EXE="./build/Release/binance_capture.exe"
else
    echo "[ERROR] binance_capture executable not found in ./build" >&2
    exit 1
fi

echo "============================================================"
echo "  BINANCE LOB VERIFICATION: LIVE vs REPLAY BIT-FOR-BIT TEST"
echo "============================================================"
echo "Duration:    ${DURATION}s"
echo "Symbol:      ${SYMBOL}"
echo "Live Dir:    ${LIVE_DIR}"
echo "Replay Dir:  ${REPLAY_DIR}"
echo "Executable:  ${EXE}"
echo "------------------------------------------------------------"

rm -rf "${LIVE_DIR}" "${REPLAY_DIR}"
mkdir -p "${LIVE_DIR}" "${REPLAY_DIR}"

echo ""
echo "[STEP 1/4] Starting Live WebSocket Capture (${DURATION}s)..."
"${EXE}" --symbols "${SYMBOL}" --duration "${DURATION}" --output-dir "${LIVE_DIR}"

LIVE_MD=$(find "${LIVE_DIR}" -name "market_data_*.csv" ! -name "*_orderbook.csv" | head -n 1)
LIVE_OB=$(find "${LIVE_DIR}" -name "*_orderbook.csv" | head -n 1)

if [ -z "${LIVE_MD}" ] || [ -z "${LIVE_OB}" ]; then
    echo "[ERROR] Live capture did not produce required CSV files." >&2
    exit 1
fi

echo "Live Capture Completed:"
echo "  Market Data: ${LIVE_MD}"
echo "  Orderbook:   ${LIVE_OB}"

echo ""
echo "[STEP 2/4] Running Offline Deterministic Replay (--bench)..."
"${EXE}" --replay "${LIVE_MD}" --output-dir "${REPLAY_DIR}" --bench

REPLAY_OB=$(find "${REPLAY_DIR}" -name "*_orderbook.csv" | head -n 1)
if [ -z "${REPLAY_OB}" ]; then
    echo "[ERROR] Replay did not produce orderbook CSV." >&2
    exit 1
fi

echo ""
echo "[STEP 3/4] Comparing SHA-256 Hashes of Live vs Replay Orderbooks..."
if command -v sha256sum >/dev/null 2>&1; then
    LIVE_HASH=$(sha256sum "${LIVE_OB}" | awk '{print $1}')
    REPLAY_HASH=$(sha256sum "${REPLAY_OB}" | awk '{print $1}')
else
    LIVE_HASH=$(shasum -a 256 "${LIVE_OB}" | awk '{print $1}')
    REPLAY_HASH=$(shasum -a 256 "${REPLAY_OB}" | awk '{print $1}')
fi

echo "  Live Orderbook Hash:   ${LIVE_HASH}"
echo "  Replay Orderbook Hash: ${REPLAY_HASH}"

if [ "${LIVE_HASH}" = "${REPLAY_HASH}" ]; then
    echo ""
    echo "[VERIFICATION PASSED] Hashes match bit-for-bit!"
    echo "Deterministic replay produces 100% identical byte-for-byte output."
else
    echo ""
    echo "[VERIFICATION FAILED] Hashes do NOT match!" >&2
    exit 1
fi

echo ""
echo "[STEP 4/4] Running Full CSV Schema and Market Invariant Sanity Checks..."
if command -v python3 >/dev/null 2>&1; then
    python3 scripts/check_csv_sanity.py --dir "${LIVE_DIR}"
elif command -v python >/dev/null 2>&1; then
    python scripts/check_csv_sanity.py --dir "${LIVE_DIR}"
fi

echo ""
echo "============================================================"
echo "  TASK 11 VERIFICATION COMPLETE: ALL CHECKS PASSED!"
echo "============================================================"
