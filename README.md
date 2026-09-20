# Aura — кроссплатформенный персональный ИИ-агент

Aura — личный ИИ-агент, который живёт на ваших устройствах (desktop Qt и iOS)
и на вашем сервере. Вы говорите Ауре: *«Хочу встретиться с другом в эти
выходные, чтобы обсудить стартап»* — она связывается с Аурой друга
(Agent-to-Agent), находит пересечение в графиках, выбирает место под ваши
диеты и предлагает подтвердить бронирование. Опасные действия (письма,
сообщения, бронирования) Аура выполняет **только после вашего явного
подтверждения**.

## Состав

| компонент | технологии | назначение |
| --- | --- | --- |
| `server/` | C++17, WebSocket (RFC 6455), libpq | ядро: auth (Argon2id, JWT, TOTP 2FA), чаты, память, задачи, инструменты, интеграции Google, уведомления/push |
| `ai/` | Python 3.11, FastAPI | ИИ-сервис: агент (LLM), гибридный RAG памяти, планировщик, STT, A2A-переговоры |
| `client/` | Qt 6.5+, QML | desktop-клиент (Windows/macOS/Linux), адаптивный: стек или рельс + master-detail |
| `ios/` | Swift, SwiftUI, AuraKit (SPM) | нативный iOS-клиент: Siri/Команды, deep links, APNs-push |
| `schema/` | PostgreSQL 16 | 22 таблицы + миграции |
| `tools/` | Python | e2e-прогон, сверка протокола iOS ↔ сервер |

## Архитектура (кратко)

```
Qt-клиент / iOS-клиент
        │  WebSocket, JSON-конверт {id, type, payload}
        ▼
C++-сервер (auth, чаты, память, задачи, инструменты,
            интеграции, уведомления)
        │  HTTP /v1/... (X-Aura-Token)          ┌─► PostgreSQL (22 таблицы)
        ▼                                       │
Python AI Service (LLM, RAG, планировщик, STT) ─┘
```

Подробно — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Быстрый старт

```bash
make venv            # .venv + зависимости AI-сервиса
make schema-check    # схема PostgreSQL (PG_URI)
make ai-run          # AI-сервис :8000
make server-build    # C++-сервер
make server-run      # сервер :9000
make check           # тесты Python + C++
```

Пошагово (PostgreSQL, OAuth Google, push, 2FA, сборка iOS) —
[docs/SETUP.md](docs/SETUP.md).

## Тесты

| слой | объём | команда |
| --- | --- | --- |
| C++ unit | 55 тестов / 491 проверка | `make server-test` |
| Python AI | 83 теста | `make ai-test` |
| E2E (полный стек, PostgreSQL) | 137 проверок | `make e2e` (серверы запущены) |

Сценарии безопасности, мобильные чек-листы — [docs/TESTING.md](docs/TESTING.md).

## Документация

| документ | о чём |
| --- | --- |
| [ARCHITECTURE](docs/ARCHITECTURE.md) | слои, компоненты, потоки данных |
| [SETUP](docs/SETUP.md) | установка и настройка всего стека, .env |
| [PROTOCOL](docs/PROTOCOL.md) | WS-протокол (64 хендлера) + HTTP API AI-сервиса |
| [DATABASE](docs/DATABASE.md) | 22 таблицы, миграции |
| [SECURITY](docs/SECURITY.md) | crypto, 2FA, токены интеграций, push |
| [PRIVACY](docs/PRIVACY.md) | какие данные хранятся и кто их видит |
| [A2A_PROTOCOL](docs/A2A_PROTOCOL.md) | переговоры агентов |
| [MOBILE_FEATURES](docs/MOBILE_FEATURES.md) | Siri, Команды, deep links, Back Tap (честно) |
| [DESIGN](docs/DESIGN.md) / [DESIGN_SYSTEM](docs/DESIGN_SYSTEM.md) | дизайн full_mix, токены темы |
| [IOS](docs/IOS.md) | сборка и возможности iOS-приложения |
| [TESTING](docs/TESTING.md) | тесты и чек-листы |
| [TROUBLESHOOTING](docs/TROUBLESHOOTING.md) | частые проблемы |
| [AUDIT](docs/AUDIT.md) | аудит кода и исправления |
| [ROADMAP_V3](docs/ROADMAP_V3.md) | план и статус этапов |

## Статус

Этапы 1–10, 12–15 готовы; этап 11 (Android) отложен по решению владельца;
этап 16 (production-сборка) — следующий. Ограничения песочницы разработки
честно описаны в [ROADMAP_V3](docs/ROADMAP_V3.md).
