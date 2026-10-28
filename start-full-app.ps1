# Selfvoy Full Stack Launcher (PowerShell)
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "           Selfvoy: Полный запуск приложения (Full Stack)             " -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "[1/3] Запуск Go AI Core на http://127.0.0.1:8000..." -ForegroundColor Green
$wslAiCmd = "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a && chmod +x ./bin/ai-core-linux && ./bin/ai-core-linux"
Start-Process -FilePath "wsl.exe" -ArgumentList "-e", "sh", "-c", $wslAiCmd -WindowStyle Minimized
Start-Sleep -Seconds 2

Write-Host "[2/3] Запуск C++ WebSocket сервера на ws://127.0.0.1:9000..." -ForegroundColor Green
$wslCmd = "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a && /mnt/c/Users/Konstantin/Desktop/selfvoy/server/build/selfvoy-server"
Start-Process -FilePath "wsl.exe" -ArgumentList "-e", "sh", "-c", $wslCmd -WindowStyle Minimized
Start-Sleep -Seconds 2

Write-Host "[3/3] Запуск веб-клиента Vite на http://localhost:5173..." -ForegroundColor Green
Start-Process -FilePath "cmd.exe" -ArgumentList "/c cd web && npm run dev"
Start-Sleep -Seconds 3

Start-Process "http://localhost:5173"

Write-Host "`n======================================================================" -ForegroundColor Cyan
Write-Host " Все компоненты запущены!" -ForegroundColor Green
Write-Host " - Веб-приложение : http://localhost:5173" -ForegroundColor White
Write-Host " - C++ Сервер     : ws://127.0.0.1:9000 (БД: Supabase Cloud)" -ForegroundColor White
Write-Host " - Go AI Core     : http://127.0.0.1:8000 (Память + Планировщик)" -ForegroundColor White
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "Для остановки закройте фоновые терминалы.`n" -ForegroundColor Gray
