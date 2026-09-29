# Архитектура Aura

Три процесса, два протокола, одна схема данных.

```
┌────────────────────┐   WebSocket (RFC 6455)   ┌──────────────────────────┐
│ Qt-клиент (desktop)│◄────────────────────────►│                          │
│ QML + AppStore     │   JSON {id,type,payload} │      C++-сервер          │
└────────────────────┘                          │  Listener → Session →    │
┌────────────────────┐                          │  ConnectionManager       │
│ iOS-клиент (Swift) │◄────────────────────────►│  AuthManager  ChatManager│
│ AuraKit (SPM)      │        тот же протокол   │  AgentManager Memory     │
└────────────────────┘                          │  ToolManager TaskManager │
                                                │  IntegrationManager      │
                                                │  NotificationsManager    │
                                                │  DatabaseManager         │
                                                └───────┬──────────┬───────┘
                                                        │ HTTP     │ SQL
                                                        ▼          ▼
                                              ┌──────────────┐  ┌────────────┐
                                              │ Python AI    │  │ PostgreSQL │
                                              │ Service :8000│  │ 22 таблицы │
                                              └──────────────┘  └────────────┘
```

## Слои

1. **Клиенты** — Qt/QML (desktop, адаптив: стек или рельс + master-detail) и
   нативный iOS (SwiftUI + AuraKit). Оба говорят одним WS-протоколом
   ([PROTOCOL.md](PROTOCOL.md)); состояние держат AppStore-объекты, сервер —
   источник истины.
2. **C++-сервер** — единственная точка авторизации и записи. Один поток на
   соединение + поток планировщика (`AURA_SCHEDULER_INTERVAL_MS`). Менеджеры
   не знают про сокеты: они принимают userId и возвращают результат,
   рассылкой событий занимается ConnectionManager (`deliverToUser`).
3. **Python AI Service** — LLM-агент, гибридный RAG памяти (BM25 + опц.
   эмбеддинги), планировщик слотов, STT, A2A-переговоры. Без состояния
   пользователя: контекст приходит в запросе от C++-сервера.
4. **PostgreSQL** — 22 таблицы ([DATABASE.md](DATABASE.md)). Для тестов и
   разработки есть встроенный JSON-бэкенд (тот же интерфейс `IDatabase`).

## Ключевые решения

- **Сервер валидирует всё.** LLM — не источник истины: JSON от модели
  проверяется по схеме, инструменты исполняются только через ToolManager с
  проверкой `tool_permissions`; опасные действия — через барьер подтверждения
  (`pending_actions` → `confirmation.approve`).
- **Событийная доставка.** Изменения приходят push-событиями WS
  (`chat.message`, `task.due`, `notification.new`), клиенты не поллят.
- **Push наружу — только через шлюз.** Ядро без TLS/HTTP2/ES256 (сознательно):
  APNs-конверт уходит POST-ом на `AURA_PUSH_WEBHOOK_URL`, шлюз подписывает
  JWT и форвардит в Apple. In-app уведомления (БД + WS) работают всегда.
- **Токены не покидают сервер.** Интеграции — AES-256-GCM в БД; push-токены
  не сериализуются; refresh — ротация, хранение хэшей; сессии — отзываемые.
- **Один протокол на все клиенты.** `tools/check_ios_protocol.py` сверяет
  типы сообщений Swift-кода с `registerHandler` сервера (64 хендлера).

## Потоки данных (примеры)

- **«Спросить Ауру»**: клиент → `agent.ask` → C++ собирает контекст памяти →
  POST `/v1/agent/run` → план/действия → ToolManager исполняет безопасное,
  опасное откладывает в `pending_actions` → ответ + события в чат.
- **A2A**: `agent.ask` c `peers` или `agent.negotiate` → POST
  `/v1/agent/negotiate` → пересечение слотов, место → `agent_reply` в чат +
  уведомление `a2a.proposal` собеседнику ([A2A_PROTOCOL.md](A2A_PROTOCOL.md)).
- **Напоминание**: `tasks.create` с `remind_at` → поток планировщика →
  событие `task.due` + уведомление + push-конверт на устройства.
- **Интеграция Google**: `integrations.begin` (PKCE S256) → consent в
  браузере → редирект (Qt: loopback 127.0.0.1; iOS: `aura://oauth`) →
  `integrations.callback` → токены в AES-256-GCM → `integrations.sync`.

## Сборка

- `cmake -S . -B build/server` — сервер (+тесты); `AURA_WITH_CLIENT=ON`
  добавляет Qt-клиент (нужен Qt ≥ 6.5: Core, Network, WebSockets, Quick,
  Qml, QuickControls2, Multimedia, TextToSpeech).
- `ios/` — XcodeGen (`project.yml`), общий слой AuraKit — SPM.
- `ai/` — FastAPI/uvicorn, зависимости в `ai/requirements.txt`.
