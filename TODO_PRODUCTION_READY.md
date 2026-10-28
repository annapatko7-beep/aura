# TODO до production-релиза v1.0

**Дата аудита:** 30.09.2026  
**Статус готовности:** 68% — осталось 5-7 дней работы  
**Основная проблема:** Код готов на 95%, нет инфраструктуры для развёртывания

---

## 🔥 КРИТИЧЕСКИЕ БЛОКЕРЫ (День 1-2)

### 1. Makefile
**Где:** `./Makefile` (создать)  
**Зачем:** CLAUDE.md ссылается на `make check`, `make docker-up` — но файла нет

**Команды для реализации:**
```makefile
check: ai-test server-test protocol-check
ai-test:
	cd ai && PYTHONPATH=. pytest -q
server-build:
	cmake -S server -B build/server -G Ninja -DSELFVOY_BUILD_TESTS=ON
	cmake --build build/server
server-test: server-build
	cd build/server && ctest --output-on-failure
protocol-check:
	python3 tools/check_ios_protocol.py
	python3 tools/check_android_protocol.py
e2e:
	SELFVOY_E2E_GOOGLE_MOCK=1 SELFVOY_E2E_PUSH_MOCK=1 python3 tools/e2e.py
schema-check:
	psql "$(PG_URI)" -f schema/schema.sql
	psql "$(PG_URI)" -f schema/seed.sql
docker-up:
	docker-compose up -d
docker-down:
	docker-compose down
docker-logs:
	docker-compose logs -f
```

---

### 2. docker-compose.yml
**Где:** `./docker-compose.yml` (создать)  
**Зачем:** SETUP.md §8 описывает стек, но файла нет

**Структура:**
```yaml
version: '3.8'
services:
  postgres:
    image: postgres:16
    environment:
      POSTGRES_DB: selfvoy
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
    volumes:
      - pgdata:/var/lib/postgresql/data
      - ./schema:/docker-entrypoint-initdb.d
    healthcheck:
      test: pg_isready -U postgres
  
  ai:
    build: ./ai
    depends_on:
      postgres: {condition: service_healthy}
    environment:
      SELFVOY_MEMORY_BACKEND: postgres
      SELFVOY_AI_TOKEN: ${SELFVOY_AI_TOKEN}
      SELFVOY_LLM_PROVIDER: ${SELFVOY_LLM_PROVIDER:-mock}
    healthcheck:
      test: curl -f http://localhost:8000/healthz
  
  server:
    build: ./server
    depends_on:
      ai: {condition: service_healthy}
      postgres: {condition: service_healthy}
    ports:
      - "9000:9000"
    environment:
      SELFVOY_DATABASE_URL: postgresql://postgres:${POSTGRES_PASSWORD}@postgres/selfvoy
      SELFVOY_AI_URL: http://ai:8000
      SELFVOY_JWT_SECRET: ${SELFVOY_JWT_SECRET}
      SELFVOY_2FA_KEY: ${SELFVOY_2FA_KEY}

volumes:
  pgdata:
```

---

### 3. .env.example
**Где:** `./.env.example` (создать)  
**Зачем:** 36+ env-переменных — нужен шаблон

```bash
# ========== КРИТИЧНЫЕ СЕКРЕТЫ (ЗАМЕНИТЬ В ПРОДЕ!) ==========
SELFVOY_JWT_SECRET=dev-jwt-secret-CHANGE-IN-PRODUCTION
SELFVOY_2FA_KEY=dev-2fa-key-CHANGE-IN-PRODUCTION
POSTGRES_PASSWORD=postgres

# ========== AI Service ==========
SELFVOY_AI_TOKEN=shared-secret-cpp-ai-CHANGE-IN-PRODUCTION
SELFVOY_LLM_PROVIDER=mock
# OPENAI_API_KEY=sk-...
# SELFVOY_LLM_MODEL=gpt-4

# ========== Database ==========
SELFVOY_DATABASE_URL=postgresql://postgres:postgres@localhost/selfvoy

# ========== Сеть ==========
SELFVOY_HOST=0.0.0.0
SELFVOY_PORT=9000
SELFVOY_MAX_CONNECTIONS=1024

# ========== Push-уведомления ==========
SELFVOY_PUSH_DRIVER=dev
# SELFVOY_PUSH_DRIVER=webhook
# SELFVOY_PUSH_WEBHOOK_URL=https://push-gateway.internal/push

# ========== Google OAuth (опционально) ==========
# SELFVOY_GOOGLE_CLIENT_ID=
# SELFVOY_GOOGLE_CLIENT_SECRET=

# ========== Rate Limiting ==========
SELFVOY_MAX_LOGIN_ATTEMPTS=8
SELFVOY_LOGIN_WINDOW=300

# ========== Логи ==========
SELFVOY_LOG_LEVEL=info
```

---

### 4. README.md
**Где:** `./README.md` (создать)  
**Зачем:** Первое, что видят пользователи — сейчас пусто

**Структура:**
```markdown
# Selfvoy — Персональный ИИ-агент

[![CI](https://github.com/USER/selfvoy/actions/workflows/ci.yml/badge.svg)](...)

Кроссплатформенное приложение с мультиагентной архитектурой: 
C++ WebSocket-сервер + Python AI-сервис + клиенты (Qt/QML, iOS, Android).

## ✨ Возможности
- 🔐 Безопасность: Argon2id, 2FA (TOTP), OAuth 2.0, шифрование токенов
- 🤖 ИИ-агент с долговременной памятью и инструментами
- 🗣️ Гибридный голос: on-device (iOS) + серверный STT
- 📱 Мультиплатформа: Desktop (Win/Mac/Linux), iOS, Android
- 🔗 A2A: переговоры между агентами

## 🚀 Быстрый старт
```bash
git clone https://github.com/USER/selfvoy-main.git
cd selfvoy-main
cp .env.example .env
# Отредактируйте .env (минимум: секреты JWT/2FA)
make docker-up
# Откройте http://localhost:9000 или запустите клиент
```

## 📚 Документация
- [Архитектура](docs/ARCHITECTURE.md) — диаграммы и потоки данных
- [Установка](docs/SETUP.md) — dev + production
- [Протокол](docs/PROTOCOL.md) — WebSocket API
- [Безопасность](docs/SECURITY.md) — криптография и 2FA
- [Тестирование](docs/TESTING.md) — 56+83+140 тестов

## 📸 Скриншоты
[TODO: добавить 2-3 изображения UI]

## 🧪 Тестирование
```bash
make check  # pytest + ctest + protocol-check
make e2e    # сквозной сценарий (141 проверка)
```

## 📄 Лицензия
[TODO: указать лицензию — MIT/Apache/Proprietary]
```

---

### 5. LICENSE
**Где:** `./LICENSE` (создать)  
**Зачем:** Без лицензии проект нельзя использовать

**Варианты:**
- MIT — максимально свободная
- Apache 2.0 — защита от патентов
- AGPL-3.0 — требование открытия SaaS
- Proprietary — коммерческая

---

### 6. AI-сервис: SELFVOY_AI_TOKEN обязателен
**Где:** `ai/selfvoy_ai/app.py:15-20, 92`  
**Проблема:** Без токена эндпоинты открыты + CORS `*` (CVE B3)

**Исправить:**
```python
# Строка 15-20
service_token = os.getenv("SELFVOY_AI_TOKEN")
if not service_token:
    if os.getenv("ENV") == "production":
        raise ValueError("SELFVOY_AI_TOKEN обязателен в production!")
    logger.warning("⚠️  SELFVOY_AI_TOKEN не задан — эндпоинты ОТКРЫТЫ (только dev!)")

# Строка 92 (CORS)
allowed_origins = os.getenv("SELFVOY_CORS_ORIGINS", "http://localhost:9000").split(",")
app.add_middleware(CORSMiddleware, allow_origins=allowed_origins)  # было: ["*"]
```

---

## 🟡 ВАЖНЫЕ ДОРАБОТКИ (День 3-5)

### 7. Health-check для C++-сервера
**Где:** `server/src/server.cpp` (добавить обработчик)  
**Зачем:** Kubernetes/Docker нужен эндпоинт для проверки живости

```cpp
// Добавить в registerHandler:
handlers["health.check"] = [](const Json& req, int64_t userId) -> Json {
    // Проверить БД: SELECT 1
    // Проверить AI: GET /healthz
    return Json::object({
        {"status", "ok"},
        {"database", db_connected ? "up" : "down"},
        {"ai_service", ai_reachable ? "up" : "down"}
    });
};
```

---

### 8. Graceful shutdown
**Где:** `server/src/main.cpp` (добавить signal handler)

```cpp
#include <csignal>

void shutdown_handler(int signal) {
    log::info("Received signal {}, shutting down gracefully...", signal);
    // 1. Остановить accept новых соединений
    // 2. Отправить close-фреймы всем клиентам
    // 3. Дождаться завершения (макс 10 сек)
    // 4. exit(0)
}

int main() {
    signal(SIGTERM, shutdown_handler);
    signal(SIGINT, shutdown_handler);
    // ...
}
```

---

### 9. Rate limiting для дорогих операций
**Где:** `server/src/server.cpp` (добавить проверки)

```cpp
// Перед вызовом agent.ask:
if (!rateLimiter.checkLimit(userId, "agent.ask", 10, 60)) {
    return error("rate_limit", "Максимум 10 запросов к агенту в минуту");
}

// Аналогично для speech.transcribe (20/мин)
```

---

### 10. Бэкапы PostgreSQL
**Где:** `tools/backup.sh` (создать)

```bash
#!/bin/bash
set -e
BACKUP_DIR=${BACKUP_DIR:-./backups}
mkdir -p "$BACKUP_DIR"
FILENAME="selfvoy-backup-$(date +%Y%m%d-%H%M%S).sql.gz"
pg_dump "$SELFVOY_DATABASE_URL" | gzip > "$BACKUP_DIR/$FILENAME"
echo "✅ Backup saved: $BACKUP_DIR/$FILENAME"
# TODO: загрузить в S3/MinIO
```

**Добавить в cron:**
```
0 2 * * * /path/to/backup.sh  # каждый день в 02:00
```

---

### 11. CHANGELOG.md
**Где:** `./CHANGELOG.md` (создать)

```markdown
# Changelog

## [1.0.0] - 2026-10-XX

### Added
- Мультиплатформенные клиенты (Desktop/iOS/Android)
- 2FA (TOTP) + резервные коды
- Google OAuth (Calendar + Gmail)
- Push-уведомления (APNs/FCM через webhook)
- Гибридный голос (on-device + серверный STT)
- A2A переговоры агентов
- 56 unit-тестов C++ + 83 Python + 140 E2E

### Security
- Argon2id для паролей
- AES-256-GCM для токенов интеграций
- Rate limiting + brute-force защита
- Исправлены CVE A1-A2, B1-B6, C1-C8, E1-E5 (см. docs/AUDIT.md)
```

---

### 12. Push-gateway документация
**Где:** `docs/PUSH_GATEWAY.md` (создать)

```markdown
# Push Gateway для Selfvoy

C++-сервер не умеет TLS/HTTP2/ES256 — шлюз подписывает JWT и форвардит в APNs/FCM.

## Контракт
POST /push
{
  "platform": "apns|fcm",
  "token": "device-token",
  "notification_id": 123,
  "apns": {
    "url": "https://api.push.apple.com/3/device/...",
    "headers": {"apns-topic": "app.selfvoy"},
    "payload": {"aps": {...}}
  }
}

## Пример на Python (50 строк)
[TODO: добавить minimal gateway с jwt.encode + httpx]
```

---

## 📝 КАК ВЕРНУТЬСЯ К ЭТОМУ ДИАЛОГУ ЗАВТРА

1. **Открыть этот файл:** `TODO_PRODUCTION_READY.md`
2. **Сказать Claude:**
   > "Вчера мы делали аудит проекта Selfvoy. Осталось реализовать пункты из `TODO_PRODUCTION_READY.md`. Начни с блокеров 1-6 (Makefile, docker-compose, .env.example, README, LICENSE, AI токен)."

3. **Ссылка на отчёт:** Полный аудит в моём последнем ответе перед этим файлом

---

## ✅ ЧТО УЖЕ ГОТОВО (не трогать)

- ✅ C++ сервер: 27 файлов, 56 тестов, 0 ошибок ASan/UBSan
- ✅ Python AI: 10 файлов, 83 теста
- ✅ Документация: 17 MD-файлов (520+ страниц)
- ✅ Мобильные клиенты: iOS (Swift), Android (Kotlin)
- ✅ CI/CD: GitHub Actions (сборка + тесты)
- ✅ Безопасность: все CVE из AUDIT.md исправлены
- ✅ База данных: 22 таблицы + 6 миграций

---

## 🎯 ПРИОРИТЕТЫ

**День 1:** Блокеры 1-5 (Makefile → docker-compose → .env → README → LICENSE)  
**День 2:** Блокер 6 (AI токен) + проверка `make check`  
**День 3-4:** Важные доработки 7-12 (health, shutdown, rate limit, backup)  
**День 5:** Полное тестирование + tag v1.0.0  

**Время до релиза:** 5-7 дней  
**Текущая готовность:** 68%

---

## 🎨 РЕДИЗАЙН UI ПО НОВЫМ МАКЕТАМ (отдельная задача, добавлено 30.09.2026)

**Источник:** 8 high-fidelity макетов от пользователя (`D:\Загрузки\high-fidelity-*.png`)
**Цель:** довести реальный UI (preview + Qt/QML) до уровня новых макетов

### Что нового в макетах vs текущее превью
- Глубже glassmorphism: сильнее свечение по краям (violet/magenta), глянцевые преломляющие рамки, ambient-подсветка под панелями
- Login: круглый светящийся лого (кольцо из лучей), gradient-кнопка Sign in со свечением, иконки в полях (конверт, замок)
- Register: орбитальные эллипсы на фоне, blur-градиенты, иконка «глаз» для пароля, dropdown таймзоны
- Home: крупный «Good evening, Alex», круглая фиолетовая кнопка микрофона, карточка «Your Selfvoy is active» со свечением
- Tasks: цветные точки-статусы справа, прогресс-бар «Time until next event», FAB «+»
- Contacts: аватары ИИ-агентов (рендер-персонажи), плавающие сферы на фоне, детальная карточка справа
- Negotiations: две карточки агентов с анимированными звуковыми волнами, таймлайн раундов с номерами
- Chat (My Selfvoy): аватар Selfvoy с фиолетовым градиентом-свечением, чипы быстрых ответов, «Selfvoy is thinking…»
- Settings: лило индикатор активного пункта меню, карточки разрешений (Calendar/Email/Booking/Payments) с тумблерами

### План работ (после блокеров v1.0)
- [ ] Скопировать макеты из D:\Загрузки в docs/design-mockups/ для референса
- [ ] Обновить токены в docs/DESIGN.md (усилить glow, gradient-кнопки)
- [ ] Синхронно править 3 источника токенов (правило CLAUDE.md — все сразу):
      client/qml/theme/SelfvoyTheme.qml + preview/styles.css + client/styles/selfvoy_theme.qss
- [ ] Gradient-fill для акцентных кнопок (violet -> lavender + glow)
- [ ] Login: круглый лого-эмблема (SVG/Canvas), иконки в полях
- [ ] Register: орбитальные линии + blur-пятна на фоне
- [ ] Home: круглая кнопка микрофона, карточка статуса со свечением
- [ ] Tasks: цветные точки-статусы + прогресс-бар + FAB
- [ ] Contacts: плавающие сферы + детальная карточка агента
- [ ] Negotiations: звуковые волны агентов + таймлайн с номерами
- [ ] Chat: аватар Selfvoy с градиентом + индикатор «thinking» + чипы ответов
- [ ] Settings: индикатор активного пункта + иконки разрешений (добавить Payments)
- [ ] Обновить preview/index.html + preview/app.js под новые экраны
- [ ] qmlcachegen по всем QML + проверка контраста (>= 4.5:1)
