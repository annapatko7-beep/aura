# Aura — сеть ИИ-агентов (Agent-to-Agent Network)

Aura соединяет **ИИ-двойников** людей. Вы говорите своей Ауре:
*«Хочу встретиться с другом в эти выходные, чтобы обсудить стартап»* —
а она связывается с Аурой друга, находит пересечение в графиках, выбирает
кофейню на полпути под ваши диеты и бронирует столик.

Проект собран строго по скелету: **Qt-клиент (C++) → WebSocket → C++-сервер →
PostgreSQL**, и отдельно **Python AI Service** по HTTP.

---

## Архитектура

```
Qt Client (C++)                        WebSocket                     C++ Server
┌──────────────────────┐                                     ┌──────────────────────────────┐
│ Login / Register     │                                     │ Listener      ожидает подключения
│ Chats                │  ws://host:9000  JSON-конверт       │ Session       одно соединение
│ Settings             │◄───────────────────────────────────►│ ConnectionManager  все сессии
│ WebSocketClient      │                                     │ AuthManager   регистрация, JWT
└──────────────────────┘                                     │ ChatManager   чаты и история
                                                             │ AgentManager  запросы в Python AI
                            HTTP  /v1/agent/run              │ MemoryManager долговременная память
                                                             │ ToolManager   действия через API
                                                             │ DatabaseManager → PostgreSQL
                                                             └───────────────┬──────────────┘
                                                                             ▼
                                                              PostgreSQL: Users, Chats,
                                                              ChatMembers, Messages,
                                                              UserMemory, UserPreferences,
                                                              Sessions
                                                             ┌───────────────┴──────────────┐
                                                             │ Python AI Service            │
                                                             │  agent.py   запрос → JSON    │
                                                             │  planner.py слоты, напоминания
                                                             │  memory.py  долговременная память
                                                             │  tools.py   общие инструменты│
                                                             │  LLM                         │
                                                             └──────────────────────────────┘
```

### Где что лежит

| Модуль скелета | Файлы |
| --- | --- |
| Login / Register | `client/qml/LoginPage.qml` |
| Chats | `client/qml/ChatsPage.qml`, `client/qml/ChatPage.qml` |
| Settings | `client/qml/SettingsPage.qml` |
| WebSocketClient | `client/src/websocketclient.{h,cpp}` |
| — | `client/src/appstore.{h,cpp}` — состояние для QML (`App`) |
| Listener | `server/src/listener.cpp` |
| Session | `server/src/session.cpp` |
| ConnectionManager | `server/src/connectionmanager.cpp` |
| AuthManager | `server/src/authmanager.cpp` (PBKDF2 + JWT HS256) |
| ChatManager | `server/src/chatmanager.cpp` |
| AgentManager | `server/src/agentmanager.cpp` |
| MemoryManager | `server/src/memorymanager.cpp` |
| ToolManager | `server/src/toolmanager.cpp` |
| DatabaseManager | `server/src/databasemanager.cpp` + `database_pg.cpp` (libpq) / `database_embedded.cpp` (dev) |
| Users…Sessions | `schema/schema.sql` (7 таблиц), `schema/seed.sql` |
| agent.py / planner.py / memory.py / tools.py | `ai/aura_ai/*.py` |
| LLM | `ai/aura_ai/llm.py` (OpenAI-совместимый провайдер или локальные правила) |

Свои вспомогательные модули сервера (без внешних зависимостей):
`json`, `crypto` (SHA-1/SHA-256/HMAC/PBKDF2/Base64), `jwt`, `ws` (RFC 6455),
`net` (HTTP-клиент), `protocol`, `config`, `log`.

---

## Быстрый старт

### 1. Всё в контейнерах

```bash
docker compose up --build postgres ai server
# PostgreSQL: localhost:5432 (aura/aura), схема и демо-данные применяются сами
# AI-сервис:  http://localhost:8000  (Swagger: /docs, дизайн-превью: /)
# Сервер:     ws://localhost:9000
```

### 2. Вручную

```bash
# PostgreSQL
createdb aura && psql aura -f schema/schema.sql && psql aura -f schema/seed.sql

# Python AI Service
cd ai && python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
uvicorn aura_ai.app:app --host 0.0.0.0 --port 8000

# C++-сервер (нужен CMake 3.21+ и libpq-dev)
cd server && cmake -S . -B build && cmake --build build
AURA_DATABASE_URL=postgresql://localhost/aura \
AURA_AI_URL=http://127.0.0.1:8000 \
AURA_JWT_SECRET=dev-secret ./build/aura-server

# Qt-клиент (Qt 6.5+)
cd client && cmake -S . -B build && cmake --build build
AURA_SERVER_URL=ws://127.0.0.1:9000 ./build/aura-client
```

Демо-пользователи из `seed.sql`: `anna@example.com` / `anya@example.com`, пароль `aura1234`.

### Переменные окружения сервера

| Переменная | По умолчанию | Смысл |
| --- | --- | --- |
| `AURA_HOST` / `AURA_PORT` | `0.0.0.0` / `9000` | адрес WebSocket |
| `AURA_DATABASE_URL` | — | PostgreSQL DSN; без него — встроенное файловое хранилище |
| `AURA_AI_URL` | `http://127.0.0.1:8000` | Python AI Service |
| `AURA_JWT_SECRET` | `aura-dev-secret` | секрет подписи токенов |
| `AURA_TOKEN_TTL` | `604800` | время жизни токена, сек |
| `AURA_TOOLS_MODE` | `sandbox` | `sandbox` (детерминированные ответы) или `http` (реальные API) |
| `AURA_LOG_LEVEL` | `info` | `debug`/`info`/`warn`/`error` |

---

## Протокол (коротко)

```jsonc
// клиент → сервер
{"id":"req-1","type":"chat.send","payload":{"chat_id":3,"body":"Привет"}}
// сервер → клиент
{"id":"req-1","type":"ok","payload":{...}}
{"id":"req-1","type":"error","code":"forbidden","message":"вы не участник этого чата"}
// push-события
{"type":"event","event":"chat.message","payload":{...}}
```

Методы: `auth.register`, `auth.login`, `auth.token`, `auth.me`, `auth.logout`,
`chat.list`, `chat.open`, `chat.history`, `chat.members`, `chat.read`, `chat.send`,
`users.search`, `agent.ask`, `agent.negotiate`, `agent.status`,
`memory.list`, `memory.add`, `memory.extract`, `prefs.get`, `prefs.set`,
`tool.list`, `tool.run`, `ping`, `server.info`.

Подробности — в [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

---

## Дизайн

Тёмный графит и полупрозрачные «пузыри» в духе iOS: тонкая светлая рамка,
блик по верхнему краю, размытие фона, отклик нажатия лёгким уменьшением.
Все токены собраны в одном месте — `client/qml/theme/AuraTheme.qml`;
веб-превью (`preview/`) повторяет их один в один и открывается по `http://localhost:8000/`.
Описание палитры и правил — в [`docs/DESIGN.md`](docs/DESIGN.md).

---

## Проверки

```bash
cd ai      && python -m pytest              # 58 тестов AI-сервиса
cd server  && cmake -S . -B build && cmake --build build && ctest --test-dir build   # 22 теста C++
python tools/e2e.py                          # сквозной сценарий через WebSocket
psql "$AURA_DATABASE_URL" -f schema/schema.sql   # применимость схемы
```

---

## Лицензия

Учебный проект, MIT.
