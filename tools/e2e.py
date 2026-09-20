#!/usr/bin/env python3
"""Сквозная проверка Aura: Qt-клиент (здесь — WebSocket-клиент) → C++-сервер → AI-сервис.

Поднимать сервисы не нужно: скрипт подключается к уже запущенным
    * C++-серверу  (ws://127.0.0.1:9000)
    * AI-сервису   (http://127.0.0.1:8000)

Проходит путь пользователя:
    регистрация двух людей → чат → «Аура, хочу встретиться…» →
    переговоры Аур (A2A) → событие chat.message в чате → память и настройки.

Запуск:  python3 tools/e2e.py
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import os
import struct
import sys
import time
import uuid
from typing import Any, Dict

import websockets

SERVER_URL = os.environ.get("AURA_E2E_SERVER", "ws://127.0.0.1:9000")
RUN = uuid.uuid4().hex[:8]

failures: list[str] = []
checks = 0


def totp(secret_b32: str, window_offset: int = 0, period: int = 30, digits: int = 6) -> str:
    """TOTP (RFC 6238): HMAC-SHA1, base32-секрет, 6 цифр, 30-секундное окно.

    Совпадает с серверной реализацией aura::totp — позволяет e2e-клиенту
    генерировать коды как приложение-аутентификатор. window_offset=1 даёт код
    следующего окна (нужно из-за защиты от повторного использования кода).
    """
    counter = int(time.time()) // period + window_offset
    key = base64.b32decode(secret_b32 + "=" * (-len(secret_b32) % 8))
    digest = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
    offset = digest[-1] & 0x0F
    code = (struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7FFFFFFF) % (10 ** digits)
    return str(code).zfill(digits)


def check(condition: bool, label: str, detail: Any = "") -> bool:
    global checks
    checks += 1
    if condition:
        print(f"  ✓ {label}")
    else:
        failures.append(label)
        print(f"  ✗ {label} — {detail}")
    return condition


class Client:
    """Мини-аналог WebSocketClient из Qt-клиента.

    Один фоновый читатель раскладывает входящие сообщения: ответы на запросы —
    по id в очередь, push-события — в self.events (как сигналы Qt-модели).
    """

    def __init__(self, name: str):
        self.name = name
        self.ws: Any = None
        self.events: list[Dict[str, Any]] = []
        self._replies: Dict[str, asyncio.Queue] = {}
        self._reader: Any = None

    async def connect(self) -> None:
        self.ws = await websockets.connect(SERVER_URL, max_size=8 * 1024 * 1024)
        self._reader = asyncio.create_task(self._pump())

    async def _pump(self) -> None:
        try:
            async for raw in self.ws:
                message = json.loads(raw)
                if message.get("type") == "event":
                    self.events.append(message)
                    print(f"    [{self.name}] событие {message.get('event')}: "
                          f"{json.dumps(message.get('payload', {}), ensure_ascii=False)[:110]}")
                    continue
                queue = self._replies.get(message.get("id", ""))
                if queue is not None:
                    await queue.put(message)
        except websockets.ConnectionClosed:
            pass

    async def call(self, kind: str, payload: Dict[str, Any] | None = None, timeout: float = 25.0) -> Dict[str, Any]:
        request_id = f"{self.name}-{kind}-{uuid.uuid4().hex[:6]}"
        queue: asyncio.Queue = asyncio.Queue()
        self._replies[request_id] = queue
        try:
            await self.ws.send(json.dumps({"id": request_id, "type": kind, "payload": payload or {}},
                                          ensure_ascii=False))
            return await asyncio.wait_for(queue.get(), timeout=timeout)
        finally:
            self._replies.pop(request_id, None)

    async def close(self) -> None:
        if self._reader:
            self._reader.cancel()
        if self.ws:
            await self.ws.close()

    def events_of(self, name: str) -> list[Dict[str, Any]]:
        return [event for event in self.events if event.get("event") == name]


async def main() -> int:
    print(f"Aura E2E → {SERVER_URL} (прогон {RUN})\n")

    anna = Client("anna")
    anya = Client("anya")
    await anna.connect()
    await anya.connect()

    # ---------------------------------------------------------------- auth
    print("1. Регистрация, подтверждение email и вход")

    async def register_and_login(client, email: str, name: str) -> str:
        """Полный поток аккаунта: регистрация → код из ответа (mail dev) → вход."""
        registered = await client.call("auth.register", {
            "email": email,
            "password": "aura1234",
            "display_name": name,
        })
        check(registered.get("type") == "ok", f"регистрация {name}", registered)
        check(registered.get("payload", {}).get("requires_verification") is True,
              f"{name}: требуется подтверждение email")
        code = registered.get("payload", {}).get("email_code", "")
        check(len(code) == 6, f"{name}: выдан шестизначный код", code)
        verified = await client.call("auth.verifyEmail", {"email": email, "code": code})
        check(verified.get("payload", {}).get("verified") is True, f"{name}: email подтверждён")
        logged = await client.call("auth.login", {"email": email, "password": "aura1234"})
        check(logged.get("type") == "ok", f"вход {name}", logged)
        return logged.get("payload", {}).get("token", "")

    token = await register_and_login(anna, f"anna.{RUN}@example.com", "Анна")
    check(len(token.split(".")) == 3, "выдан JWT из трёх частей", token[:24])

    # Токен предъявляется заново — проверяет, что сессия записана в таблицу sessions
    fresh = Client("fresh")
    await fresh.connect()
    reauth = await fresh.call("auth.token", {"token": token})
    check(reauth.get("type") == "ok", "токен принят повторно (сессия есть в БД)", reauth)
    await fresh.close()

    # Управление сессиями: второй вход даёт вторую сессию; список, отзыв, проверка.
    extra = Client("extra")
    await extra.connect()
    second_login = await extra.call("auth.login", {"email": f"anna.{RUN}@example.com",
                                                   "password": "aura1234"})
    check(second_login.get("type") == "ok", "второй вход Анны", second_login)
    sessions = await anna.call("sessions.list", {})
    session_items = sessions.get("payload", {}).get("sessions", [])
    check(sessions.get("type") == "ok" and len(session_items) == 2,
          "sessions.list вернул две живые сессии", session_items)
    check(any(item.get("current") for item in session_items), "текущая сессия помечена")
    other_session = next((i["id"] for i in session_items if not i.get("current")), "")
    revoked = await anna.call("sessions.revoke", {"session_id": other_session})
    check(revoked.get("payload", {}).get("revoked") is True, "чужая сессия отозвана")
    probe = Client("probe")
    await probe.connect()
    recheck = await probe.call("auth.token",
                               {"token": second_login.get("payload", {}).get("token", "")})
    check(recheck.get("code") == "unauthorized", "токен отозванной сессии не проходит", recheck)
    await probe.close()
    await extra.close()

    await register_and_login(anya, f"anya.{RUN}@example.com", "Аня")

    # Настройки: у Ани своя диета и город — это нужно для выбора места
    await anya.call("prefs.set", {"diet": ["gluten_free"], "city": "Херлен",
                                  "lat": 50.888, "lon": 5.978})
    prefs = await anna.call("prefs.set", {"diet": ["vegan"], "city": "Керкраде", "budget_limit": 3,
                                          "lat": 50.861, "lon": 6.064,
                                          "preferred_hours": [10, 11, 12, 16, 17, 18]})
    check(prefs.get("payload", {}).get("city") == "Керкраде", "настройки Анны сохранены",
          prefs.get("payload"))

    # Память
    await anna.call("memory.add", {"text": "Люблю тихие кофейни", "kind": "preference"})
    memory = await anna.call("memory.list", {"query": "кофейни"})
    entries = memory.get("payload", {}).get("entries", [])
    check(any("кофейни" in entry.get("text", "") for entry in entries),
          "долговременная память читается", entries)

    # ----------------------------------------------------------------- чат
    print("\n2. Чат между пользователями")
    opened = await anna.call("chat.open", {"contact": f"anya.{RUN}@example.com"})
    chat_id = opened.get("payload", {}).get("chat", {}).get("id", 0)
    check(chat_id > 0, "чат создан", opened)

    reopened = await anna.call("chat.open", {"contact": f"anya.{RUN}@example.com"})
    check(reopened.get("payload", {}).get("chat", {}).get("id") == chat_id,
          "повторное открытие возвращает тот же чат", reopened.get("payload"))

    sent = await anna.call("chat.send", {"chat_id": chat_id, "body": "Привет! Есть идея стартапа."})
    check(sent.get("type") == "ok", "сообщение отправлено", sent)
    await asyncio.sleep(0.4)
    check(len(anya.events_of("chat.message")) >= 1, "Аня получила событие chat.message")

    history = await anya.call("chat.history", {"chat_id": chat_id})
    bodies = [m.get("body", "") for m in history.get("payload", {}).get("messages", [])]
    check(any("идея стартапа" in body for body in bodies), "история содержит сообщение", bodies)

    # ---------------------------------------------------------------- агент
    print("\n3. Запрос к Ауре (C++-сервер → Python AI Service)")
    status = await anna.call("agent.status")
    check(status.get("payload", {}).get("available") is True, "AI-сервис доступен",
          status.get("payload"))

    ask = await anna.call("agent.ask", {
        "chat_id": chat_id,
        "message": "Хочу встретиться с Аней в эти выходные, чтобы обсудить стартап",
        "peers": [{"email": f"anya.{RUN}@example.com"}],
    }, timeout=40)
    check(ask.get("type") == "ok", "Аура ответила", ask)
    payload = ask.get("payload", {})
    print("    ответ Ауры:", json.dumps(payload, ensure_ascii=False)[:400])
    check(payload.get("intent") == "schedule_meeting", "намерение определено как встреча",
          payload.get("intent"))
    check(bool(payload.get("actions")), "AI-сервис вернул действия", payload.get("actions"))
    results = payload.get("results", [])
    # Этап 8: безопасные действия исполняются сразу, опасные (send_message)
    # уходят в барьер подтверждения (requires_confirmation), а не исполняются.
    executed = [r for r in results if r.get("ok")]
    pending_confirm = [r for r in results if r.get("requires_confirmation")]
    hard_failed = [r for r in results
                   if not r.get("ok") and not r.get("requires_confirmation") and not r.get("denied")]
    check(bool(results) and not hard_failed,
          "действия исполнены или ждут подтверждения (без ошибок)", hard_failed or results)
    check(bool(executed), "безопасные действия исполнены сразу", executed)
    check(bool(pending_confirm),
          "опасное действие (send_message) требует подтверждения", pending_confirm)
    confirmation_id = pending_confirm[0].get("confirmation_id") if pending_confirm else None
    check(confirmation_id is not None, "выдан confirmation_id", pending_confirm)

    a2a = payload.get("a2a") or {}
    check(bool(a2a), "проведены переговоры Agent-to-Agent", payload.keys())
    if a2a:
        check(a2a.get("accepted") is True or bool(a2a.get("counter_slots")),
              "A2A вернул слот или встречное предложение", a2a)
        if a2a.get("accepted"):
            place = a2a.get("place") or {}
            check(bool(place.get("name")), "выбрано место", place)
            print(f"    договорились: {a2a.get('slot', {}).get('start')} — {place.get('name')}")

    await asyncio.sleep(0.5)
    agent_messages = anya.events_of("chat.message")
    check(len(agent_messages) >= 2, "результат Ауры опубликован в чате",
          len(agent_messages))

    # ------------------------------------- разрешения и барьер подтверждения
    print("\n3c. Разрешения и барьер подтверждения (этап 8)")

    # permissions.list: каталог с флагами опасности и эффективными режимами.
    perms = await anna.call("permissions.list")
    perm_items = perms.get("payload", {}).get("tools", [])
    check(perms.get("type") == "ok" and len(perm_items) >= 8,
          "permissions.list вернул каталог инструментов", perms.get("payload"))
    email_perm = next((p for p in perm_items if p.get("tool") == "send_email"), {})
    check(email_perm.get("dangerous") is True and email_perm.get("default_mode") == "ask",
          "send_email помечен опасным (default ask)", email_perm)
    cafe_perm = next((p for p in perm_items if p.get("tool") == "find_cafe"), {})
    check(cafe_perm.get("dangerous") is False and cafe_perm.get("default_mode") == "allow",
          "find_cafe безопасен (default allow)", cafe_perm)

    # confirmation.list: действие из agent.ask уже ожидает подтверждения.
    confirms = await anna.call("confirmation.list", {"status": "pending"})
    pending_items = confirms.get("payload", {}).get("actions", [])
    check(confirms.get("type") == "ok" and len(pending_items) >= 1,
          "confirmation.list показал отложенное действие", confirms.get("payload"))

    # Подтверждаем реальное действие, созданное агентом, — сервер исполняет его.
    if confirmation_id is not None:
        approved = await anna.call("confirmation.approve", {"id": confirmation_id})
        check(approved.get("type") == "ok"
              and approved.get("payload", {}).get("status") == "executed",
              "confirmation.approve исполнил действие", approved.get("payload"))
        # Повторное подтверждение уже обработанного — ошибка.
        again = await anna.call("confirmation.approve", {"id": confirmation_id})
        check(again.get("code") == "bad_request",
              "повторное подтверждение отклонено", again)

    # permissions.set: переводим send_email в deny и проверяем валидацию.
    set_perm = await anna.call("permissions.set", {"tool": "send_email", "mode": "deny"})
    check(set_perm.get("type") == "ok" and set_perm.get("payload", {}).get("mode") == "deny",
          "permissions.set применил режим deny", set_perm.get("payload"))
    bad_mode = await anna.call("permissions.set", {"tool": "send_email", "mode": "maybe"})
    check(bad_mode.get("code") == "bad_request", "недопустимый режим отклонён", bad_mode)
    bad_tool = await anna.call("permissions.set", {"tool": "launch_missiles", "mode": "allow"})
    check(bad_tool.get("code") == "bad_request", "неизвестный инструмент отклонён", bad_tool)

    # Режим deny: send_email уже переведён в deny — намерение «письмо»
    # отклоняется сразу, без исполнения и без отложенного действия.
    deny_ask = await anna.call("agent.ask", {
        "message": "Отправь письмо team@aura.io с темой встреча",
    }, timeout=40)
    deny_results = deny_ask.get("payload", {}).get("results", [])
    check(any(r.get("denied") for r in deny_results),
          "send_email в режиме deny отклонён без исполнения", deny_results)

    # Режим ask: возвращаем send_email в ask — намерение уходит в подтверждение,
    # затем отклоняем его через confirmation.deny.
    await anna.call("permissions.set", {"tool": "send_email", "mode": "ask"})
    ask_again = await anna.call("agent.ask", {
        "message": "Отправь письмо team@aura.io с темой встреча",
    }, timeout=40)
    ask_results = ask_again.get("payload", {}).get("results", [])
    email_pending = [r for r in ask_results if r.get("requires_confirmation")]
    check(bool(email_pending), "намерение «письмо» требует подтверждения", ask_results)
    if email_pending:
        denied = await anna.call("confirmation.deny", {"id": email_pending[0].get("confirmation_id")})
        check(denied.get("payload", {}).get("status") == "denied",
              "confirmation.deny отклонил действие", denied.get("payload"))

    # ------------------------------------------- задачи и напоминания (этап 8)
    print("\n3d. Задачи, напоминания и планировщик (этап 8)")

    task_new = await anna.call("tasks.create", {
        "title": "Подготовить презентацию",
        "notes": "к пятнице",
        "priority": 2,
    })
    check(task_new.get("type") == "ok", "tasks.create создал задачу", task_new.get("payload"))
    task_id = task_new.get("payload", {}).get("id", 0)
    check(task_id > 0 and task_new.get("payload", {}).get("status") == "pending",
          "задача в статусе pending", task_new.get("payload"))

    tasks_list = await anna.call("tasks.list", {})
    check(any(t.get("id") == task_id for t in tasks_list.get("payload", {}).get("tasks", [])),
          "tasks.list содержит задачу", tasks_list.get("payload"))

    done = await anna.call("tasks.complete", {"id": task_id})
    check(done.get("payload", {}).get("status") == "done",
          "tasks.complete отметил задачу выполненной", done.get("payload"))

    # Напоминание с прошедшим сроком → планировщик отправляет уведомление.
    await anna.call("tasks.create", {"title": "Позвонить клиенту",
                                     "remind_at": "2020-01-01T00:00:00.000Z"})
    due = await anna.call("tasks.due", {})
    check(due.get("payload", {}).get("sent", 0) >= 1,
          "планировщик отправил наступившее напоминание", due.get("payload"))
    await asyncio.sleep(0.3)
    check(len(anna.events_of("task.due")) >= 1, "клиент получил событие task.due")

    # create_reminder через инструмент тоже создаёт задачу.
    remind = await anna.call("tool.run", {
        "tool": "create_reminder",
        "args": {"text": "выпить воды", "at": "2030-01-01T10:00:00.000Z"},
    })
    check(remind.get("payload", {}).get("task_id", 0) > 0,
          "create_reminder создал задачу", remind.get("payload"))

    # ------------------------------------------------------------ речь (STT)
    print("\n3b. Распознавание речи (C++-сервер → Python AI Service)")

    # Минимальный WAV (16 кГц, моно, 16 бит, 0.1 c тишины). Mock-STT не
    # распознаёт по-настоящему, но путь аудио через сервер и AI-сервис
    # (WS speech.transcribe → POST /v1/speech/transcribe) проверяется целиком.
    def tiny_wav() -> bytes:
        pcm = b"\x00\x00" * 1600
        data_size = len(pcm)
        return (b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE"
                + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
                + b"data" + struct.pack("<I", data_size) + pcm)

    audio_b64 = base64.b64encode(tiny_wav()).decode()
    stt = await anna.call("speech.transcribe", {"audio": audio_b64, "language": "ru"})
    check(stt.get("type") == "ok", "speech.transcribe вернул ok", stt)
    stt_payload = stt.get("payload", {})
    check(bool(stt_payload.get("text")), "STT вернул непустой текст", stt_payload)
    check(stt_payload.get("provider") == "mock",
          "STT-провайдер — mock (без AURA_STT_URL)", stt_payload.get("provider"))
    check(stt_payload.get("language") == "ru", "STT вернул язык запроса",
          stt_payload.get("language"))

    # Пустое аудио отклоняется до обращения к бэкенду.
    empty_stt = await anna.call("speech.transcribe", {"audio": ""})
    check(empty_stt.get("code") == "bad_request", "STT: пустое аудио отклонено", empty_stt)

    # ----------------------------------------------------------------- 2FA
    print("\n4. Двухфакторная аутентификация (TOTP, RFC 6238)")
    vera = Client("vera")
    await vera.connect()
    vera_email = f"vera.{RUN}@example.com"

    # Регистрация → подтверждение → вход (2FA ещё не настроена).
    await register_and_login(vera, vera_email, "Вера")

    # setup2fa: сервер выдаёт base32-секрет и otpauth:// URI.
    setup = await vera.call("auth.setup2fa", {})
    setup_payload = setup.get("payload", {})
    secret = setup_payload.get("secret", "")
    otpauth = setup_payload.get("otpauth_uri", "")
    check(setup.get("type") == "ok" and len(secret) >= 16, "2FA: setup2fa вернул секрет",
          setup_payload)
    check(otpauth.startswith("otpauth://totp/"), "2FA: otpauth URI корректен", otpauth)
    check(f"secret={secret}" in otpauth, "2FA: секрет присутствует в otpauth URI", otpauth)
    check(setup_payload.get("digits") == 6 and setup_payload.get("period") == 30
          and setup_payload.get("algorithm") == "SHA1",
          "2FA: параметры TOTP (SHA1/6 цифр/30 с)", setup_payload)

    # До подтверждения 2FA в статусе pending и не включена.
    status_pending = await vera.call("auth.status2fa", {})
    check(status_pending.get("payload", {}).get("pending") is True,
          "2FA: статус pending до подтверждения", status_pending.get("payload"))
    check(status_pending.get("payload", {}).get("enabled") is False,
          "2FA: ещё не включена", status_pending.get("payload"))

    # confirm2fa текущим кодом → включается и выдаёт 10 резервных кодов.
    confirm = await vera.call("auth.confirm2fa", {"code": totp(secret)})
    recovery = confirm.get("payload", {}).get("recovery_codes", [])
    check(confirm.get("type") == "ok", "2FA: confirm2fa принял код", confirm)
    check(len(recovery) == 10, "2FA: выдано 10 резервных кодов", len(recovery))
    check(all(len(c) == 11 and c[5] == "-" for c in recovery),
          "2FA: формат резервных кодов XXXXX-XXXXX", recovery[:2])

    status_on = await vera.call("auth.status2fa", {})
    check(status_on.get("payload", {}).get("enabled") is True,
          "2FA: включена после подтверждения", status_on.get("payload"))
    check(status_on.get("payload", {}).get("recovery_codes_left") == 10,
          "2FA: 10 резервных кодов доступно", status_on.get("payload"))

    # Вход без кода (и без доверенного устройства) → requires_2fa.
    gate = Client("vera-gate")
    await gate.connect()
    blocked = await gate.call("auth.login", {"email": vera_email, "password": "aura1234"})
    check(blocked.get("code") == "requires_2fa",
          "2FA: вход без кода отклонён (requires_2fa)", blocked)
    await gate.close()

    # Вход по резервному коду; повторное использование того же кода отклоняется.
    first_code = recovery[0]
    rc_client = Client("vera-rc")
    await rc_client.connect()
    rc_login = await rc_client.call("auth.login2fa",
                                    {"email": vera_email, "password": "aura1234", "code": first_code})
    check(rc_login.get("type") == "ok", "2FA: вход по резервному коду", rc_login)
    check(len(rc_login.get("payload", {}).get("token", "").split(".")) == 3,
          "2FA: по резервному коду выдан JWT")
    rc_reuse = await rc_client.call("auth.login2fa",
                                    {"email": vera_email, "password": "aura1234", "code": first_code})
    check(rc_reuse.get("code") == "unauthorized",
          "2FA: повторное использование резервного кода отклонено", rc_reuse)
    await rc_client.close()

    status_after_rc = await vera.call("auth.status2fa", {})
    check(status_after_rc.get("payload", {}).get("recovery_codes_left") == 9,
          "2FA: осталось 9 резервных кодов", status_after_rc.get("payload"))

    # Вход по TOTP-коду (следующее окно — защита от повтора) + доверенное устройство.
    device_id = f"vera-device-{RUN}"
    totp_client = Client("vera-totp")
    await totp_client.connect()
    totp_login = await totp_client.call("auth.login2fa", {
        "email": vera_email, "password": "aura1234",
        "code": totp(secret, window_offset=1),
        "trust_device": True, "device_id": device_id,
    })
    check(totp_login.get("type") == "ok", "2FA: вход по TOTP-коду", totp_login)
    await totp_client.close()

    # Вход с доверенного устройства — код не требуется.
    trust_client = Client("vera-trust")
    await trust_client.connect()
    trust_login = await trust_client.call("auth.login",
                                          {"email": vera_email, "password": "aura1234",
                                           "device_id": device_id})
    check(trust_login.get("type") == "ok", "2FA: вход с доверенного устройства без кода",
          trust_login)
    await trust_client.close()

    # Список доверенных устройств и отзыв.
    devices = await vera.call("devices.list", {})
    device_items = devices.get("payload", {}).get("devices", [])
    check(len(device_items) >= 1, "2FA: доверенное устройство в списке", device_items)
    dev_id = device_items[0].get("id", "") if device_items else ""
    check(bool(dev_id), "2FA: у доверенного устройства есть id", device_items)
    revoke = await vera.call("devices.revoke", {"id": dev_id})
    check(revoke.get("type") == "ok", "2FA: доверенное устройство отозвано", revoke)

    # После отзыва то же устройство снова требует код.
    revoked_gate = Client("vera-revoked")
    await revoked_gate.connect()
    revoked_login = await revoked_gate.call("auth.login",
                                            {"email": vera_email, "password": "aura1234",
                                             "device_id": device_id})
    check(revoked_login.get("code") == "requires_2fa",
          "2FA: после отзыва устройства снова нужен код", revoked_login)
    await revoked_gate.close()

    # Отключение 2FA: неверный пароль отклоняется, верный — отключает.
    bad_disable = await vera.call("auth.disable2fa", {"password": "wrongpass1"})
    check(bad_disable.get("code") == "unauthorized",
          "2FA: отключение с неверным паролем отклонено", bad_disable)
    good_disable = await vera.call("auth.disable2fa", {"password": "aura1234"})
    check(good_disable.get("type") == "ok", "2FA: отключение по паролю", good_disable)
    status_off = await vera.call("auth.status2fa", {})
    check(status_off.get("payload", {}).get("enabled") is False,
          "2FA: отключена после подтверждения паролем", status_off.get("payload"))
    await vera.close()

    # -------------------------------------------------------------- итоги
    print("\n5. Состояние сервера")
    info = await anna.call("server.info")
    server_payload = info.get("payload", {})
    check(server_payload.get("status") == "ok", "healthcheck сервера", server_payload.get("status"))
    check("postgresql" in server_payload.get("database", ""), "сервер работает с PostgreSQL",
          server_payload.get("database"))
    check(server_payload.get("connections", {}).get("connections", 0) >= 2,
          "ConnectionManager видит обе сессии", server_payload.get("connections"))

    await anna.close()
    await anya.close()

    print(f"\nпроверок: {checks}, ошибок: {len(failures)}")
    if failures:
        for failure in failures:
            print("  ✗", failure)
        return 1
    print("E2E: OK")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
