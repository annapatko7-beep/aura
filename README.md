# Selfvoy — Персональный ИИ-агент

[![CI](https://github.com/USER/selfvoy/actions/workflows/ci.yml/badge.svg)](https://github.com/USER/selfvoy/actions/workflows/ci.yml)

Кроссплатформенное приложение с мультиагентной архитектурой: C++17 WebSocket-сервер + Python AI-сервис (LLM/RAG/память) + клиенты на Qt6/QML, iOS (SwiftUI), Android (Kotlin + Compose). База данных — PostgreSQL (22 таблицы).

## ✨ Возможности

- 🔐 **Безопасность:** Argon2id для паролей, 2FA (TOTP) с резервными кодами, JWT, OAuth 2.0 (Google), AES-256-GCM для токенов интеграций
- 🤖 **ИИ-агент:** LLM-оркестрация, долговременная память (факты, предпочтения, расписание, контакты), RAG-поиск, инструменты
- 🗣️ **Гибридный голос:** on-device распознавание (iOS) + серверный STT (Whisper-совместимый)
- 📱 **Мультиплатформа:** Desktop (Windows/macOS/Linux), iOS, Android
- 🔗 **Agent-to-Agent:** переговоры между агентами (до 5 раундов, затем эскалация)
- 🔔 **Push-уведомления:** APNs/FCM через webhook-шлюз, rate limiting, brute-force защита

## 🚀 Быстрый старт

```bash
git clone https://github.com/USER/selfvoy-main.git
cd selfvoy-main
cp .env.example .env
# Заполните в .env все секреты (каждый: openssl rand -hex 32):
# SELFVOY_JWT_SECRET, SELFVOY_2FA_KEY, SELFVOY_AI_TOKEN, POSTGRES_PASSWORD,
# SELFVOY_DB_PASSWORD, SELFVOY_AI_DB_PASSWORD — без них compose не стартует
make docker-up
# Откройте http://localhost:9000 или запустите клиент
```

Для локальной разработки (без Docker):

```bash
# 1. Применить схему БД (нужен PostgreSQL)
make schema-check

# 2. Запустить AI-сервис (Python)
make ai-run  # → http://127.0.0.1:8000 (SELFVOY_AI_INSECURE_DEV=1, только localhost)

# 3. Собрать и запустить C++-сервер
make server-build
make server-run  # → http://localhost:9000

# 4. Прогнать E2E-тесты (сервер + AI должны быть запущены)
make e2e
```

## 📚 Документация

- [Архитектура](docs/ARCHITECTURE.md) — диаграммы и потоки данных
- [Установка](docs/SETUP.md) — dev + production
- [Протокол](docs/PROTOCOL.md) — WebSocket API (клиент ↔ сервер)
- [База данных](docs/DATABASE.md) — схема 22 таблиц + миграции
- [Безопасность](docs/SECURITY.md) — криптография, 2FA, rate limiting
- [Тестирование](docs/TESTING.md) — 56 unit-тестов C++ + 83 Python + 141 E2E
- [Android](docs/ANDROID.md) — сборка APK, тестирование без SDK
- [Push Gateway](docs/PUSH_GATEWAY.md) — подписание JWT для APNs/FCM

## 🧪 Тестирование

```bash
make check           # pytest (AI) + ctest (C++) + protocol-check
make ai-test         # только Python AI-сервис
make server-test     # только C++ сервер
make protocol-check  # сверка типов сообщений iOS/Android с сервером
make e2e             # сквозной сценарий (141 проверка)
```

## 🏗️ Структура монорепо

```
selfvoy-main/
├── server/          C++17 сервер (WebSocket, Argon2id, JWT, TOTP, OAuth)
├── ai/              Python 3.11+ FastAPI (LLM, RAG, память, планировщик)
├── client/          Qt6/QML desktop-клиент (Windows/macOS/Linux)
├── android/         Kotlin + Compose (модули: selfvoykit + app)
├── ios/             SwiftUI-клиент
├── schema/          PostgreSQL схема + миграции
├── docs/            Документация (17 MD-файлов, 520+ страниц)
├── tools/           e2e.py (эталонный клиент протокола), check_*_protocol.py
├── preview/         HTML-превью дизайна (glassmorphism UI)
├── web/             Веб-клиент: React + Capacitor (APK/iOS) + Electron (Windows/macOS),
│                    тот же протокол, что у остальных клиентов (web/README.md)
└── .github/         CI/CD (tests, Windows .exe, Android .apk, iOS, web client)
```

## 🛠️ Технологии

**Backend:** C++17 (POSIX sockets, poll, nlohmann/json, Argon2), Python 3.11+, FastAPI, PostgreSQL 16  
**Frontend:** Qt6 (Core/Network/Qml/Quick/WebSockets/Multimedia/TTS), SwiftUI, Jetpack Compose  
**AI:** OpenAI API-совместимые LLM, BM25 + семантический поиск (RAG), Whisper-совместимый STT  
**Безопасность:** Argon2id, AES-256-GCM, JWT (HS256), TOTP (RFC 6238), OAuth 2.0  
**Push:** APNs/FCM через webhook-шлюз (TLS/HTTP2/ES256 подпись вне C++)

## 📄 Лицензия

MIT + Commons Clause — код открыт для просмотра, использования, модификаций, но **запрещено коммерческое использование другими** (SaaS, платные продукты на основе кода) без разрешения.

Подробнее: [LICENSE](LICENSE)

## 🤝 Вклад

Pull requests приветствуются. Для крупных изменений сначала откройте issue для обсуждения.

Перед коммитом обязательно:
- Прогнать `make check`
- Если менялся протокол — `make protocol-check`
- Если менялись серверные сценарии — `make e2e`

## 📞 Контакты

**GitHub:** [https://github.com/USER/selfvoy-main](https://github.com/USER/selfvoy-main)  
**Issues:** [https://github.com/USER/selfvoy-main/issues](https://github.com/USER/selfvoy-main/issues)

---

🤖 *Создано с [Claude Code](https://claude.com/claude-code)*
