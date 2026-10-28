@echo off
chcp 65001 >nul
title Selfvoy AI Core 2.0 (Go 1.23+)
echo ======================================================================
echo         Selfvoy AI Core 2.0 (High-Performance Go Engine)
echo ======================================================================
echo.

cd /d "%~dp0"

if not exist "bin\ai-core.exe" (
    echo [INFO] Сборка бинарного файла bin\ai-core.exe...
    set "PATH=%USERPROFILE%\go-sdk\go\bin;%PATH%"
    go build -ldflags="-s -w" -o bin\ai-core.exe .\cmd\ai-core
    if errorlevel 1 (
        echo [ERROR] Ошибка сборки Go-бинарника.
        pause
        exit /b 1
    )
)

echo [INFO] Запуск AI Core на http://127.0.0.1:8000...
echo [INFO] Нажмите Ctrl+C для остановки.
echo.
bin\ai-core.exe
