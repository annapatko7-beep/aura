#!/bin/bash
# SessionStart: зависимости для тестов в облачных сессиях Claude Code
# (make ai-test / server-test / protocol-check, vitest веб-клиента).
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"

# AI-сервис: зависимости ставим тем же интерпретатором, которым идут тесты
# (python3 -m pytest) — отдельный pytest в PATH может быть от другого Python.
python3 -m pip install --quiet --disable-pip-version-check -r ai/requirements.txt pytest httpx

# Веб-клиент: npm install (а не ci) — переиспользует node_modules из кэша контейнера.
(cd web && npm install --no-audit --no-fund --loglevel=error)

# C++-сервер с PostgreSQL-бэкендом (для e2e на PostgreSQL) — нужны заголовки libpq.
if ! [ -f /usr/include/postgresql/libpq-fe.h ] && command -v apt-get >/dev/null 2>&1; then
  apt-get install -y -q libpq-dev >/dev/null 2>&1 || true
fi
