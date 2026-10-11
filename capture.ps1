# Kill any dangling websocat instances from previous runs to release file locks
Get-Process websocat -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$fixturesDir = Join-Path $PWD.Path "fixtures"
if (-not (Test-Path $fixturesDir)) {
    New-Item -ItemType Directory -Path $fixturesDir | Out-Null
}

$filePath = Join-Path $fixturesDir "raw_spot_btcusdt.txt"
$target = 300
$count = 0

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($filePath, $false, $utf8NoBom)

Write-Host "Connecting to Binance Vision WebSocket (fallback host for ISP bypass)..." -ForegroundColor Cyan

try {
    .\websocat.exe "wss://data-stream.binance.vision:9443/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade" | ForEach-Object {
        $writer.WriteLine($_)
        $count++

        $percent = [math]::Min(100, [int](($count * 100) / $target))
        Write-Progress -Activity "Capturing Binance WebSocket Frames" -Status "$count of $target messages captured" -PercentComplete $percent

        if ($count -ge $target) {
            Get-Process websocat -ErrorAction SilentlyContinue | Stop-Process
        }
    }
}
finally {
    $writer.Flush()
    $writer.Dispose()
    Write-Progress -Activity "Capturing Binance WebSocket Frames" -Completed
    Write-Host "`nCapture complete! Saved $count messages to $filePath" -ForegroundColor Green
}
