<#
.SYNOPSIS
    PowerShell CSV Sanity Checker for Binance LOB Capture Engine
.DESCRIPTION
    Validates Deliverable A (9 columns, conn_seq monotonic per epoch)
    and Deliverable B (26 columns, bids descending, asks ascending, bid0 < ask0, seqNo monotonic)
#>

param(
    [string]$Directory = "",
    [string]$MarketDataFile = "",
    [string]$OrderbookFile = ""
)

$expectedMDHeader = "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json"
$expectedOBHeader = "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4"

function Check-MarketDataFile([string]$path) {
    Write-Host "[SANITY] Checking Market Data CSV: $path" -ForegroundColor Cyan
    if (-not (Test-Path $path)) {
        Write-Error "File does not exist: $path"
        return $false
    }

    $lines = [System.IO.File]::ReadLines($path)
    $enumerator = $lines.GetEnumerator()
    if (-not $enumerator.MoveNext()) {
        Write-Error "File is empty: $path"
        return $false
    }

    $header = $enumerator.Current
    if ($header -ne $expectedMDHeader) {
        Write-Error "Header mismatch! Got: $header"
        return $false
    }
    Write-Host "  [PASS] Header matches exact 9-column specification." -ForegroundColor Green

    $lastEpoch = -1
    $lastSeq = -1
    $rowCount = 0
    $errors = 0

    while ($enumerator.MoveNext()) {
        $line = $enumerator.Current
        $rowCount++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }

        # Extract first 8 comma-separated fields
        $parts = $line.Split(',', 9)
        if ($parts.Count -ne 9) {
            Write-Warning "Row $rowCount does not have 9 columns"
            $errors++
            if ($errors -gt 5) { break }
            continue
        }

        $epoch = [int64]$parts[5]
        $seq = [int64]$parts[6]

        if ($lastEpoch -ne $epoch) {
            $lastEpoch = $epoch
            $lastSeq = $seq
        } else {
            if ($seq -ne ($lastSeq + 1)) {
                Write-Warning "Row $rowCount non-monotonic conn_seq in epoch $epoch (prev=$lastSeq, curr=$seq)"
                $errors++
            }
            $lastSeq = $seq
        }
    }

    Write-Host "  Total rows inspected: $rowCount"
    if ($errors -eq 0) {
        Write-Host "  [RESULT] Deliverable A PASSED all checks." -ForegroundColor Green
        return $true
    } else {
        Write-Host "  [RESULT] Deliverable A FAILED with $errors errors." -ForegroundColor Red
        return $false
    }
}

function Check-OrderbookFile([string]$path) {
    Write-Host "[SANITY] Checking Orderbook CSV: $path" -ForegroundColor Cyan
    if (-not (Test-Path $path)) {
        Write-Error "File does not exist: $path"
        return $false
    }

    $lines = [System.IO.File]::ReadLines($path)
    $enumerator = $lines.GetEnumerator()
    if (-not $enumerator.MoveNext()) {
        Write-Error "File is empty: $path"
        return $false
    }

    $header = $enumerator.Current
    if ($header -ne $expectedOBHeader) {
        Write-Error "Header mismatch! Got: $header"
        return $false
    }
    Write-Host "  [PASS] Header matches exact 26-column specification." -ForegroundColor Green

    $lastSeqNo = -1
    $rowCount = 0
    $errors = 0

    while ($enumerator.MoveNext()) {
        $line = $enumerator.Current
        $rowCount++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }

        $parts = $line.Split(',')
        if ($parts.Count -ne 26) {
            Write-Warning "Row $rowCount does not have 26 columns (got $($parts.Count))"
            $errors++
            if ($errors -gt 5) { break }
            continue
        }

        $seqNo = [int64]$parts[2]
        if ($lastSeqNo -ne -1 -and $seqNo -ne ($lastSeqNo + 1)) {
            Write-Warning "Row $rowCount non-monotonic seqNo (prev=$lastSeqNo, curr=$seqNo)"
            $errors++
        }
        $lastSeqNo = $seqNo

        $bid0 = [int64]$parts[6]
        $bid1 = [int64]$parts[7]
        $bid2 = [int64]$parts[8]
        $bid3 = [int64]$parts[9]
        $bid4 = [int64]$parts[10]

        $ask0 = [int64]$parts[16]
        $ask1 = [int64]$parts[17]
        $ask2 = [int64]$parts[18]
        $ask3 = [int64]$parts[19]
        $ask4 = [int64]$parts[20]

        # Bids strictly descending
        if (-not ($bid0 -gt $bid1 -and $bid1 -gt $bid2 -and $bid2 -gt $bid3 -and $bid3 -gt $bid4)) {
            Write-Warning "Row $rowCount bids not strictly descending: $bid0, $bid1, $bid2, $bid3, $bid4"
            $errors++
        }

        # Asks strictly ascending
        if (-not ($ask0 -lt $ask1 -and $ask1 -lt $ask2 -and $ask2 -lt $ask3 -and $ask3 -lt $ask4)) {
            Write-Warning "Row $rowCount asks not strictly ascending: $ask0, $ask1, $ask2, $ask3, $ask4"
            $errors++
        }

        # Spread positive (no cross)
        if ($bid0 -ge $ask0) {
            Write-Warning "Row $rowCount crossed book: bid0 ($bid0) >= ask0 ($ask0)"
            $errors++
        }

        if ($errors -gt 20) { break }
    }

    Write-Host "  Total rows inspected: $rowCount"
    if ($errors -eq 0) {
        Write-Host "  [RESULT] Deliverable B PASSED all checks." -ForegroundColor Green
        return $true
    } else {
        Write-Host "  [RESULT] Deliverable B FAILED with $errors errors." -ForegroundColor Red
        return $false
    }
}

if ($Directory -ne "") {
    Get-ChildItem -Path $Directory -Filter "*orderbook*.csv" | ForEach-Object { Check-OrderbookFile $_.FullName }
    Get-ChildItem -Path $Directory -Filter "*market_data*.csv" | Where-Object { $_.Name -notlike "*orderbook*" } | ForEach-Object { Check-MarketDataFile $_.FullName }
} elseif ($MarketDataFile -ne "") {
    Check-MarketDataFile $MarketDataFile
} elseif ($OrderbookFile -ne "") {
    Check-OrderbookFile $OrderbookFile
} else {
    Write-Host "Usage: .\scripts\check_csv_sanity.ps1 -Directory <dir>"
}
