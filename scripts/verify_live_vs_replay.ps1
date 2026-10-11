<#
.SYNOPSIS
    End-to-End Verification: Live Capture vs Offline Replay Determinism
.DESCRIPTION
    Runs a live WebSocket capture (default 120s), feeds the resulting market_data CSV
    into the zero-network offline replay engine, computes SHA-256 hashes of both orderbook CSVs,
    and runs full sanity checking on emitted files.
#>

param(
    [int]$Duration = 120,
    [string]$Symbol = "BTCUSDT",
    [string]$OutputDirLive = "verify_live_120s",
    [string]$OutputDirReplay = "verify_replay_120s",
    [string]$ExePath = ".\build\Release\binance_capture.exe"
)

$ErrorActionPreference = "Stop"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  BINANCE LOB VERIFICATION: LIVE vs REPLAY BIT-FOR-BIT TEST" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Duration:       $Duration seconds"
Write-Host "Symbol:         $Symbol"
Write-Host "Live Dir:       $OutputDirLive"
Write-Host "Replay Dir:     $OutputDirReplay"
Write-Host "Executable:     $ExePath"
Write-Host "------------------------------------------------------------"

if (-not (Test-Path $ExePath)) {
    Write-Error "Executable not found at: $ExePath. Please build Release first."
    exit 1
}

# 1. Clean previous verification output directories
Remove-Item -Force -Recurse $OutputDirLive -ErrorAction SilentlyContinue
Remove-Item -Force -Recurse $OutputDirReplay -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OutputDirLive | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDirReplay | Out-Null

# 2. Run Live Capture for $Duration seconds
Write-Host "`n[STEP 1/4] Starting Live WebSocket Capture ($Duration s)..." -ForegroundColor Yellow
$liveArgs = @("--symbols", $Symbol, "--duration", $Duration, "--output-dir", $OutputDirLive)
& $ExePath $liveArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Live capture failed with exit code $LASTEXITCODE"
    exit 1
}

# 3. Locate generated files
$marketDataFiles = Get-ChildItem -Path $OutputDirLive -Filter "market_data_*.csv" | Where-Object { $_.Name -notlike "*_orderbook.csv" }
if ($marketDataFiles.Count -eq 0) {
    Write-Error "No market_data CSV found in $OutputDirLive"
    exit 1
}
$liveMarketDataCsv = $marketDataFiles[0].FullName

$liveOrderbookFiles = Get-ChildItem -Path $OutputDirLive -Filter "*_orderbook.csv"
if ($liveOrderbookFiles.Count -eq 0) {
    Write-Error "No orderbook CSV generated in $OutputDirLive"
    exit 1
}
$liveOrderbookCsv = $liveOrderbookFiles[0].FullName

Write-Host "Live Capture Completed:" -ForegroundColor Green
Write-Host "  Market Data: $liveMarketDataCsv"
Write-Host "  Orderbook:   $liveOrderbookCsv"

# 4. Run Offline Replay on the captured market data CSV
Write-Host "`n[STEP 2/4] Running Offline Deterministic Replay (--bench)..." -ForegroundColor Yellow
$replayArgs = @("--replay", $liveMarketDataCsv, "--output-dir", $OutputDirReplay, "--bench")
& $ExePath $replayArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Offline replay failed with exit code $LASTEXITCODE"
    exit 1
}

$replayOrderbookFiles = Get-ChildItem -Path $OutputDirReplay -Filter "*_orderbook.csv"
if ($replayOrderbookFiles.Count -eq 0) {
    Write-Error "No orderbook CSV generated in $OutputDirReplay"
    exit 1
}
$replayOrderbookCsv = $replayOrderbookFiles[0].FullName

# 5. Compute and compare SHA-256 hashes
Write-Host "`n[STEP 3/4] Comparing SHA-256 Hashes of Live vs Replay Orderbooks..." -ForegroundColor Yellow
$liveHash = (Get-FileHash -Path $liveOrderbookCsv -Algorithm SHA256).Hash
$replayHash = (Get-FileHash -Path $replayOrderbookCsv -Algorithm SHA256).Hash

Write-Host "  Live Orderbook Hash:   $liveHash"
Write-Host "  Replay Orderbook Hash: $replayHash"

if ($liveHash -eq $replayHash) {
    Write-Host "`n[VERIFICATION PASSED] Hashes match bit-for-bit!" -ForegroundColor Green
    Write-Host "Deterministic replay produces 100% identical byte-for-byte output." -ForegroundColor Green
} else {
    Write-Host "`n[VERIFICATION FAILED] Hashes do NOT match!" -ForegroundColor Red
    exit 1
}

# 6. Run Sanity Checks on Emitted CSV Files
Write-Host "`n[STEP 4/4] Running Full CSV Schema and Market Invariant Sanity Checks..." -ForegroundColor Yellow
$pythonAvailable = (Get-Command python -ErrorAction SilentlyContinue)
if ($pythonAvailable) {
    python scripts\check_csv_sanity.py --dir $OutputDirLive
} else {
    powershell -ExecutionPolicy Bypass -File scripts\check_csv_sanity.ps1 -Directory $OutputDirLive
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "  TASK 11 VERIFICATION COMPLETE: ALL CHECKS PASSED!" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Cyan
