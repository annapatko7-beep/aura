@echo off
chcp 65001 >nul
title Selfvoy Full Stack Launcher
echo ======================================================================
echo           Selfvoy: Полный запуск приложения (Full Stack)
echo ======================================================================
echo.

cd /d "%~dp0"

echo [1/3] Запуск Go AI Core на http://127.0.0.1:8000...
start "Selfvoy AI Core" /min wsl -e sh -c "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a && chmod +x ./bin/ai-core-linux && ./bin/ai-core-linux"
timeout /t 2 >nul

echo [2/3] Запуск C++ WebSocket сервера на ws://127.0.0.1:9000...
start "Selfvoy Server" /min wsl -e sh -c "cd /mnt/c/Users/Konstantin/Desktop/selfvoy && set -a && [ -f .env ] && . ./.env && set +a && /mnt/c/Users/Konstantin/Desktop/selfvoy/server/build/selfvoy-server"
timeout /t 2 >nul

echo [3/3] Запуск веб-клиента Vite на http://localhost:5173...
cd web
start "Selfvoy Web Client" cmd /c "npm run dev"

timeout /t 3 >nul
start http://localhost:5173

echo.
echo ======================================================================
echo  Все компоненты запущены!
echo  - Веб-приложение : http://localhost:5173
echo  - C++ Сервер     : ws://127.0.0.1:9000 (БД: Supabase Cloud)
echo  - Go AI Core     : http://127.0.0.1:8000 (Память + Планировщик)
echo ======================================================================
echo  Для остановки закройте окна запущенных процессов.
pause
