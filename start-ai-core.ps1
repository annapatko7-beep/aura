# Selfvoy AI Core 2.0 (Go 1.23+) - PowerShell Launcher
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "         Selfvoy AI Core 2.0 (High-Performance Go Engine)             " -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

$BinPath = Join-Path $Root "bin\ai-core.exe"

if (-not (Test-Path $BinPath)) {
    Write-Host "[INFO] Компиляция бинарного файла bin/ai-core.exe..." -ForegroundColor Yellow
    $env:PATH = "$env:USERPROFILE\go-sdk\go\bin;" + $env:PATH
    go build -ldflags="-s -w" -o bin/ai-core.exe ./cmd/ai-core
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Ошибка сборки Go-бинарника." -ForegroundColor Red
        exit 1
    }
}

Write-Host "[INFO] Запуск AI Core на http://127.0.0.1:8000..." -ForegroundColor Green
Write-Host "[INFO] Нажмите Ctrl+C для завершения процесса.`n" -ForegroundColor Gray

& $BinPath
