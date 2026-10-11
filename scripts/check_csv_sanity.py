#!/usr/bin/env python3
"""
CSV Sanity Checker for Binance LOB Capture Engine
Validates Deliverable A (market data CSV) and Deliverable B (orderbook CSV)
against strict schema and market invariants:
- Exact header format
- Exactly 26 columns per row in Deliverable B, 9 columns in Deliverable A
- Strict bid descending order: bid0 > bid1 > bid2 > bid3 > bid4
- Strict ask ascending order: ask0 < ask1 < ask2 < ask3 < ask4
- Positive spread (no crossed book): bid0 < ask0
- Monotonic seqNo in Deliverable B
- Monotonic conn_seq per epoch in Deliverable A
"""

import sys
import os
import csv
import json
import argparse

EXPECTED_MARKET_DATA_HEADER = [
    "recv_tsec", "recv_tnsec", "venue", "stream_kind",
    "shard_id", "conn_epoch", "conn_seq", "symbol", "payload_json"
]

EXPECTED_ORDERBOOK_HEADER = [
    "tsec", "tnsec", "seqNo", "id", "type", "side",
    "bid0", "bid1", "bid2", "bid3", "bid4",
    "bid_size0", "bid_size1", "bid_size2", "bid_size3", "bid_size4",
    "ask0", "ask1", "ask2", "ask3", "ask4",
    "ask_size0", "ask_size1", "ask_size2", "ask_size3", "ask_size4"
]

def check_market_data(filepath):
    print(f"[SANITY] Checking Market Data CSV (Deliverable A): {filepath}")
    errors = 0
    warnings = 0
    row_count = 0

    if not os.path.exists(filepath):
        print(f"  [ERROR] File does not exist: {filepath}")
        return False

    with open(filepath, 'r', newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            print("  [ERROR] File is empty")
            return False

        if header != EXPECTED_MARKET_DATA_HEADER:
            print(f"  [ERROR] Header mismatch:\n    Got:      {header}\n    Expected: {EXPECTED_MARKET_DATA_HEADER}")
            errors += 1
        else:
            print("  [PASS] Header matches exact 9-column specification.")

        last_epoch = None
        last_seq = None

        for line_num, row in enumerate(reader, start=2):
            row_count += 1
            if len(row) != 9:
                print(f"  [ERROR] Line {line_num}: Expected 9 columns, got {len(row)}")
                errors += 1
                if errors > 10: break
                continue

            try:
                tsec = int(row[0])
                tnsec = int(row[1])
                epoch = int(row[5])
                seq = int(row[6])
                payload_str = row[8]
            except ValueError as e:
                print(f"  [ERROR] Line {line_num}: Integer conversion failed: {e}")
                errors += 1
                if errors > 10: break
                continue

            if not (0 <= tnsec <= 999999999):
                print(f"  [ERROR] Line {line_num}: tnsec out of range [0, 999999999]: {tnsec}")
                errors += 1

            if last_epoch is None or epoch != last_epoch:
                last_epoch = epoch
                last_seq = seq
            else:
                if seq != last_seq + 1:
                    print(f"  [ERROR] Line {line_num}: Non-monotonic conn_seq in epoch {epoch}: prev={last_seq}, curr={seq}")
                    errors += 1
                last_seq = seq

            # Fast JSON sanity check
            if not payload_str.startswith("{") or not payload_str.endswith("}"):
                print(f"  [ERROR] Line {line_num}: Invalid JSON payload enclosing braces")
                errors += 1

            if errors > 20:
                print("  [ABORT] Too many errors encountered.")
                break

    print(f"  Total rows inspected: {row_count}")
    if errors == 0:
        print("  [RESULT] Deliverable A PASSED all sanity checks.")
        return True
    else:
        print(f"  [RESULT] Deliverable A FAILED with {errors} errors.")
        return False


def check_orderbook(filepath):
    print(f"[SANITY] Checking Orderbook CSV (Deliverable B): {filepath}")
    errors = 0
    warnings = 0
    row_count = 0

    if not os.path.exists(filepath):
        print(f"  [ERROR] File does not exist: {filepath}")
        return False

    with open(filepath, 'r', newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            print("  [ERROR] File is empty")
            return False

        if header != EXPECTED_ORDERBOOK_HEADER:
            print(f"  [ERROR] Header mismatch:\n    Got:      {header}\n    Expected: {EXPECTED_ORDERBOOK_HEADER}")
            errors += 1
        else:
            print("  [PASS] Header matches exact 26-column specification.")

        last_seq_no = None

        for line_num, row in enumerate(reader, start=2):
            row_count += 1
            if len(row) != 26:
                print(f"  [ERROR] Line {line_num}: Expected 26 columns, got {len(row)}")
                errors += 1
                if errors > 10: break
                continue

            try:
                tsec = int(row[0])
                tnsec = int(row[1])
                seq_no = int(row[2])
                id_val = int(row[3])
                ev_type = row[4]
                side = row[5]

                bids = [int(row[6 + i]) for i in range(5)]
                bid_sizes = [int(row[11 + i]) for i in range(5)]
                asks = [int(row[16 + i]) for i in range(5)]
                ask_sizes = [int(row[21 + i]) for i in range(5)]
            except ValueError as e:
                print(f"  [ERROR] Line {line_num}: Integer conversion failed: {e}")
                errors += 1
                if errors > 10: break
                continue

            # Check sequence monotonicity
            if last_seq_no is not None:
                if seq_no != last_seq_no + 1:
                    print(f"  [ERROR] Line {line_num}: Non-monotonic seqNo: prev={last_seq_no}, curr={seq_no}")
                    errors += 1
            last_seq_no = seq_no

            # Check event type and side
            if ev_type not in ('S', 'D', 'T'):
                print(f"  [ERROR] Line {line_num}: Invalid event type '{ev_type}'")
                errors += 1

            if side not in ('B', 'S', 'N'):
                print(f"  [ERROR] Line {line_num}: Invalid side '{side}'")
                errors += 1

            # Check bids strictly descending
            for i in range(4):
                if bids[i] <= bids[i + 1]:
                    print(f"  [ERROR] Line {line_num}: Bids not strictly descending: bid{i}={bids[i]} <= bid{i+1}={bids[i+1]}")
                    errors += 1

            # Check asks strictly ascending
            for i in range(4):
                if asks[i] >= asks[i + 1]:
                    print(f"  [ERROR] Line {line_num}: Asks not strictly ascending: ask{i}={asks[i]} >= ask{i+1}={asks[i+1]}")
                    errors += 1

            # Check best bid < best ask (no crossed book)
            if bids[0] >= asks[0]:
                print(f"  [ERROR] Line {line_num}: Crossed book detected! bid0={bids[0]} >= ask0={asks[0]}")
                errors += 1

            # Check quantities non-negative
            for i in range(5):
                if bid_sizes[i] < 0:
                    print(f"  [ERROR] Line {line_num}: Negative bid_size{i}={bid_sizes[i]}")
                    errors += 1
                if ask_sizes[i] < 0:
                    print(f"  [ERROR] Line {line_num}: Negative ask_size{i}={ask_sizes[i]}")
                    errors += 1

            if errors > 20:
                print("  [ABORT] Too many errors encountered.")
                break

    print(f"  Total rows inspected: {row_count}")
    if errors == 0:
        print("  [RESULT] Deliverable B PASSED all sanity checks.")
        return True
    else:
        print(f"  [RESULT] Deliverable B FAILED with {errors} errors.")
        return False


def main():
    parser = argparse.ArgumentParser(description="CSV Sanity Checker for Binance LOB Capture")
    parser.add_argument("--market-data", "-m", help="Path to market_data CSV (Deliverable A)")
    parser.add_argument("--orderbook", "-b", help="Path to orderbook CSV (Deliverable B)")
    parser.add_argument("--dir", "-d", help="Directory containing capture CSV files to auto-detect")
    args = parser.parse_args()

    success = True

    if args.dir:
        for fname in os.listdir(args.dir):
            full_path = os.path.join(args.dir, fname)
            if fname.endswith(".csv") and ("orderbook" in fname):
                if not check_orderbook(full_path):
                    success = False
            elif fname.endswith(".csv") and ("market_data" in fname):
                if not check_market_data(full_path):
                    success = False
        sys.exit(0 if success else 1)

    if args.market_data:
        if not check_market_data(args.market_data):
            success = False

    if args.orderbook:
        if not check_orderbook(args.orderbook):
            success = False

    if not args.market_data and not args.orderbook and not args.dir:
        parser.print_help()
        sys.exit(1)

    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
