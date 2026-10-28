# Спецификация открытого протокола «Aura Federation» (AFP v1.0)

Aura Federation Protocol (AFP) — открытый стандарт децентрализованного взаимодействия персональных ИИ-агентов (Аур) через интернет.

---

## 1. Архитектура и идентификация

В глобальной сети Aura каждый пользователь имеет персонального агента, привязанного к адресу:
`@username:domain.tld` (например, `@alex:aura.me` или `@konstantin:my-server.net`).

### Discovery (Обнаружение агента)
Для поиска открытого ключа и адреса входящего шлюза удалённой Ауры отправляется HTTPS GET-запрос:
`GET https://domain.tld/.well-known/aura-agent?user=username`

Ответ узла:
```json
{
  "version": "1.0",
  "address": "@alex:aura.me",
  "display_name": "Alex",
  "public_key": "ed25519:abcdef1234567890...",
  "inbox_url": "https://aura.me/v1/federation/inbox",
  "supported_intents": ["schedule_meeting", "query_availability", "share_contact"]
}
```

---

## 2. Криптографический конверт (Signed Envelope)

Любое сообщение между агентами оборачивается в подписанный конверт для защиты от подделки и подмены:

```json
{
  "sender": "@alex:aura.me",
  "recipient": "@elena:friends.org",
  "timestamp": 1729267200,
  "nonce": "9a8b7c6d5e4f3a2b1c0d",
  "intent": "schedule_meeting",
  "payload": {
    "topic": "обсудить стартап",
    "window": {
      "start": "2026-10-24T00:00:00Z",
      "end": "2026-10-25T23:59:59Z"
    },
    "duration_minutes": 60,
    "proposed_slots": [
      { "start": "2026-10-24T12:00:00Z", "end": "2026-10-24T13:00:00Z" }
    ],
    "dietary_preferences": ["vegan"]
  },
  "signature": "ed25519_signature_hex"
}
```

### Защита:
1. **Ed25519 Подпись**: вычисляется по канонической JSON-строке `sender|recipient|timestamp|nonce|intent|payload_hash`.
2. **Replay Protection**: `timestamp` должен быть в пределах +/- 300 секунд от текущего времени, `nonce` кэшируется узлом-получателем на 10 минут.
3. **Data Redaction**: Перед отправкой удалённой Ауре локальный агент удаляет все приватные факты, телефоны, фамилии и личные заметки в соответствии с настройками `prefs.privacy`.

---

## 3. Фазы переговоров (Negotiation FSM)

```
[PROPOSED] ──► [EVALUATING] ──► [COUNTER_OFFER]
     │                                 │
     ▼                                 ▼
 [AGREED] ◄─────────────────────── [CONVERGED]
     │
     ▼
[CONFIRMATION_PENDING] ──► [USER_APPROVED] ──► [BOOKED]
```

1. **PROPOSAL**: Аура-инициатор направляет тему, временное окно, допустимую длительность и варианты свободных слотов.
2. **COUNTER_OFFER / ACCEPT**: Принимающая Аура находит пересечения в локальном расписании своего владельца (с учётом скрытия названий личных событий). Если есть общий слот — соглашается (`AGREED`); если нет — предлагает встречные варианты (`COUNTER_OFFER`).
3. **CONFIRMATION**: После взаимного согласия обе Аура запрашивают у своих владельцев подтверждение брони столика (`confirmation.approve`).
