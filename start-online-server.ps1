# Selfvoy: Запуск бесплатного публичного сервера
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "         Selfvoy: Запуск бесплатного публичного сервера               " -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

Write-Host "`n[1/2] Запуск сервера Selfvoy и AI Core в WSL на портах 8000/9000..." -ForegroundColor Yellow
Start-Process -FilePath "wsl.exe" -ArgumentList "-e", "sh", "-c", "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a ; (chmod +x ./bin/ai-core-linux && ./bin/ai-core-linux &) ; sleep 2 ; /mnt/c/Users/Konstantin/Desktop/selfvoy/server/build/selfvoy-server" -WindowStyle Hidden
Start-Sleep -Seconds 3

Write-Host "`n[2/2] Запуск защищенного публичного туннеля Cloudflare..." -ForegroundColor Yellow
Write-Host "Публичный адрес туннеля появится ниже (Ctrl+C для остановки):`n" -ForegroundColor Green
wsl.exe -e cloudflared tunnel --url http://127.0.0.1:9000
