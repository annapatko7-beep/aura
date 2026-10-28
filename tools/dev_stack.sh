#!/usr/bin/env bash
# Dev-стек для проверки клиентов: AI-сервис :8000 + сервер :9000 со встроенной
# БД, dev-почтой (код подтверждения в ответе) и песочницей инструментов.
# Секреты генерируются на запуск, если не заданы в окружении.
#   cmake -S . -B build -DSELFVOY_WITH_LIBPQ=OFF && cmake --build build
#   tools/dev_stack.sh            # логи и база — в build/devstack
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN=${SELFVOY_RUN_DIR:-$ROOT/build/devstack}
SERVER=${SELFVOY_SERVER_BIN:-$ROOT/build/server/selfvoy-server}
PYTHON=${PYTHON:-python3}
mkdir -p "$RUN"

wait_port() {  # host port seconds
    for _ in $(seq 1 "$3"); do
        (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# Общий секрет сервер <-> AI: без него AI-сервис закрыт (503).
export SELFVOY_AI_TOKEN=${SELFVOY_AI_TOKEN:-$(openssl rand -hex 32)}
(cd "$ROOT/ai" && PYTHONPATH=. nohup "$PYTHON" -m selfvoy_ai.app > "$RUN/ai.log" 2>&1 &)
wait_port 127.0.0.1 8000 60 || { echo "AI-сервис не поднялся"; tail -40 "$RUN/ai.log"; exit 1; }

export SELFVOY_JWT_SECRET=${SELFVOY_JWT_SECRET:-$(openssl rand -hex 32)}
export SELFVOY_2FA_KEY=${SELFVOY_2FA_KEY:-$(openssl rand -hex 32)}
export SELFVOY_EMBEDDED_DB=${SELFVOY_EMBEDDED_DB:-$RUN/db.json}
export SELFVOY_MAIL_DRIVER=dev SELFVOY_TOOLS_MODE=sandbox SELFVOY_AI_URL=http://127.0.0.1:8000
nohup "$SERVER" > "$RUN/server.log" 2>&1 &
wait_port 127.0.0.1 9000 60 || { echo "Сервер не поднялся"; tail -40 "$RUN/server.log"; exit 1; }
echo "dev-стек: AI http://127.0.0.1:8000, сервер ws://127.0.0.1:9000 (логи: $RUN)"
