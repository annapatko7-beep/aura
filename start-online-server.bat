@echo off
chcp 65001 >nul
title Selfvoy Online Server + Cloudflare Tunnel
echo ======================================================================
echo          Selfvoy: Запуск бесплатного публичного сервера
echo ======================================================================
echo.
echo [1/2] Запуск сервера Selfvoy и AI Core в WSL на портах 8000/9000...
start /b wsl -e sh -c "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a ; (chmod +x ./bin/ai-core-linux && ./bin/ai-core-linux &) ; sleep 2 ; /mnt/c/Users/Konstantin/Desktop/selfvoy/server/build/selfvoy-server"
timeout /t 3 >nul

echo [2/2] Запуск защищенного публичного туннеля Cloudflare...
echo.
echo Публичный адрес туннеля появится ниже:
echo ======================================================================
wsl -e cloudflared tunnel --url http://127.0.0.1:9000
