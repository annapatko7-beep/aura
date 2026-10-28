.PHONY: check ai-test server-build server-test protocol-check e2e llm-check schema-check docker-build docker-up docker-down docker-logs ai-run server-run

# Полный прогон всех проверок
check: ai-test server-test protocol-check

# Python AI-сервис: тесты
ai-test:
	cd ai && PYTHONPATH=. python3 -m pytest -q

# Python AI-сервис: запуск (dev). Слушает 127.0.0.1:8000. Без SELFVOY_AI_TOKEN
# сервис закрыт (503), поэтому для локальной разработки включаем
# SELFVOY_AI_INSECURE_DEV=1; если токен задан — он проверяется всегда.
# В production SELFVOY_AI_INSECURE_DEV не задавать.
ai-run:
	cd ai && SELFVOY_AI_INSECURE_DEV=$${SELFVOY_AI_INSECURE_DEV:-1} PYTHONPATH=. python -m selfvoy_ai.app

# C++ сервер: сборка
server-build:
	cmake -S server -B build/server -G Ninja -DSELFVOY_BUILD_TESTS=ON
	cmake --build build/server

# C++ сервер: тесты (ctest)
server-test: server-build
	cd build/server && ctest --output-on-failure

# C++ сервер: запуск (dev). SELFVOY_DEV_MODE=1 разрешает dev-почту и запуск без
# секретов; в продакшене не задавать (см. docs/SETUP.md).
server-run:
	SELFVOY_DEV_MODE=$${SELFVOY_DEV_MODE:-1} ./build/server/selfvoy-server

# Проверка протокола (iOS + Android vs сервер)
protocol-check:
	python3 tools/check_ios_protocol.py
	python3 tools/check_android_protocol.py

# E2E-сценарий (456 проверок со всеми моками): нужны запущенные AI :8000 и сервер :9000
e2e:
	SELFVOY_E2E_GOOGLE_MOCK=1 SELFVOY_E2E_PUSH_MOCK=1 python3 tools/e2e.py

# Отвечает ли настоящая LLM (OPENAI_API_KEY, SELFVOY_LLM_PROVIDER=openai): поднимает
# AI-сервис на :8077 с ключом из окружения и прогоняет tools/llm_check.py.
llm-check:
	@test -n "$$OPENAI_API_KEY" || { echo "OPENAI_API_KEY не задан (см. docs/DEPLOY.md)"; exit 1; }
	@cd ai && SELFVOY_AI_TOKEN=llm-check-local-token-0123456789 SELFVOY_LLM_PROVIDER=$${SELFVOY_LLM_PROVIDER:-openai} \
		PYTHONPATH=. python3 -c "import uvicorn; from selfvoy_ai.app import app; uvicorn.run(app, host='127.0.0.1', port=8077, log_level='warning')" & \
		pid=$$!; sleep 4; \
		SELFVOY_AI_TOKEN=llm-check-local-token-0123456789 python3 tools/llm_check.py http://127.0.0.1:8077; rc=$$?; \
		kill $$pid; exit $$rc

# Применить схему БД + seed-данные
schema-check:
	psql "$(PG_URI)" -f schema/schema.sql
	psql "$(PG_URI)" -f schema/seed.sql

# Docker Compose: prod-стек (postgres + ai + server). Секреты — в .env
# (см. .env.example); без них compose откажется стартовать.
docker-build:
	docker-compose build

docker-up:
	docker-compose up -d

docker-down:
	docker-compose down

docker-logs:
	docker-compose logs -f
