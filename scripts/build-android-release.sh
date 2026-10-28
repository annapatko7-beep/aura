#!/usr/bin/env bash
# ==============================================================================
# Selfvoy Android (.aab & .apk) Release Build Script (Bash/CI)
# ==============================================================================

set -euo pipefail

SERVER_URL="${1:-wss://api.yourdomain.com}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
WEB_DIR="${ROOT_DIR}/web"
ANDROID_DIR="${WEB_DIR}/android"
RELEASE_DIR="${ROOT_DIR}/release"

echo "=== Сборка релизной версии Selfvoy для Android ==="
echo "Боевой сервер WebSocket: $SERVER_URL"

export VITE_SELFVOY_SERVER_URL="$SERVER_URL"
export NODE_ENV="production"

cd "$WEB_DIR"
echo "[1/4] Сборка веб-бандла..."
npm run build

echo "[2/4] Синхронизация Capacitor..."
npx cap sync android

cd "$ANDROID_DIR"
echo "[3/4] Сборка Google Play App Bundle (bundleRelease)..."
./gradlew bundleRelease

echo "[4/4] Сборка APK (assembleRelease)..."
./gradlew assembleRelease

mkdir -p "$RELEASE_DIR"

if ls app/build/outputs/bundle/release/*.aab 1> /dev/null 2>&1; then
    cp app/build/outputs/bundle/release/*.aab "${RELEASE_DIR}/selfvoy-release.aab"
    echo "✓ Сохранён: ${RELEASE_DIR}/selfvoy-release.aab"
fi

if ls app/build/outputs/apk/release/*.apk 1> /dev/null 2>&1; then
    cp app/build/outputs/apk/release/*.apk "${RELEASE_DIR}/selfvoy-release.apk"
    echo "✓ Сохранён: ${RELEASE_DIR}/selfvoy-release.apk"
fi

echo "=== Сборка Android завершена успешно! ==="
