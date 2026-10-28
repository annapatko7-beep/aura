# Changelog

Все значимые изменения проекта Selfvoy документируются в этом файле.

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.0.0/),
версионирование следует [Semantic Versioning](https://semver.org/lang/ru/).

## [Unreleased]

### Changed
- Продукт переименован: Aura → **Selfvoy**. Переменные окружения `SELFVOY_*`
  (старые `AURA_*` сервер и AI-сервис читают ещё один релиз, с предупреждением),
  пакет `selfvoy_ai`, цели CMake `selfvoy-*`, deep links `selfvoy://`, Android
  `app.selfvoy`, iOS `ios/Selfvoy`, база `selfvoy` и роли `selfvoy_app`/`selfvoy_ai`.
  Установка прежней версии переводится на новые имена повторным запуском
  `deploy/install.sh` (см. docs/DEPLOY.md).

### В планах
- Mobile push через webhook-gateway (документация готова)
- Graceful shutdown C++-сервера (SIGTERM/SIGINT)
- Health-check эндпоинт для Kubernetes
- Автоматические бэкапы PostgreSQL

---

## [1.0.0] - 2026-10-XX

Первый production-ready релиз. Полный стек: C++ WebSocket-сервер, Python AI-сервис, клиенты на Qt6/iOS/Android.

### Added

#### Серверная часть (C++17)
- WebSocket-протокол (35+ типов сообщений) с JSON-валидацией
- Аутентификация: Argon2id для паролей (12 итераций, 64 МБ памяти)
- 2FA (TOTP RFC 6238) с резервными кодами (10 штук, AES-256-GCM)
- Google OAuth 2.0: авторизация + интеграции (Calendar, Gmail)
- JWT-токены (HS256, TTL 7 дней, refresh 30 дней)
- Push-уведомления: webhook-драйвер для APNs/FCM (TLS/HTTP2 вне C++)
- Rate limiting: защита от brute-force (8 попыток / 5 минут)
- Управление задачами и уведомлениями (22 таблицы PostgreSQL)
- 56 unit-тестов (ctest), проверка ASan/UBSan

#### AI-сервис (Python 3.11+, FastAPI)
- LLM-оркестрация: OpenAI API-совместимые модели
- Долговременная память: 4 типа (fact, preference, schedule, contact)
- RAG-поиск: BM25 + семантические эмбеддинги (опционально)
- Agent-to-Agent переговоры (до 5 раундов, затем эскалация)
- Whisper-совместимый STT (multipart/form-data + JSON base64)
- Планировщик: свободные слоты с учётом календаря и предпочтений
- 10 инструментов (weather, web_search, calculator, task_manager, и др.)
- 83 теста (pytest), покрытие 87%

#### Клиентские приложения
- **Desktop (Qt6/QML):** Windows/macOS/Linux, glassmorphism UI, WebSocket + HTTP/2
- **iOS (SwiftUI):** on-device распознавание голоса, deep links (selfvoy://)
- **Android (Kotlin + Compose):** два модуля (selfvoykit — чистый JVM, app — UI)
- Гибридный голос: on-device (iOS) + серверный STT fallback
- Локальное шифрование токенов интеграций (AES-256-GCM)

#### Инфраструктура
- Docker Compose: postgres + ai + server (healthchecks, volume persistence)
- GitHub Actions CI: pytest + ctest + protocol-check + сборка .exe/.apk
- Makefile: `make check` (все тесты), `make e2e` (141 проверка), `make docker-up`
- E2E-тесты: эталонный Python-клиент протокола (tools/e2e.py)
- Protocol checkers: автоматическая сверка типов iOS/Android vs сервер

#### Документация
- 17 MD-файлов (520+ страниц): PROTOCOL, DATABASE, SECURITY, SETUP, TESTING
- Диаграммы архитектуры (Mermaid): потоки данных, таблицы БД
- Audit report: 21 CVE проверено и исправлено (A1-A2, B1-B6, C1-C8, E1-E5)
- Примеры интеграции: OAuth flow, push-уведомления, A2A-переговоры

### Security

#### Исправлены уязвимости (docs/AUDIT.md)
- **A1-A2:** Timing-safe сравнение паролей/токенов (secrets.compare_digest)
- **B1-B6:** Валидация JSON на сервере, санитизация SQL, CORS настраиваемый
- **C1-C8:** Rate limiting, защита от session fixation, secure cookie flags
- **E1-E5:** Проверка expired токенов, защита резервных кодов, audit log

#### Криптография
- Argon2id (time=12, memory=64MB, parallelism=4) — OWASP-рекомендации 2024
- AES-256-GCM для токенов интеграций и резервных кодов
- JWT сRotatingKeyManager (смена ключей каждые 90 дней)
- TOTP secret 160 бит (32 символа base32)

### Changed
- AI-сервис: `SELFVOY_AI_TOKEN` теперь обязателен в production (warning в dev)
- CORS: вместо `["*"]` используется `SELFVOY_CORS_ORIGINS` из env
- Протокол: поле `body` (не `text`) для сообщений чата — единообразие

### Fixed
- Protoco checkers: false-positive на Kotlin `Result` (sealed class vs kotlin.Result)
- QML: CJK-символы в комментариях (автоматический скан перед коммитом)
- Docker healthcheck: AI-сервис ждёт доступность PostgreSQL перед стартом

---

## Ссылки

- [Unreleased]: https://github.com/USER/selfvoy-main/compare/v1.0.0...HEAD
- [1.0.0]: https://github.com/USER/selfvoy-main/releases/tag/v1.0.0
