# Короткие команды разработки Aura.
#   make help — список целей

VENV    ?= .venv
# Если локального .venv нет — берём системный интерпретатор
PY      := $(if $(wildcard $(VENV)/bin/python),$(VENV)/bin/python,python3)
PIP     := $(if $(wildcard $(VENV)/bin/pip),$(VENV)/bin/pip,pip3)
UVICORN := $(if $(wildcard $(VENV)/bin/uvicorn),$(VENV)/bin/uvicorn,uvicorn)
PG_URI ?= postgresql://postgres:@/postgres?host=$(HOME)/.cache/pgdata

.PHONY: help venv ai-test server-build server-test e2e schema-check ai-run server-run preview check docker-build docker-up docker-down docker-logs

help:
	@echo "Aura — цели:"
	@echo "  venv          создать .venv и поставить зависимости AI-сервиса"
	@echo "  ai-test       тесты Python AI Service"
	@echo "  server-build  собрать C++-сервер (CMake)"
	@echo "  server-test   юнит-тесты C++-сервера через ctest"
	@echo "  e2e           сквозной сценарий (нужны запущенные серверы)"
	@echo "  schema-check  применить schema.sql к PostgreSQL из PG_URI"
	@echo "  ai-run        запустить AI-сервис на :8000"
	@echo "  server-run    запустить C++-сервер на :9000"
	@echo "  check         всё вышеперечисленное, кроме e2e и *-run"
	@echo "  docker-build  собрать образы стека (postgres + ai + server)"
	@echo "  docker-up     поднять стек в фоне"
	@echo "  docker-down   остановить стек"
	@echo "  docker-logs   логи сервера и AI-сервиса"

venv:
	python3 -m venv $(VENV)
	$(PIP) install -r ai/requirements.txt

ai-test:
	cd ai && PYTHONPATH=. $(PY) -m pytest -q

server-build:
	cmake -S server -B build/server
	cmake --build build/server

server-test: server-build
	ctest --test-dir build/server --output-on-failure

e2e:
	$(PY) tools/e2e.py

schema-check:
	psql "$(PG_URI)" -v ON_ERROR_STOP=1 -f schema/schema.sql
	psql "$(PG_URI)" -v ON_ERROR_STOP=1 -f schema/seed.sql

ai-run:
	cd ai && PYTHONPATH=. $(UVICORN) aura_ai.app:app --host 0.0.0.0 --port 8000

server-run:
	AURA_DATABASE_URL="$(PG_URI)" AURA_AI_URL=http://127.0.0.1:8000 \
	  ./build/server/aura-server

check: ai-test server-test
	@echo "OK: тесты Python и C++ прошли"

# --- production-стек (Docker Compose: postgres + ai + server) --------------
# Секреты — через окружение или .env-файл compose (см. docs/SETUP.md).

docker-build:
	docker compose build

docker-up:
	docker compose up -d

docker-down:
	docker compose down

docker-logs:
	docker compose logs -f server ai
