#!/usr/bin/env python3
"""Сквозная проверка Selfvoy: Qt-клиент (здесь — WebSocket-клиент) → C++-сервер → AI-сервис.

Поднимать сервисы не нужно: скрипт подключается к уже запущенным
    * C++-серверу  (ws://127.0.0.1:9000)
    * AI-сервису   (http://127.0.0.1:8000)

Проходит путь пользователя:
    регистрация двух людей → чат → «Selfvoy, хочу встретиться…» →
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
import re
import struct
import sys
import time
import urllib.error
import urllib.request
import uuid
from datetime import datetime
from typing import Any, Dict

import websockets

SERVER_URL = os.environ.get("SELFVOY_E2E_SERVER", "ws://127.0.0.1:9000")
RUN = uuid.uuid4().hex[:8]

failures: list[str] = []
checks = 0


def totp(secret_b32: str, window_offset: int = 0, period: int = 30, digits: int = 6) -> str:
    """TOTP (RFC 6238): HMAC-SHA1, base32-секрет, 6 цифр, 30-секундное окно.

    Совпадает с серверной реализацией selfvoy::totp — позволяет e2e-клиенту
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

    def __init__(self, name: str, source: str | None = None):
        self.name = name
        self.source = source  # локальный адрес соединения (другой адрес петли — свои лимиты по IP)
        self.ws: Any = None
        self.events: list[Dict[str, Any]] = []
        self._replies: Dict[str, asyncio.Queue] = {}
        self._reader: Any = None

    async def connect(self) -> None:
        if self.source and "://127.0.0.1:" in SERVER_URL:
            try:
                self.ws = await websockets.connect(SERVER_URL, max_size=8 * 1024 * 1024,
                                                   local_addr=(self.source, 0))
            except OSError:
                self.ws = None  # адрес недоступен — соединяемся как остальные клиенты
        if self.ws is None:
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


class GoogleMock:
    """Mock эндпоинтов Google OAuth/API для e2e (этап 9).

    Поднимается только при SELFVOY_E2E_GOOGLE_MOCK=1; C++-сервер должен быть
    запущен с SELFVOY_GOOGLE_* на этот порт (net.h не делает TLS — mock по http).
    Проверяет PKCE: code_challenge из /auth сверяется с SHA256(code_verifier).
    Первый access_token календаря («access-cal-1») на /events отвечает 401 —
    так проверяется авто-refresh по refresh_token.
    """

    def __init__(self, port: int):
        self.port = port
        self.codes: Dict[str, Dict[str, str]] = {}
        self.refreshed = 0
        self.revoked_tokens: list[str] = []
        self.sent_raw: list[str] = []
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", self.port)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    def _id_token(self, email: str) -> str:
        header = base64.urlsafe_b64encode(b'{"alg":"none","typ":"JWT"}').rstrip(b"=").decode()
        payload = base64.urlsafe_b64encode(
            json.dumps({"email": email, "sub": "e2e-user"}).encode()).rstrip(b"=").decode()
        return f"{header}.{payload}.sig"

    def _route(self, method: str, target: str, headers: Dict[str, str],
               body: str) -> tuple[int, str, Dict[str, str]]:
        from urllib.parse import parse_qs, urlparse
        url = urlparse(target)
        query = parse_qs(url.query)
        if url.path == "/o/oauth2/v2/auth":
            # «Consent-экран»: сразу выдаём code и редиректим на redirect_uri.
            code = "code-" + uuid.uuid4().hex[:12]
            self.codes[code] = {
                "challenge": query.get("code_challenge", [""])[0],
                "redirect_uri": query.get("redirect_uri", [""])[0],
                "scope": query.get("scope", [""])[0],
            }
            state = query.get("state", [""])[0]
            redirect = query.get("redirect_uri", [""])[0]
            location = f"{redirect}{'&' if '?' in redirect else '?'}code={code}&state={state}"
            return 302, "", {"Location": location}
        if url.path == "/token":
            form = parse_qs(body)
            grant = form.get("grant_type", [""])[0]
            if grant == "authorization_code":
                stored = self.codes.pop(form.get("code", [""])[0], None)
                if stored is None:
                    return 400, json.dumps({"error": "invalid_grant"}), {}
                verifier = form.get("code_verifier", [""])[0]
                expected = base64.urlsafe_b64encode(
                    hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
                if expected != stored["challenge"]:
                    return 400, json.dumps(
                        {"error": "invalid_grant", "error_description": "pkce mismatch"}), {}
                access = "access-gmail-1" if "gmail" in stored["scope"] else "access-cal-1"
                return 200, json.dumps({
                    "access_token": access,
                    "refresh_token": "refresh-e2e",
                    "expires_in": 3600,
                    "id_token": self._id_token("e2e-user@gmail.com"),
                    "scope": stored["scope"],
                    "token_type": "Bearer",
                }), {}
            if grant == "refresh_token":
                self.refreshed += 1
                return 200, json.dumps({
                    "access_token": f"access-refreshed-{self.refreshed}",
                    "expires_in": 3600,
                    "token_type": "Bearer",
                }), {}
            return 400, json.dumps({"error": "unsupported_grant_type"}), {}
        if url.path == "/revoke":
            form = parse_qs(body)
            self.revoked_tokens.append(form.get("token", [""])[0])
            return 200, "", {}
        if url.path == "/calendar/v3/calendars/primary/events":
            if headers.get("authorization", "") != "Bearer access-refreshed-1":
                # Первичный токен «истёк» — сервер должен обновить его и повторить.
                return 401, json.dumps({"error": {"message": "token expired"}}), {}
            return 200, json.dumps({"items": [
                {"summary": "Планёрка", "start": {"dateTime": "2030-01-02T10:00:00Z"}},
                {"summary": "Врач", "start": {"date": "2030-01-03"}},
            ]}), {}
        if url.path == "/gmail/v1/users/me/profile":
            return 200, json.dumps({"emailAddress": "e2e-user@gmail.com"}), {}
        if url.path == "/gmail/v1/users/me/messages/send":
            try:
                raw = json.loads(body).get("raw", "")
            except json.JSONDecodeError:
                raw = ""
            self.sent_raw.append(raw)
            if not raw:
                return 400, json.dumps({"error": {"message": "empty raw"}}), {}
            return 200, json.dumps({"id": "msg-e2e", "threadId": "thr-e2e"}), {}
        return 404, json.dumps({"error": {"message": "not found"}}), {}

    async def _handle(self, reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        try:
            request_line = (await reader.readline()).decode("latin-1").strip()
            if not request_line:
                return
            headers: Dict[str, str] = {}
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, _, value = line.decode("latin-1").partition(":")
                headers[key.strip().lower()] = value.strip()
            body = b""
            if headers.get("content-length"):
                body = await reader.readexactly(int(headers["content-length"]))
            parts = request_line.split(" ")
            method = parts[0]
            target = parts[1] if len(parts) > 1 else "/"
            status, payload, extra = self._route(method, target, headers,
                                                 body.decode("utf-8", "replace"))
            payload_bytes = payload.encode("utf-8")
            head = f"HTTP/1.1 {status} {'OK' if status == 200 else 'Result'}\r\n"
            head += f"Content-Length: {len(payload_bytes)}\r\nConnection: close\r\n"
            for key, value in extra.items():
                head += f"{key}: {value}\r\n"
            head += "\r\n"  # пустая строка: конец заголовков (строгий парсер net.h)
            writer.write(head.encode("latin-1") + payload_bytes)
            await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


class PushMock:
    """Приёмник push-конвертов (этап 13).

    Поднимается при SELFVOY_E2E_PUSH_MOCK=1; C++-сервер должен быть запущен с
    SELFVOY_PUSH_DRIVER=webhook и SELFVOY_PUSH_WEBHOOK_URL=http://127.0.0.1:<порт>/push.
    Сохраняет JSON-тела POST /push — так проверяется веерная рассылка
    (apns-конверт с токеном, kind и aps.alert).
    """

    def __init__(self, port: int):
        self.port = port
        self.envelopes: list[Dict[str, Any]] = []
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", self.port)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def _handle(self, reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        try:
            request_line = (await reader.readline()).decode("latin-1").strip()
            if not request_line:
                return
            headers: Dict[str, str] = {}
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, _, value = line.decode("latin-1").partition(":")
                headers[key.strip().lower()] = value.strip()
            body = b""
            if headers.get("content-length"):
                body = await reader.readexactly(int(headers["content-length"]))
            parts = request_line.split(" ")
            target = parts[1] if len(parts) > 1 else "/"
            if parts[0] == "POST" and target == "/push":
                try:
                    self.envelopes.append(json.loads(body.decode("utf-8")))
                except json.JSONDecodeError:
                    pass
            payload = b"{}"
            head = ("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n")
            writer.write(head.encode("latin-1") + payload)
            await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


class TelegramMock:
    """Мок Telegram Bot API (F2).

    Поднимается при SELFVOY_E2E_TELEGRAM_MOCK=1; C++-сервер должен быть запущен с
    SELFVOY_TELEGRAM_BOT_TOKEN=e2e:token, SELFVOY_TELEGRAM_BOT_USERNAME=selfvoy_e2e_bot и
    SELFVOY_TELEGRAM_API_URL=http://127.0.0.1:<порт>. getUpdates отдаёт поставленные
    тестом апдейты (long polling: ответ сразу, как только апдейт появился); sendMessage,
    answerCallbackQuery и editMessageReplyMarkup записываются. Чаты из blocked получают
    403 (бот заблокирован).
    """

    TOKEN = "e2e:token"

    def __init__(self, port: int):
        self.port = port
        self.sent: list[Dict[str, Any]] = []
        self.blocked: set[int] = set()
        self.bad_token: list[str] = []
        self.polls = 0
        self._updates: list[Dict[str, Any]] = []
        self._next_id = 1
        self._event = asyncio.Event()
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", self.port)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    def push(self, update: Dict[str, Any]) -> None:
        update = dict(update, update_id=self._next_id)
        self._next_id += 1
        self._updates.append(update)
        self._event.set()

    def text(self, chat_id: int, text: str, username: str = "e2e_user", first_name: str = "") -> None:
        sender: Dict[str, Any] = {"id": chat_id, "is_bot": False, "username": username}
        if first_name:
            sender["first_name"] = first_name
        self.push({"message": {"message_id": self._next_id, "date": int(time.time()), "text": text,
                               "chat": {"id": chat_id, "type": "private"}, "from": sender}})

    def callback(self, chat_id: int, data: str, message_id: int = 1) -> None:
        self.push({"callback_query": {"id": f"cb{self._next_id}", "data": data,
                                      "from": {"id": chat_id, "is_bot": False},
                                      "message": {"message_id": message_id,
                                                  "chat": {"id": chat_id, "type": "private"}}}})

    def calls(self, method: str, chat_id: int | None = None) -> list[Dict[str, Any]]:
        return [c["body"] for c in self.sent if c["method"] == method
                and (chat_id is None or c["body"].get("chat_id") == chat_id)]

    async def wait_for(self, method: str, chat_id: int | None, after: int, timeout: float = 15.0,
                       contains: str = "") -> Dict[str, Any] | None:
        """Первый вызов method (в чат chat_id) с номером ≥ after, текст которого содержит contains."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, call in enumerate(self.sent):
                body = call["body"]
                if index < after or call["method"] != method:
                    continue
                if chat_id is not None and body.get("chat_id") != chat_id:
                    continue
                if contains and contains not in body.get("text", ""):
                    continue
                return body
            await asyncio.sleep(0.1)
        return None

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request_line = (await reader.readline()).decode("latin-1").strip()
            if not request_line:
                return
            headers: Dict[str, str] = {}
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, _, value = line.decode("latin-1").partition(":")
                headers[key.strip().lower()] = value.strip()
            raw = b""
            if headers.get("content-length"):
                raw = await reader.readexactly(int(headers["content-length"]))
            try:
                body = json.loads(raw.decode("utf-8")) if raw else {}
            except json.JSONDecodeError:
                body = {}
            target = request_line.split(" ")[1] if " " in request_line else "/"
            prefix = f"/bot{self.TOKEN}/"
            status, result = 200, {"ok": True, "result": True}
            if not target.startswith(prefix):
                self.bad_token.append(target)
                status, result = 401, {"ok": False, "error_code": 401, "description": "Unauthorized"}
            else:
                method = target[len(prefix):]
                if method == "getUpdates":
                    self.polls += 1
                    offset = int(body.get("offset", 0) or 0)
                    self._updates = [u for u in self._updates if u["update_id"] >= offset]
                    if not self._updates:
                        self._event.clear()
                        try:
                            await asyncio.wait_for(self._event.wait(),
                                                   timeout=min(float(body.get("timeout", 0) or 0), 25.0))
                        except asyncio.TimeoutError:
                            pass
                    result = {"ok": True, "result": [u for u in self._updates if u["update_id"] >= offset]}
                else:
                    self.sent.append({"method": method, "body": body})
                    if method == "sendMessage" and body.get("chat_id") in self.blocked:
                        status, result = 403, {"ok": False, "error_code": 403,
                                               "description": "Forbidden: bot was blocked by the user"}
                    elif method == "sendMessage":
                        result = {"ok": True, "result": {"message_id": len(self.sent),
                                                         "chat": {"id": body.get("chat_id")}}}
            payload = json.dumps(result).encode("utf-8")
            reason = {200: "OK", 401: "Unauthorized", 403: "Forbidden"}.get(status, "OK")
            head = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n"
                    f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n")
            writer.write(head.encode("latin-1") + payload)
            await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError, BrokenPipeError):
            pass
        finally:
            writer.close()


class YooKassaMock:
    """Мок API ЮKassa (F4).

    Поднимается при SELFVOY_E2E_YOOKASSA_MOCK=1; C++-сервер должен быть запущен с
    SELFVOY_YOOKASSA_API_URL=http://127.0.0.1:<порт>. Принимает Basic SHOP:SECRET
    (иначе 401 invalid_credentials): GET /me, POST /payments (Idempotence-Key
    обязателен; повтор ключа — тот же платёж), GET /payments/<id>, POST /refunds.
    Служебный /__succeed/<id> переводит платёж в succeeded (гость «оплатил»).
    """

    SHOP = "100500"
    SECRET = "test_e2e_Fh8hUAVVBGUGbjmlzba6TB0iyUbos"

    def __init__(self, port: int):
        self.port = port
        self.calls: list[Dict[str, Any]] = []
        self.payments: Dict[str, Dict[str, Any]] = {}
        self.by_key: Dict[str, str] = {}
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", self.port)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    def calls_of(self, method: str, path: str) -> list[Dict[str, Any]]:
        return [c for c in self.calls if c["method"] == method and c["path"].startswith(path)]

    def _route(self, method: str, path: str, headers: Dict[str, str], body: Dict[str, Any]) -> tuple[int, Any]:
        if path.startswith("/__succeed/"):
            payment = self.payments.get(path[len("/__succeed/"):])
            if payment is None:
                return 404, {"type": "error", "code": "not_found"}
            payment["status"], payment["paid"] = "succeeded", True
            return 200, {"ok": True}
        self.calls.append({"method": method, "path": path, "headers": headers, "body": body})
        token = base64.b64encode(f"{self.SHOP}:{self.SECRET}".encode()).decode()
        if headers.get("authorization") != f"Basic {token}":
            return 401, {"type": "error", "id": uuid.uuid4().hex, "code": "invalid_credentials",
                         "description": "Authentication by given credentials failed"}
        if method == "GET" and path == "/me":
            return 200, {"account_id": self.SHOP, "test": True, "status": "enabled"}
        if method == "POST" and path == "/payments":
            key = headers.get("idempotence-key", "")
            if not key:
                return 400, {"type": "error", "code": "invalid_request", "description": "Idempotence-Key required"}
            payment_id = self.by_key.get(key)
            if payment_id is None:
                payment_id = str(uuid.uuid4())
                self.by_key[key] = payment_id
                self.payments[payment_id] = {
                    "id": payment_id, "status": "pending", "paid": False, "amount": body.get("amount", {}),
                    "description": body.get("description", ""), "metadata": body.get("metadata", {}),
                    "confirmation": {"type": "redirect",
                                     "confirmation_url": f"https://yoomoney.example/checkout/{payment_id}",
                                     "return_url": body.get("confirmation", {}).get("return_url", "")}}
            return 200, self.payments[payment_id]
        if method == "GET" and path.startswith("/payments/"):
            payment = self.payments.get(path[len("/payments/"):])
            return (200, payment) if payment else (404, {"type": "error", "code": "not_found"})
        if method == "POST" and path == "/refunds":
            payment = self.payments.get(body.get("payment_id", ""))
            if payment is None or payment["status"] != "succeeded":
                return 400, {"type": "error", "code": "invalid_request", "description": "payment not succeeded"}
            return 200, {"id": str(uuid.uuid4()), "status": "succeeded", "payment_id": payment["id"],
                         "amount": body.get("amount", {})}
        return 404, {"type": "error", "code": "not_found"}

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request_line = (await reader.readline()).decode("latin-1").strip()
            if not request_line:
                return
            headers: Dict[str, str] = {}
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, _, value = line.decode("latin-1").partition(":")
                headers[key.strip().lower()] = value.strip()
            raw = b""
            if headers.get("content-length"):
                raw = await reader.readexactly(int(headers["content-length"]))
            try:
                body = json.loads(raw.decode("utf-8")) if raw else {}
            except json.JSONDecodeError:
                body = {}
            parts = request_line.split(" ")
            method, target = parts[0], (parts[1] if len(parts) > 1 else "/")
            status, result = self._route(method, target, headers, body)
            payload = json.dumps(result).encode("utf-8")
            reason = {200: "OK", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found"}.get(status, "OK")
            head = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n"
                    f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n")
            writer.write(head.encode("latin-1") + payload)
            await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError, BrokenPipeError):
            pass
        finally:
            writer.close()


async def http_get(path: str, method: str = "GET") -> tuple[int, Dict[str, str], str]:
    """HTTP-запрос к порту C++-сервера (тот же, что WebSocket): ws://h:p → http://h:p."""
    base = SERVER_URL.replace("wss://", "https://", 1).replace("ws://", "http://", 1).rstrip("/")

    def run() -> tuple[int, Dict[str, str], str]:
        request = urllib.request.Request(base + path, method=method, data=b"" if method == "POST" else None)
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return (response.status, {k.lower(): v for k, v in response.headers.items()},
                        response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            return error.code, {k.lower(): v for k, v in error.headers.items()}, error.read().decode("utf-8", "replace")
        except (urllib.error.URLError, OSError) as error:
            return 0, {}, str(error)

    return await asyncio.to_thread(run)


async def f6_slots_post_and_ics(anna: "Client", anya: "Client") -> None:
    """F6: пост «свободные окна» (booking.slotsPost) и ICS-подписка (calendar.feed*, GET /ics/…)."""
    print("\n3a++. Пост «свободные окна» и ICS-подписка (F6)")
    created = await anna.call("booking.pageCreate", {
        "title": f"Окна {RUN}", "duration_minutes": 60, "days_ahead": 14, "max_per_day": 4, "notice_hours": 2,
    })
    page = created.get("payload", {}).get("page", {})
    page_id, slug = page.get("id", 0), page.get("slug", "")
    check(created.get("type") == "ok" and bool(slug), "F6: страница для поста создана", created)

    posted = await anna.call("booking.slotsPost", {"page_id": page_id}, timeout=40)
    post = posted.get("payload", {})
    text = post.get("text", "")
    check(posted.get("type") == "ok" and post.get("slots", 0) > 0, "booking.slotsPost: есть свободные окна", posted)
    check(f"#/b/{slug}" in text and text.splitlines()[-1].startswith("Записаться: "),
          "пост содержит ссылку страницы", text)
    check(text.startswith(f"Свободные окна: Окна {RUN} (1 ч)")
          and re.search(r"^(Пн|Вт|Ср|Чт|Пт|Сб|Вс), \d{1,2} [а-я]{3}: \d\d:\d\d", text, re.M) is not None,
          "пост: заголовок и строка дня «Пн, 21 окт: 10:00»", text)
    vk = (await anna.call("booking.slotsPost", {"page_id": page_id, "format": "vk", "days": 3}, timeout=40))
    check("Запись онлайн: " in vk.get("payload", {}).get("text", ""), "формат vk — «Запись онлайн: …»", vk)
    bad_days = await anna.call("booking.slotsPost", {"page_id": page_id, "days": 15})
    check(bad_days.get("code") == "bad_request", "days вне 1–14 — bad_request", bad_days)
    foreign = await anya.call("booking.slotsPost", {"page_id": page_id})
    check(foreign.get("code") == "not_found", "чужую страницу в пост не взять", foreign)

    guest = Client("f6-guest")
    await guest.connect()
    slots = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {}).get(
        "slots", [])
    start = slots[0].get("start", "") if slots else ""
    booked = await guest.call("booking.book", {
        "slug": slug, "start": start, "guest_name": "Фёдор", "comment": "Вопрос про ICS, сроки; детали",
    }, timeout=40)
    check(booked.get("type") == "ok", "F6: гость записался", booked)
    await guest.close()
    booking_id = 0
    for item in (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", []):
        if item.get("guest_name") == "Фёдор":
            booking_id = item.get("id", 0)
    start_unix = int(datetime.fromisoformat(start.replace("Z", "+00:00")).timestamp()) if start else 0
    after = (await anna.call("booking.slotsPost", {"page_id": page_id}, timeout=40)).get("payload", {})
    check(after.get("slots", 0) == post.get("slots", 0) - 1, "занятое окно из поста пропало",
          [post.get("slots"), after.get("slots")])

    feed = (await anna.call("calendar.feed")).get("payload", {})
    check(feed.get("enabled") is False and feed.get("path") == "", "calendar.feed: подписка выключена", feed)
    reset = (await anna.call("calendar.feedReset")).get("payload", {})
    path = reset.get("path", "")
    check(reset.get("enabled") is True and re.fullmatch(r"/ics/[A-Za-z0-9_-]{43}\.ics", path) is not None,
          "calendar.feedReset: путь /ics/<токен>.ics", reset)
    # Сервер с SELFVOY_SERVER_PUBLIC_URL: e2e получает тот же адрес в SELFVOY_E2E_SERVER_PUBLIC_URL.
    public = os.environ.get("SELFVOY_E2E_SERVER_PUBLIC_URL", "").rstrip("/")
    feed_url = reset.get("url", "")
    check(feed_url == public + path if public else (feed_url == "" or feed_url.endswith(path)),
          "url = SELFVOY_SERVER_PUBLIC_URL + path (без переменной — пусто)", reset)
    status, headers, body = await http_get(path)
    check(status == 200 and headers.get("content-type", "") == "text/calendar; charset=utf-8",
          "GET /ics/<токен>.ics — 200 text/calendar", [status, headers.get("content-type"), body[:200]])
    check(headers.get("cache-control") == "no-store", "ICS: Cache-Control: no-store", headers)
    unfolded = body.replace("\r\n ", "")
    check(body.startswith("BEGIN:VCALENDAR\r\n") and "X-WR-CALNAME:Selfvoy\r\n" in body
          and body.endswith("END:VCALENDAR\r\n"), "ICS: календарь с CRLF и X-WR-CALNAME", body[:300])
    check(f"UID:booking-{booking_id}-{start_unix}@selfvoy\r\n" in unfolded and "STATUS:CONFIRMED" in unfolded,
          "ICS содержит VEVENT записи", [booking_id, start_unix, unfolded[:600]])
    check(f"SUMMARY:Окна {RUN}: Фёдор\r\n" in unfolded
          and "DESCRIPTION:Вопрос про ICS\\, сроки\\; детали\r\n" in unfolded,
          "ICS: SUMMARY и экранированный DESCRIPTION", unfolded[:800])
    again = (await anna.call("calendar.feed")).get("payload", {})
    check(again.get("enabled") is True and again.get("path") == "", "calendar.feed: включена, токен не показан",
          again)

    new_path = (await anna.call("calendar.feedReset")).get("payload", {}).get("path", "")
    old_status, _, _ = await http_get(path)
    check(old_status == 404, "старый токен после feedReset — 404", old_status)
    new_status, _, _ = await http_get(new_path)
    check(new_status == 200 and new_path != path, "новый токен работает", new_status)
    post_status, post_headers, _ = await http_get(new_path, "POST")
    check(post_status == 405 and post_headers.get("allow") == "GET", "ICS: только GET (405)", post_status)
    unknown_status, _, _ = await http_get("/ics/" + "A" * 43 + ".ics")
    check(unknown_status == 404, "неизвестный токен — 404", unknown_status)
    disabled = await anna.call("calendar.feedDisable")
    check(disabled.get("payload", {}).get("enabled") is False, "calendar.feedDisable", disabled)
    off_status, _, _ = await http_get(new_path)
    check(off_status == 404, "после feedDisable — 404", off_status)
    health_status, _, health_body = await http_get("/healthz")
    check(health_status == 200 and json.loads(health_body or "{}").get("status") == "ok",
          "GET /healthz на порту сервера", [health_status, health_body])

    if booking_id:
        await anna.call("booking.ownerCancel", {"booking_id": booking_id})
    await anna.call("booking.pageDelete", {"page_id": page_id})


async def f7_ai_admin(anna: "Client", telegram_mock: "TelegramMock | None") -> None:
    """F7: ИИ-администратор страницы в Telegram — цена, кнопки времени, двухшаговая запись, вопрос специалисту."""
    print("\n3b++. ИИ-администратор в Telegram (F7)")
    bot_name = (await anna.call("server.info")).get("payload", {}).get("telegram_bot", "")
    if telegram_mock is None or not bot_name:
        refused = await anna.call("booking.pageCreate", {"title": f"ИИ {RUN}", "ai_admin": True})
        if not bot_name:
            check(refused.get("code") == "bad_request", "бот не настроен: ai_admin — bad_request", refused)
        print("  — пропуск сценария F7: нужны SELFVOY_E2E_TELEGRAM_MOCK=1 и SELFVOY_TELEGRAM_* у сервера")
        return
    tg = telegram_mock
    guest_chat, stranger_chat = 5552001, 5552009
    owner_name = (await anna.call("auth.me")).get("payload", {}).get("display_name", "")

    bad = await anna.call("booking.pageCreate", {"title": f"ИИ {RUN}", "ai_admin": "да"})
    check(bad.get("code") == "bad_request", "ai_admin не bool — bad_request", bad)
    created = await anna.call("booking.pageCreate", {
        "title": f"Массаж {RUN}", "duration_minutes": 30, "days_ahead": 14, "max_per_day": 4, "notice_hours": 1,
        "price": 150000, "currency": "RUB", "cancel_hours": 12, "ai_admin": True,
    })
    page = created.get("payload", {}).get("page", {})
    page_id, slug = page.get("id", 0), page.get("slug", "")
    ask_url = page.get("telegram_ask_url", "")
    check(page.get("ai_admin") is True and ask_url == f"https://t.me/{bot_name}?start=p_{slug}",
          "booking.pageCreate: ai_admin и telegram_ask_url (deep link p_<slug>)", page)
    guest = Client("f7-guest")
    await guest.connect()
    view = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    check(view.get("telegram_ask_url") == ask_url, "booking.get: гость видит telegram_ask_url", view.get("telegram_ask_url"))
    await guest.close()

    # /start p_<slug> — чат гостя привязан к странице.
    mark = len(tg.sent)
    tg.text(guest_chat, "/start p_" + slug, username="timofey_e2e", first_name="Тимофей")
    hello = await tg.wait_for("sendMessage", guest_chat, mark, timeout=20)
    check(hello is not None and hello.get("text", "").startswith(f"Здравствуйте! Я ИИ-администратор «Массаж {RUN}»"),
          "/start p_<slug>: приветствие ИИ-администратора", hello)
    mark = len(tg.sent)
    tg.text(stranger_chat, "/start p_zzzzzzzzzz")
    unknown = await tg.wait_for("sendMessage", stranger_chat, mark)
    check(unknown is not None and "не найдена" in unknown.get("text", ""), "/start с неизвестной страницей — отказ",
          unknown)

    # Вопрос о цене — ответ с ценой и политикой (запасной режим AI без LLM).
    mark = len(tg.sent)
    tg.text(guest_chat, "Сколько стоит сеанс?", username="timofey_e2e", first_name="Тимофей")
    price = await tg.wait_for("sendMessage", guest_chat, mark, timeout=30)
    check(price is not None and "Стоимость — 1" in price.get("text", "") and "500 ₽" in price.get("text", "")
          and "за 12 ч" in price.get("text", ""), "вопрос о цене: цена и политика отмены", price)

    # Запрос времени — кнопки (подписанные callback), без записи.
    mark = len(tg.sent)
    tg.text(guest_chat, "Когда можно записаться?", username="timofey_e2e", first_name="Тимофей")
    offer = await tg.wait_for("sendMessage", guest_chat, mark, timeout=40)
    rows = (offer or {}).get("reply_markup", {}).get("inline_keyboard", [])
    pick = rows[0][0].get("callback_data", "") if rows and rows[0] else ""
    check(offer is not None and 1 <= len(rows) <= 3 and pick.startswith(f"as:{page_id}:") and len(pick) <= 64,
          "запрос времени: до 3 кнопок свободного времени", offer)
    listed = (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", [])
    check(listed == [], "кнопки времени ничего не записывают", listed)

    # Чужой чат с той же кнопкой — отказ.
    mark = len(tg.sent)
    tg.callback(stranger_chat, pick)
    foreign = await tg.wait_for("answerCallbackQuery", None, mark)
    check(foreign is not None and "не для этого чата" in foreign.get("text", ""), "кнопка времени из чужого чата отвергнута",
          foreign)

    # Нажатие — вопрос «Записать на …?», подтверждение — запись обычным путём.
    mark = len(tg.sent)
    tg.callback(guest_chat, pick)
    ask = await tg.wait_for("sendMessage", guest_chat, mark, timeout=30, contains="Записать вас на")
    confirm_rows = (ask or {}).get("reply_markup", {}).get("inline_keyboard", [])
    confirm = confirm_rows[0][0].get("callback_data", "") if confirm_rows and confirm_rows[0] else ""
    check(ask is not None and confirm.startswith(f"ab:{page_id}:") and "Имя для записи: Тимофей" in ask.get("text", ""),
          "нажатие времени: вопрос «Записать на …?» с кнопкой подтверждения", ask)
    mark = len(tg.sent)
    tg.callback(guest_chat, confirm)
    done = await tg.wait_for("sendMessage", guest_chat, mark, timeout=40, contains="Готово: вы записаны на")
    answered = await tg.wait_for("answerCallbackQuery", None, mark, timeout=10)
    notice = await tg.wait_for("sendMessage", guest_chat, mark, timeout=20, contains="Вы записаны")
    check(done is not None and answered is not None and answered.get("text") == "Записано",
          "подтверждение: «Записано», бот подтвердил запись", {"done": done, "answer": answered})
    check(notice is not None, "извещение о записи пришло в этот чат (как booking.created)", notice)
    listed = (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", [])
    check(len(listed) == 1 and listed[0].get("guest_name") == "Тимофей" and listed[0].get("guest_email") == ""
          and "@timofey_e2e" in listed[0].get("guest_comment", "") and listed[0].get("price") == 150000,
          "запись создана обычным путём: имя из Telegram, без email, цена страницы", listed[:1])
    mark = len(tg.sent)
    tg.callback(guest_chat, confirm)
    again = await tg.wait_for("answerCallbackQuery", None, mark)
    check(again is not None and "заняли" in again.get("text", ""), "повторное подтверждение — время уже занято", again)

    # Вопрос вне сценариев — специалисту: уведомление telegram.question, ответ telegram.reply.
    mark = len(tg.sent)
    tg.text(guest_chat, "У меня аллергия на масла, это важно?", username="timofey_e2e", first_name="Тимофей")
    escalated = await tg.wait_for("sendMessage", guest_chat, mark, timeout=30)
    check(escalated is not None and escalated.get("text") == "Передал специалисту, он ответит вам здесь или напишет.",
          "escalate: гостю — «Передал специалисту»", escalated)
    question = None
    for _ in range(30):
        for item in (await anna.call("notifications.list", {"limit": 50})).get("payload", {}).get("notifications", []):
            if item.get("kind") == "telegram.question" and item.get("payload", {}).get("page_id") == page_id:
                question = item
                break
        if question:
            break
        await asyncio.sleep(0.2)
    qp = (question or {}).get("payload", {})
    check(question is not None and qp.get("guest_name") == "Тимофей" and qp.get("telegram_username") == "timofey_e2e"
          and "аллергия" in qp.get("text", "") and qp.get("question_id", 0) > 0,
          "владелец получил telegram.question {question_id, page_id, guest_name, telegram_username, text}", question)
    questions = (await anna.call("telegram.questions", {"status": "open"})).get("payload", {}).get("questions", [])
    mine = [q for q in questions if q.get("id") == qp.get("question_id")]
    check(len(mine) == 1 and mine[0].get("can_reply") is True and mine[0].get("page_title") == f"Массаж {RUN}",
          "telegram.questions: вопрос открыт, можно ответить", questions[:2])
    mark = len(tg.sent)
    replied = await anna.call("telegram.reply", {"question_id": qp.get("question_id", 0),
                                                 "text": "Масла без запаха, всё будет хорошо."})
    delivered = await tg.wait_for("sendMessage", guest_chat, mark, timeout=15, contains="Масла без запаха")
    check(replied.get("payload", {}).get("question", {}).get("status") == "answered" and delivered is not None
          and delivered.get("text") == f"{owner_name}: Масла без запаха, всё будет хорошо.",
          "telegram.reply: ответ специалиста дошёл гостю в Telegram", {"reply": replied, "sent": delivered})
    empty = await anna.call("telegram.reply", {"question_id": qp.get("question_id", 0), "text": ""})
    check(empty.get("code") == "bad_request", "telegram.reply без текста — bad_request", empty)
    missing = await anna.call("telegram.reply", {"question_id": 999999999, "text": "x"})
    check(missing.get("code") == "not_found", "telegram.reply на чужой/несуществующий вопрос — not_found", missing)

    # /stop — ответить в Telegram больше нельзя.
    mark = len(tg.sent)
    tg.text(guest_chat, "/stop")
    await tg.wait_for("sendMessage", guest_chat, mark)
    stopped = await anna.call("telegram.reply", {"question_id": qp.get("question_id", 0), "text": "Ещё?"})
    check(stopped.get("code") == "conflict", "после /stop гостя telegram.reply — conflict", stopped)
    await anna.call("booking.pageDelete", {"page_id": page_id})


async def s1b_reminders_confirm_branding(anna: "Client", telegram_mock: "TelegramMock | None") -> None:
    """F1 + F3: напоминания гостю в виде владельца, booking.confirm, «Приду» из Telegram, подпись Selfvoy."""
    print("\n3b+. Напоминания гостю, подтверждение «Приду» и подпись Selfvoy (F1, F3)")

    def at(value: str) -> float:
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except (AttributeError, ValueError):
            return 0.0

    created = await anna.call("booking.pageCreate", {
        "title": f"Напоминания {RUN}", "duration_minutes": 30, "days_ahead": 14, "max_per_day": 4,
        "notice_hours": 1,
    })
    page = created.get("payload", {}).get("page", {})
    page_id, slug = page.get("id", 0), page.get("slug", "")
    check(page.get("guest_reminders") is True and page.get("hide_branding") is False,
          "booking.pageCreate: guest_reminders=true, hide_branding=false по умолчанию", page)
    ref_code = (await anna.call("referral.get")).get("payload", {}).get("code", "")

    guest = Client("s1b-guest")
    await guest.connect()
    view = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    branding = view.get("branding", {})
    check(branding.get("show") is True and branding.get("text") == "Запись через Selfvoy"
          and branding.get("url") == f"https://selfvoy.example.com/#/signup?ref={ref_code}&src=booking"
          and view.get("ref") == ref_code, "booking.get: подпись Selfvoy со ссылкой регистрации", branding)
    # Встречи не раньше чем через 26 ч — у них оба напоминания (за сутки и за 2 часа).
    far = [s.get("start", "") for s in view.get("slots", []) if at(s.get("start", "")) >= time.time() + 26 * 3600]
    check(len(far) >= 2, "есть свободное время дальше суток", view.get("slots", [])[:3])
    if len(far) < 2:
        await guest.close()
        return
    booked = await guest.call("booking.book", {"slug": slug, "start": far[0], "guest_name": "Роза",
                                               "guest_email": f"roza.{RUN}@example.com"}, timeout=40)
    token = booked.get("payload", {}).get("token", "")
    check(booked.get("type") == "ok" and bool(token), "запись на время через сутки", booked)
    owned = (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", [])
    reminders = owned[0].get("reminders", []) if owned else []
    kinds = sorted(r.get("kind") for r in reminders)
    due = {r.get("kind"): at(r.get("due_at", "")) - at(r.get("start", "")) for r in reminders}
    check(kinds == ["24h", "2h"] and due.get("24h") == -86400 and due.get("2h") == -7200
          and all(r.get("sent_at") == "" and at(r.get("start", "")) == at(far[0]) for r in reminders),
          "владелец видит запланированные напоминания за сутки и за 2 часа", reminders)
    managed = (await guest.call("booking.manage", {"token": token}, timeout=40)).get("payload", {}).get("booking", {})
    check(managed.get("branding", {}).get("show") is True
          and managed.get("occurrences", [{}])[0].get("confirmed_at") == "",
          "booking.manage: подпись и встреча ещё не подтверждена", managed)

    # booking.confirm: confirmed_at у встречи; уведомление booking.confirmed — один раз.
    def confirmed_events() -> int:
        return len([e for e in anna.events_of("notification.new")
                    if e.get("payload", {}).get("kind") == "booking.confirmed"])

    before = confirmed_events()
    first = await guest.call("booking.confirm", {"token": token})
    confirmed_at = first.get("payload", {}).get("booking", {}).get("occurrences", [{}])[0].get("confirmed_at", "")
    check(first.get("type") == "ok" and bool(confirmed_at), "booking.confirm: встреча подтверждена", first)
    again = await guest.call("booking.confirm", {"token": token, "occurrence": far[0]})
    check(again.get("payload", {}).get("booking", {}).get("occurrences", [{}])[0].get("confirmed_at") == confirmed_at,
          "повторный booking.confirm ничего не меняет", again)
    await asyncio.sleep(0.5)
    check(confirmed_events() - before == 1, "владелец получил booking.confirmed ровно один раз",
          confirmed_events() - before)
    owned = (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", [])
    check(bool(owned) and owned[0].get("occurrences", [{}])[0].get("confirmed_at") == confirmed_at,
          "booking.list: confirmed_at у встречи", owned[:1])
    wrong = await guest.call("booking.confirm", {"token": "A" * 43})
    check(wrong.get("code") == "not_found", "booking.confirm с чужим токеном — not_found", wrong)

    # «Приду» из Telegram: только из чата, привязанного к записи.
    if telegram_mock is not None and booked.get("payload", {}).get("telegram_url"):
        tg = telegram_mock
        guest_chat, stranger_chat = 5551001, 5551009
        second = await guest.call("booking.book", {"slug": slug, "start": far[1], "guest_name": "Савва"}, timeout=40)
        second_token = second.get("payload", {}).get("token", "")
        second_url = second.get("payload", {}).get("telegram_url", "")
        mark = len(tg.sent)
        tg.text(guest_chat, "/start " + second_url.split("?start=", 1)[-1])
        await tg.wait_for("sendMessage", guest_chat, mark)
        second_id = 0
        for item in (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", []):
            if item.get("guest_name") == "Савва":
                second_id = item.get("id", 0)
        data = f"cf:{second_id}:{int(at(far[1]))}"
        mark = len(tg.sent)
        tg.callback(stranger_chat, data)
        foreign = await tg.wait_for("answerCallbackQuery", None, mark)
        view2 = (await guest.call("booking.manage", {"token": second_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(foreign is not None and "не для этого чата" in foreign.get("text", "")
              and view2.get("occurrences", [{}])[0].get("confirmed_at") == "",
              "«Приду» из чужого чата не подтверждает", foreign)
        mark = len(tg.sent)
        tg.callback(guest_chat, data, message_id=42)
        answer = await tg.wait_for("answerCallbackQuery", None, mark)
        edited = await tg.wait_for("editMessageReplyMarkup", guest_chat, mark)
        view2 = (await guest.call("booking.manage", {"token": second_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(answer is not None and answer.get("text") == "Спасибо, ждём вас" and edited is not None
              and edited.get("message_id") == 42 and bool(view2.get("occurrences", [{}])[0].get("confirmed_at")),
              "«Приду» из привязанного чата подтверждает встречу и убирает кнопки", {"answer": answer, "view": view2})

    # hide_branding при SELFVOY_PLANS_ENFORCED=0 сохраняется и скрывает подпись.
    hidden = await anna.call("booking.pageUpdate", {"page_id": page_id, "hide_branding": True})
    check(hidden.get("payload", {}).get("page", {}).get("hide_branding") is True,
          "booking.pageUpdate: hide_branding сохранён (тарифы не включены)", hidden)
    view = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    managed = (await guest.call("booking.manage", {"token": token}, timeout=40)).get("payload", {}).get("booking", {})
    check(view.get("branding", {}).get("show") is False and managed.get("branding", {}).get("show") is False,
          "подпись скрыта в booking.get и booking.manage", {"page": view.get("branding"),
                                                             "booking": managed.get("branding")})
    cancelled = await guest.call("booking.cancel", {"token": token})
    owned = (await anna.call("booking.list", {"page_id": page_id})).get("payload", {}).get("bookings", [])
    target = [b for b in owned if b.get("guest_name") == "Роза"]
    check(cancelled.get("type") == "ok" and bool(target) and target[0].get("reminders") == [],
          "отмена записи удаляет неотправленные напоминания", target[:1])
    await guest.close()
    await anna.call("booking.pageDelete", {"page_id": page_id})


async def s2a_payments(anna: "Client", yookassa: "YooKassaMock | None") -> None:
    """F4: цена, предоплата, политика отмены; ЮKassa (мок) и оплата по ссылке."""
    print("\n5a+. Цена, предоплата, политика отмены и ЮKassa (F4)")

    def at(value: str) -> float:
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except (AttributeError, ValueError):
            return 0.0

    def payment_events(status: str) -> int:
        return len([e for e in anna.events_of("notification.new")
                    if e.get("payload", {}).get("kind") == "booking.payment"
                    and e.get("payload", {}).get("payload", {}).get("status") == status])

    base = {"duration_minutes": 30, "days_ahead": 14, "max_per_day": 4, "notice_hours": 1}
    status = (await anna.call("payments.status")).get("payload", {})
    check(status.get("connected") is False and status.get("shop_id") == "",
          "payments.status: ЮKassa не подключена", status)
    no_method = await anna.call("booking.pageCreate", dict(base, title=f"Предоплата {RUN}", price=250000,
                                                           prepay="full"))
    check(no_method.get("code") == "bad_request", "предоплата без способа оплаты — bad_request", no_method)
    bad_deposit = await anna.call("booking.pageCreate", dict(base, title=f"Депозит {RUN}", price=1000,
                                                             prepay="deposit", deposit=2000,
                                                             payment_url="https://pay.example/e2e"))
    check(bad_deposit.get("code") == "bad_request", "депозит больше цены — bad_request", bad_deposit)
    # Гость — с другого адреса петли (127.0.0.2): лимиты гостевых методов считаются по IP,
    # а все сценарии выше уже записывались с 127.0.0.1 в ту же минуту.
    guest = Client("s2a-guest", "127.0.0.2")
    await guest.connect()
    pages: list[int] = []

    if yookassa is None:
        print("  — пропуск сценария ЮKassa: нужны SELFVOY_E2E_YOOKASSA_MOCK=1 и SELFVOY_YOOKASSA_API_URL у сервера")
    else:
        yk = yookassa
        wrong = await anna.call("payments.connect", {"provider": "yookassa", "shop_id": yk.SHOP,
                                                     "secret_key": "test_wrong_secret_value"})
        check(wrong.get("code") == "bad_request" and len(yk.calls_of("GET", "/me")) == 1,
              "payments.connect с неверным ключом — bad_request (ЮKassa ответила 401)", wrong)
        connected = await anna.call("payments.connect", {"provider": "yookassa", "shop_id": yk.SHOP,
                                                         "secret_key": yk.SECRET})
        info = connected.get("payload", {})
        check(info.get("connected") is True and info.get("shop_id") == yk.SHOP and info.get("provider") == "yookassa"
              and "secret_key" not in info, "payments.connect: магазин подключён (GET /me)", connected)
        created = await anna.call("booking.pageCreate", dict(base, title=f"ЮKassa {RUN}", price=250000,
                                                             prepay="full", cancel_hours=0,
                                                             policy_text="Отмена в любое время — полный возврат"))
        page = created.get("payload", {}).get("page", {})
        pages.append(page.get("id", 0))
        check(page.get("price") == 250000 and page.get("prepay") == "full" and page.get("currency") == "RUB",
              "страница с ценой и полной предоплатой (способ — ЮKassa)", page)
        view = (await guest.call("booking.get", {"slug": page.get("slug", "")}, timeout=40)).get("payload", {}) \
            .get("page", {})
        check(view.get("price") == 250000 and view.get("prepay") == "full"
              and view.get("policy", {}).get("cancel_hours") == 0
              and view.get("policy", {}).get("text") == "Отмена в любое время — полный возврат",
              "booking.get: цена, предоплата и политика отмены", {k: view.get(k) for k in ("price", "prepay", "policy")})
        slots = [s.get("start", "") for s in view.get("slots", [])]
        check(len(slots) >= 1, "есть свободное время на странице с предоплатой", view.get("slots", [])[:2])
        if slots:
            booked = await guest.call("booking.book", {"slug": page.get("slug", ""), "start": slots[0],
                                                       "guest_name": "Платон",
                                                       "guest_email": f"platon.{RUN}@example.com"}, timeout=40)
            token = booked.get("payload", {}).get("token", "")
            payment = booked.get("payload", {}).get("booking", {}).get("payment") or {}
            action = payment.get("action_url", "")
            check(booked.get("type") == "ok" and payment.get("status") == "pending"
                  and payment.get("method") == "yookassa" and payment.get("amount") == 250000
                  and action.startswith("https://yoomoney.example/checkout/") and "provider_id" not in payment
                  and at(payment.get("due_at", "")) > time.time(),
                  "booking.book: оплата pending, ссылка оплаты ЮKassa", booked)
            owned = (await anna.call("booking.list", {"page_id": page.get("id", 0)})).get("payload", {}) \
                .get("bookings", [])
            booking_id = owned[0].get("id", 0) if owned else 0
            payment_id = action.rsplit("/", 1)[-1]
            created_calls = yk.calls_of("POST", "/payments")
            last = created_calls[-1] if created_calls else {"headers": {}, "body": {}}
            check(bool(owned) and owned[0].get("payment", {}).get("provider_id") == payment_id
                  and last["headers"].get("idempotence-key") == f"bk-{booking_id}"
                  and last["body"].get("amount") == {"value": "2500.00", "currency": "RUB"}
                  and last["body"].get("capture") is True and "receipt" not in last["body"]
                  and last["body"].get("confirmation", {}).get("return_url", "").endswith(f"/#/m/{token}?paid=1"),
                  "POST /payments: Idempotence-Key bk-<id>, сумма 2500.00, возврат на ссылку управления",
                  {"headers": last["headers"].get("idempotence-key"), "body": last["body"]})
            before_paid = payment_events("paid")

            def succeed() -> int:
                request = urllib.request.Request(f"http://127.0.0.1:{yk.port}/__succeed/{payment_id}",
                                                 method="POST", data=b"")
                with urllib.request.urlopen(request, timeout=5) as response:
                    return response.status

            check(await asyncio.to_thread(succeed) == 200, "мок ЮKassa: платёж оплачен (/__succeed)")
            managed = (await guest.call("booking.manage", {"token": token}, timeout=40)).get("payload", {}) \
                .get("booking", {})
            check(managed.get("payment", {}).get("status") == "paid" and bool(managed.get("payment", {}).get("paid_at")),
                  "booking.manage после оплаты: payment.status = paid", managed.get("payment"))
            await asyncio.sleep(0.5)
            check(payment_events("paid") - before_paid == 1, "владелец получил booking.payment (paid)",
                  payment_events("paid") - before_paid)
            cancelled = await guest.call("booking.cancel", {"token": token})
            refunds = yk.calls_of("POST", "/refunds")
            refund = refunds[-1] if refunds else {"headers": {}, "body": {}}
            check(cancelled.get("type") == "ok"
                  and cancelled.get("payload", {}).get("booking", {}).get("payment", {}).get("status") == "refunded"
                  and refund["body"].get("payment_id") == payment_id
                  and refund["headers"].get("idempotence-key") == f"rf-{booking_id}",
                  "отмена до срока — возврат через ЮKassa (POST /refunds rf-<id>)",
                  {"cancel": cancelled.get("payload", {}).get("booking", {}).get("payment"), "refund": refund})
        disconnected = await anna.call("payments.disconnect")
        check(disconnected.get("payload", {}).get("connected") is False, "payments.disconnect", disconnected)

    # Оплата по ссылке: «Я оплатил(а)» → владелец отмечает оплату.
    link_page = (await anna.call("booking.pageCreate", dict(
        base, title=f"По ссылке {RUN}", price=200000, prepay="deposit", deposit=50000,
        payment_url="https://pay.example/e2e", payment_note="СБП по номеру +7 900 000-00-00",
        pay_within_minutes=30))).get("payload", {}).get("page", {})
    pages.append(link_page.get("id", 0))
    check(link_page.get("prepay") == "deposit" and link_page.get("deposit") == 50000,
          "страница с депозитом и оплатой по ссылке", link_page)
    link_view = (await guest.call("booking.get", {"slug": link_page.get("slug", "")}, timeout=40)) \
        .get("payload", {}).get("page", {})
    link_slots = [s.get("start", "") for s in link_view.get("slots", [])]
    if link_slots:
        link_booked = await guest.call("booking.book", {"slug": link_page.get("slug", ""), "start": link_slots[0],
                                                        "guest_name": "Лиля"}, timeout=40)
        link_token = link_booked.get("payload", {}).get("token", "")
        link_payment = link_booked.get("payload", {}).get("booking", {}).get("payment") or {}
        check(link_payment.get("method") == "link" and link_payment.get("kind") == "deposit"
              and link_payment.get("amount") == 50000 and link_payment.get("action_url") == "https://pay.example/e2e",
              "booking.book: депозит по ссылке владельца", link_booked)
        link_guest_view = link_booked.get("payload", {}).get("booking", {})
        check(link_guest_view.get("price") == 200000 and link_guest_view.get("currency") == "RUB",
              "booking.book: снимок цены у записи (price — цена услуги, не депозит)",
              {k: link_guest_view.get(k) for k in ("price", "currency")})
        before_claim = payment_events("claimed")
        claimed = await guest.call("booking.paymentClaim", {"token": link_token})
        check(claimed.get("payload", {}).get("booking", {}).get("payment", {}).get("status") == "claimed",
              "booking.paymentClaim: «Я оплатил(а)» → claimed", claimed)
        await asyncio.sleep(0.5)
        check(payment_events("claimed") - before_claim == 1, "владелец получил booking.payment (claimed)",
              payment_events("claimed") - before_claim)
        owned = (await anna.call("booking.list", {"page_id": link_page.get("id", 0)})).get("payload", {}) \
            .get("bookings", [])
        link_id = owned[0].get("id", 0) if owned else 0
        check(bool(owned) and owned[0].get("price") == 200000 and owned[0].get("currency") == "RUB"
              and (owned[0].get("payment") or {}).get("amount") == 50000,
              "booking.list: price = цена услуги, payment.amount = депозит", owned[:1])
        # Владелец поднял цену страницы — у сделанной записи остаётся цена на момент записи.
        raised = await anna.call("booking.pageUpdate", {"page_id": link_page.get("id", 0), "price": 300000})
        check(raised.get("payload", {}).get("page", {}).get("price") == 300000,
              "booking.pageUpdate: новая цена страницы", raised)
        owned_after = (await anna.call("booking.list", {"page_id": link_page.get("id", 0)})).get("payload", {}) \
            .get("bookings", [])
        managed_after = (await guest.call("booking.manage", {"token": link_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(bool(owned_after) and owned_after[0].get("price") == 200000 and managed_after.get("price") == 200000
              and managed_after.get("currency") == "RUB",
              "смена цены страницы не меняет price записи (вид владельца и гостя)",
              {"owner": owned_after[0].get("price") if owned_after else None, "guest": managed_after.get("price")})
        set_paid = await anna.call("booking.paymentSet", {"booking_id": link_id, "status": "paid"})
        check(set_paid.get("payload", {}).get("booking", {}).get("payment", {}).get("status") == "paid",
              "booking.paymentSet: владелец отметил оплату", set_paid)
        bad_status = await anna.call("booking.paymentSet", {"booking_id": link_id, "status": "pending"})
        check(bad_status.get("code") == "bad_request", "booking.paymentSet: неверный статус — bad_request", bad_status)
        await anna.call("booking.ownerCancel", {"booking_id": link_id})
    else:
        check(False, "есть свободное время на странице с оплатой по ссылке", link_view.get("slots", [])[:2])

    # late_cancel=block: позже срока гость не отменяет и не переносит.
    strict = (await anna.call("booking.pageCreate", dict(base, title=f"Строго {RUN}", cancel_hours=168,
                                                         late_cancel="block"))).get("payload", {}).get("page", {})
    pages.append(strict.get("id", 0))
    strict_view = (await guest.call("booking.get", {"slug": strict.get("slug", "")}, timeout=40)) \
        .get("payload", {}).get("page", {})
    near = [s.get("start", "") for s in strict_view.get("slots", []) if at(s.get("start", "")) < time.time() + 160 * 3600]
    if near:
        strict_booked = await guest.call("booking.book", {"slug": strict.get("slug", ""), "start": near[0],
                                                          "guest_name": "Стас"}, timeout=40)
        strict_token = strict_booked.get("payload", {}).get("token", "")
        strict_managed = (await guest.call("booking.manage", {"token": strict_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(strict_managed.get("late") is True and strict_managed.get("can_cancel") is False
              and strict_managed.get("can_reschedule") is False
              and strict_managed.get("policy", {}).get("late_cancel") == "block",
              "late_cancel=block: позже срока can_cancel/can_reschedule = false", strict_managed)
        refused = await guest.call("booking.cancel", {"token": strict_token})
        check(refused.get("code") == "conflict" and "меньше 168 ч" in refused.get("message", ""),
              "late_cancel=block: отмена гостем позже срока — conflict", refused)
        owned = (await anna.call("booking.list", {"page_id": strict.get("id", 0)})).get("payload", {}) \
            .get("bookings", [])
        if owned:
            await anna.call("booking.ownerCancel", {"booking_id": owned[0].get("id", 0)})
    else:
        check(False, "есть свободное время в ближайшие 7 дней", strict_view.get("slots", [])[:2])
    await guest.close()
    for page_id in pages:
        await anna.call("booking.pageDelete", {"page_id": page_id})


def server_log_mails(kind: str, to: str) -> int | None:
    """Писем вида kind на адрес to в логе C++-сервера (dev-почта пишет «[dev] письмо (kind) для to»).

    Путь к логу — SELFVOY_E2E_SERVER_LOG (его задаёт e2e_pg.sh); без него — None (проверка пропускается).
    """
    path = os.environ.get("SELFVOY_E2E_SERVER_LOG", "")
    if not path or not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as handle:
        return sum(1 for line in handle if f"[dev] письмо ({kind}) для {to}:" in line)


async def s2b_packages(anna: "Client", yookassa: "YooKassaMock | None") -> None:
    """F5: пакеты/абонементы (покупка при записи, запись по пакету, остаток, возврат) и cc_email."""
    print("\n5a++. Пакеты, абонементы и второй получатель (F5)")

    def at(value: str) -> float:
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except (AttributeError, ValueError):
            return 0.0

    def owner_events(kind: str) -> list[Dict[str, Any]]:
        return [e.get("payload", {}) for e in anna.events_of("notification.new")
                if e.get("payload", {}).get("kind") == kind]

    base = {"duration_minutes": 30, "days_ahead": 14, "max_per_day": 6, "notice_hours": 1}
    bad_offer = await anna.call("booking.pageCreate", dict(base, title=f"Пакет-ошибка {RUN}",
                                                           packages=[{"title": "Одно", "sessions": 1, "price": 1000}]))
    check(bad_offer.get("code") == "bad_request", "packages: sessions < 2 — bad_request", bad_offer)
    created = await anna.call("booking.pageCreate", dict(base, title=f"Абонементы {RUN}", cancel_hours=24,
                                                         packages=[{"title": "3 занятия", "sessions": 3,
                                                                    "price": 900000}]))
    page = created.get("payload", {}).get("page", {})
    pages = [page.get("id", 0)]
    offers = page.get("packages", [])
    check(len(offers) == 1 and offers[0].get("id") == 1 and offers[0].get("valid_days") == 90,
          "booking.pageCreate: предложение пакета (id 1, срок 90 дней)", offers)
    kept = dict(offers[0], title="3 занятия (утро)") if offers else {}
    updated = await anna.call("booking.pageUpdate", {"page_id": page.get("id", 0), "price": 150000,
                                                     "packages": [kept, {"title": "5 занятий", "sessions": 5,
                                                                         "price": 1400000, "valid_days": 60}]})
    offers = updated.get("payload", {}).get("page", {}).get("packages", [])
    check([o.get("id") for o in offers] == [1, 2] and offers[0].get("title") == "3 занятия (утро)",
          "booking.pageUpdate: пакет с id сохраняет id, новый получает следующий", offers)

    guest = Client("s2b-guest", "127.0.0.3")
    await guest.connect()
    slug = page.get("slug", "")
    view = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    check(len(view.get("packages", [])) == 2, "booking.get: гость видит предложения пакетов", view.get("packages"))
    far = [s.get("start", "") for s in view.get("slots", []) if at(s.get("start", "")) > time.time() + 26 * 3600]
    if len(far) < 3:
        check(False, "есть 3 свободных окна позже чем через 26 ч", view.get("slots", [])[:3])
        await guest.close()
        for page_id in pages:
            await anna.call("booking.pageDelete", {"page_id": page_id})
        return

    guest_email, cc_email = f"pk.{RUN}@example.com", f"parent.{RUN}@example.com"
    bad_cc = await guest.call("booking.book", {"slug": slug, "start": far[0], "guest_name": "Маша",
                                               "guest_email": guest_email, "cc_email": guest_email.upper()},
                              timeout=40)
    check(bad_cc.get("code") == "bad_request", "booking.book: cc_email = guest_email — bad_request", bad_cc)
    purchased_before = len(owner_events("package.purchased"))
    bought = await guest.call("booking.book", {"slug": slug, "start": far[0], "guest_name": "Маша",
                                               "guest_email": guest_email, "cc_email": cc_email,
                                               "package_id": 1}, timeout=40)
    info = bought.get("payload", {})
    package_token = info.get("package_token", "")
    pack = info.get("package", {})
    check(bought.get("type") == "ok" and len(package_token) == 43
          and info.get("package_url", "").endswith(f"/#/p/{package_token}")
          and pack.get("status") == "active" and pack.get("sessions_left") == 2
          and pack.get("payment", {}).get("kind") == "package",
          "покупка пакета при записи: package_token/url, пакет active, остаток 2", info)
    check(info.get("booking", {}).get("cc_email") == f"p***@example.com",
          "вид гостя: cc_email замаскирован", info.get("booking", {}).get("cc_email"))
    check(view.get("price") == 150000 and info.get("booking", {}).get("price") == 0
          and info.get("booking", {}).get("by_package") is True,
          "запись с покупкой пакета: price = 0 (оплачено абонементом), хотя цена страницы 150000",
          {"page": view.get("price"), "booking": info.get("booking", {}).get("price")})
    owned = (await anna.call("booking.list", {"page_id": page.get("id", 0)})).get("payload", {}).get("bookings", [])
    package_id = owned[0].get("package_id", 0) if owned else 0
    check(bool(owned) and owned[0].get("cc_email") == cc_email and package_id > 0,
          "вид владельца: cc_email и package_id записи", owned[:1])
    check(bool(owned) and owned[0].get("price") == 0,
          "вид владельца: запись по пакету — price 0", owned[:1])
    await asyncio.sleep(0.5)
    check(len(owner_events("package.purchased")) - purchased_before == 1, "владелец получил package.purchased",
          owner_events("package.purchased")[-1:])
    await asyncio.sleep(0.5)
    cc_mails = server_log_mails("booking.created", cc_email)
    if cc_mails is not None:
        check(cc_mails == 1, "cc_email получил письмо о записи (dev-почта в логе сервера)", cc_mails)
    got = (await guest.call("package.get", {"token": package_token})).get("payload", {}).get("package", {})
    check(got.get("sessions_left") == 2 and got.get("page_slug") == slug and len(got.get("bookings", [])) == 1
          and got.get("branding", {}).get("text") == "Запись через Selfvoy",
          "package.get: остаток, страница, встречи, подпись", got)
    check(len(got.get("ref", "")) == 8 and got.get("ref") == view.get("ref"),
          "package.get: ref — реферальный код владельца (как booking.get)", {"package": got.get("ref"),
                                                                              "page": view.get("ref")})

    low_before = len(owner_events("package.low"))
    by_token = await guest.call("booking.book", {"slug": slug, "start": far[1], "package_token": package_token},
                                timeout=40)
    by_token_info = by_token.get("payload", {})
    check(by_token.get("type") == "ok" and by_token_info.get("booking", {}).get("guest_name") == "Маша"
          and by_token_info.get("package", {}).get("sessions_left") == 1,
          "запись по package_token: имя из пакета, остаток уменьшился до 1", by_token)
    check(by_token_info.get("booking", {}).get("price") == 0,
          "запись по package_token: price 0", by_token_info.get("booking", {}).get("price"))
    await asyncio.sleep(0.5)
    lows = owner_events("package.low")
    check(len(lows) - low_before == 1 and lows[-1].get("payload", {}).get("left") == 1,
          "package.low владельцу: осталось 1 занятие", lows[-1:])
    low_mails = server_log_mails("package.low", cc_email)
    if low_mails is not None:
        check(low_mails == 1, "cc_email получил «осталось 1 занятие»", low_mails)
    cancelled = await guest.call("booking.cancel", {"token": by_token_info.get("token", "")})
    after_cancel = (await guest.call("package.get", {"token": package_token})).get("payload", {}).get("package", {})
    check(cancelled.get("type") == "ok" and after_cancel.get("sessions_left") == 2,
          "отмена до срока возвращает занятие (остаток снова 2)", after_cancel.get("sessions_left"))
    other_page = await guest.call("booking.book", {"slug": "zzzzzzzzzz", "start": far[2],
                                                   "package_token": package_token}, timeout=40)
    check(other_page.get("code") == "not_found", "запись по пакету на несуществующей странице — not_found", other_page)

    # Владелец: оплата пакета, продажа вне сайта, корректировка, список, отмена.
    set_paid = await anna.call("package.paymentSet", {"package_id": package_id, "status": "paid"})
    check(set_paid.get("payload", {}).get("package", {}).get("payment", {}).get("status") == "paid",
          "package.paymentSet: владелец отметил оплату пакета", set_paid)
    issued = await anna.call("package.issue", {"page_id": page.get("id", 0), "offer_id": 2, "guest_name": "Пётр",
                                               "guest_email": f"petr.{RUN}@example.com", "paid": True})
    issued_info = issued.get("payload", {})
    issued_pack = issued_info.get("package", {})
    check(issued.get("type") == "ok" and issued_pack.get("status") == "active"
          and issued_pack.get("sessions_left") == 5 and issued_pack.get("valid_days") == 60
          and issued_pack.get("payment", {}).get("status") == "paid"
          and issued_info.get("url", "").endswith("/#/p/" + issued_info.get("token", "-")),
          "package.issue: пакет вне сайта, active, оплачен, ссылка", issued_info)
    adjusted = await anna.call("package.adjust", {"package_id": issued_pack.get("id", 0), "delta": 2})
    check(adjusted.get("payload", {}).get("package", {}).get("sessions_left") == 7, "package.adjust: +2 занятия",
          adjusted)
    zero = await anna.call("package.adjust", {"package_id": issued_pack.get("id", 0), "delta": 0})
    check(zero.get("code") == "bad_request", "package.adjust: delta 0 — bad_request", zero)
    listed = (await anna.call("package.list", {"page_id": page.get("id", 0)})).get("payload", {}).get("packages", [])
    check({p.get("id") for p in listed} == {package_id, issued_pack.get("id")}, "package.list: оба пакета страницы",
          [p.get("id") for p in listed])
    gone = await anna.call("package.cancel", {"package_id": issued_pack.get("id", 0)})
    check(gone.get("payload", {}).get("package", {}).get("status") == "cancelled", "package.cancel: пакет отменён", gone)
    refused = await guest.call("booking.book", {"slug": slug, "start": far[2],
                                                "package_token": issued_info.get("token", "")}, timeout=40)
    check(refused.get("code") == "conflict", "запись по отменённому пакету — conflict", refused)

    if yookassa is not None:
        yk = yookassa
        await anna.call("payments.connect", {"provider": "yookassa", "shop_id": yk.SHOP, "secret_key": yk.SECRET})
        yk_buy = await guest.call("booking.book", {"slug": slug, "start": far[2], "guest_name": "Юля",
                                                   "package_id": 1}, timeout=40)
        yk_info = yk_buy.get("payload", {})
        yk_token = yk_info.get("package_token", "")
        yk_pack = yk_info.get("package", {})
        calls = yk.calls_of("POST", "/payments")
        last = calls[-1] if calls else {"headers": {}, "body": {}}
        check(yk_pack.get("status") == "pending" and yk_pack.get("payment", {}).get("method") == "yookassa"
              and last["headers"].get("idempotence-key", "").startswith("pk-")
              and last["body"].get("confirmation", {}).get("return_url", "").endswith(f"/#/p/{yk_token}?paid=1"),
              "покупка через ЮKassa: pending, POST /payments pk-<id>, возврат на ссылку пакета",
              {"package": yk_pack, "headers": last["headers"].get("idempotence-key")})
        payment_id = yk_pack.get("payment", {}).get("action_url", "").rsplit("/", 1)[-1]

        def succeed() -> int:
            request = urllib.request.Request(f"http://127.0.0.1:{yk.port}/__succeed/{payment_id}",
                                             method="POST", data=b"")
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status

        await asyncio.to_thread(succeed)
        paid = (await guest.call("package.get", {"token": yk_token})).get("payload", {}).get("package", {})
        check(paid.get("status") == "active" and paid.get("payment", {}).get("status") == "paid"
              and bool(paid.get("valid_until")), "package.get после оплаты ЮKassa: active, paid, valid_until", paid)
        await anna.call("payments.disconnect")

    await guest.close()
    for item in (await anna.call("booking.list", {"page_id": page.get("id", 0)})).get("payload", {}).get("bookings", []):
        if item.get("status") == "active":
            await anna.call("booking.ownerCancel", {"booking_id": item.get("id", 0)})
    for page_id in pages:
        await anna.call("booking.pageDelete", {"page_id": page_id})


def server_log_link(kind: str, to: str) -> str | None:
    """Ссылка последнего письма вида kind на адрес to из лога C++-сервера (dev-почта:
    «[dev] письмо (kind) для to: <тема> — <ссылка>»). None — лога нет (проверка пропускается)."""
    path = os.environ.get("SELFVOY_E2E_SERVER_LOG", "")
    if not path or not os.path.exists(path):
        return None
    link = ""
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if f"[dev] письмо ({kind}) для {to}:" in line and " — " in line:
                link = line.rstrip("\n").rsplit(" — ", 1)[-1].strip()
    return link


async def s3_waitlist_rebook_tax(anna: "Client", telegram_mock: "TelegramMock | None") -> None:
    """F8: лист ожидания (держание, предложение, «забрать»), повторный приём; F10: чек «Мой налог»."""
    print("\n5a+++. Лист ожидания, повторный приём (F8) и чек «Мой налог» (F10)")

    def at(value: str) -> float:
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except (AttributeError, ValueError):
            return 0.0

    def owner_events(kind: str) -> list[Dict[str, Any]]:
        return [e.get("payload", {}) for e in anna.events_of("notification.new")
                if e.get("payload", {}).get("kind") == kind]

    async def wait_events(kind: str, count: int, timeout: float = 5.0) -> list[Dict[str, Any]]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and len(owner_events(kind)) < count:
            await asyncio.sleep(0.1)
        return owner_events(kind)

    async def mailed_link(kind: str, to: str) -> str | None:
        """Ссылка из письма: dev-почта пишет его в лог из фоновой очереди — ждём до 5 с."""
        for _ in range(50):
            link = server_log_link(kind, to)
            if link is None or link:
                return link
            await asyncio.sleep(0.1)
        return ""

    base = {"duration_minutes": 30, "days_ahead": 14, "max_per_day": 6, "notice_hours": 1}
    created = await anna.call("booking.pageCreate", dict(base, title=f"Лист ожидания {RUN}", waitlist=True,
                                                         waitlist_hold_minutes=30, rebook_after_days=14))
    page = created.get("payload", {}).get("page", {})
    pages = [page.get("id", 0)]
    check(page.get("waitlist") is True and page.get("waitlist_hold_minutes") == 30
          and page.get("rebook_after_days") == 14,
          "booking.pageCreate: waitlist, waitlist_hold_minutes, rebook_after_days", page)
    bad = await anna.call("booking.pageUpdate", {"page_id": page.get("id", 0), "rebook_after_days": 3})
    check(bad.get("code") == "bad_request", "rebook_after_days 3 (не 0 и не 7–180) — bad_request", bad)
    plain = await anna.call("booking.pageCreate", dict(base, title=f"Без листа {RUN}"))
    plain_page = plain.get("payload", {}).get("page", {})
    pages.append(plain_page.get("id", 0))
    slug = page.get("slug", "")

    guest = Client("s3-guest", "127.0.0.4")
    waiter = Client("s3-waiter", "127.0.0.5")
    rival = Client("s3-rival", "127.0.0.6")
    for client in (guest, waiter, rival):
        await client.connect()

    async def finish() -> None:
        for client in (guest, waiter, rival):
            await client.close()
        for item in (await anna.call("booking.list", {"limit": 200})).get("payload", {}).get("bookings", []):
            if item.get("status") == "active" and item.get("page_id") in pages:
                await anna.call("booking.ownerCancel", {"booking_id": item.get("id", 0)})
        for entry in (await anna.call("booking.waitlist", {})).get("payload", {}).get("entries", []):
            if entry.get("page_id") in pages and entry.get("status") in ("waiting", "offered"):
                await anna.call("booking.waitlistRemove", {"entry_id": entry.get("id", 0)})
        await anna.call("prefs.set", {"npd_receipts": False})
        for page_id in pages:
            await anna.call("booking.pageDelete", {"page_id": page_id})

    view = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    check(view.get("waitlist") is True and view.get("waitlist_hold_minutes") == 30
          and view.get("rebook_after_days") == 14, "booking.get: гость видит лист ожидания", view)
    far = [s for s in view.get("slots", []) if at(s.get("start", "")) > time.time() + 3 * 3600]
    if len(far) < 2:
        check(False, "есть 2 свободных окна позже чем через 3 ч", view.get("slots", [])[:3])
        await finish()
        return
    slot = far[0]
    start, day = slot.get("start", ""), slot.get("day", "")

    booked = await guest.call("booking.book", {"slug": slug, "start": start, "guest_name": "Гость",
                                               "guest_email": f"s3.guest.{RUN}@example.com"}, timeout=40)
    check(booked.get("type") == "ok", "booking.book на странице с листом ожидания", booked)
    owned = (await anna.call("booking.list", {"page_id": page.get("id", 0)})).get("payload", {}).get("bookings", [])
    first = owned[0] if owned else {}
    end = first.get("occurrences", [{}])[0].get("end", "")
    check(at(first.get("rebook_due_at", "")) == at(end) + 14 * 86400 and first.get("rebook_sent_at") == "",
          "вид владельца: rebook_due_at = конец встречи + 14 дней, rebook_sent_at пуст",
          {"due": first.get("rebook_due_at"), "end": end})

    # Гость без свободного времени встаёт в лист ожидания.
    forbidden = await waiter.call("booking.waitlistJoin", {"slug": plain_page.get("slug", ""), "guest_name": "Вера",
                                                           "guest_email": f"s3.vera.{RUN}@example.com"})
    check(forbidden.get("code") == "forbidden", "booking.waitlistJoin: страница без листа — forbidden", forbidden)
    no_email = await waiter.call("booking.waitlistJoin", {"slug": slug, "guest_name": "Вера"})
    check(no_email.get("code") == "bad_request", "booking.waitlistJoin без email — bad_request", no_email)
    vera_email = f"s3.vera.{RUN}@example.com"
    joined_before = len(owner_events("waitlist.joined"))
    joined = await waiter.call("booking.waitlistJoin", {"slug": slug, "guest_name": "Вера", "guest_email": vera_email,
                                                        "days": [day], "part_of_day": "any",
                                                        "comment": "Любое время в этот день"})
    entry = joined.get("payload", {}).get("entry", {})
    telegram_url = joined.get("payload", {}).get("telegram_url", "")
    check(joined.get("type") == "ok" and entry.get("status") == "waiting" and entry.get("days") == [day]
          and bool(entry.get("expires_at")), "booking.waitlistJoin: запись waiting, день, срок", joined)
    if telegram_mock is not None:
        check(telegram_url.startswith("https://t.me/selfvoy_e2e_bot?start=w_"),
              "booking.waitlistJoin: telegram_url — подписка на предложения в боте", telegram_url)
    events = await wait_events("waitlist.joined", joined_before + 1)
    check(len(events) == joined_before + 1 and events[-1].get("payload", {}).get("entry_id") == entry.get("id"),
          "владелец получил waitlist.joined", events[-1:])
    again = await waiter.call("booking.waitlistJoin", {"slug": slug, "guest_name": "Вера",
                                                       "guest_email": vera_email.upper(), "days": [day]})
    check(again.get("payload", {}).get("entry", {}).get("id") == entry.get("id"),
          "повтор с тем же email (другой регистр) обновляет ту же запись", again)
    chat = 7_300_000 + int(RUN[:4], 16)
    if telegram_mock is not None and telegram_url:
        mark = len(telegram_mock.sent)
        telegram_mock.text(chat, "/start " + telegram_url.split("?start=", 1)[-1])
        greeting = await telegram_mock.wait_for("sendMessage", chat, mark, contains="Готово: как только")
        check(greeting is not None, "бот: подписка на предложения листа ожидания (/start w_…)", greeting)

    # Отмена освобождает время — оно держится за Верой и не видно другим.
    mark = len(telegram_mock.sent) if telegram_mock is not None else 0
    cancelled = await guest.call("booking.cancel", {"token": booked.get("payload", {}).get("token", "")})
    check(cancelled.get("type") == "ok", "гость отменил запись — время освободилось", cancelled)
    entries = (await anna.call("booking.waitlist", {"page_id": page.get("id", 0)})).get("payload", {}).get("entries", [])
    mine = next((e for e in entries if e.get("id") == entry.get("id")), {})
    check(mine.get("status") == "offered" and at(mine.get("offer", {}).get("start", "")) == at(start)
          and mine.get("guest_email", "").startswith("s***@"),
          "booking.waitlist: время предложено Вере (offer.start), email замаскирован", mine)
    taken = await rival.call("booking.book", {"slug": slug, "start": start, "guest_name": "Соперник"}, timeout=40)
    check(taken.get("code") == "conflict", "держание: другой гость не записывается на это время", taken)
    view = (await rival.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    check(all(at(s.get("start", "")) != at(start) for s in view.get("slots", [])),
          "держание: время не видно в booking.get", start)

    token = ""
    if telegram_mock is not None and telegram_url:
        offer_message = await telegram_mock.wait_for("sendMessage", chat, mark, contains="Освободилось время")
        buttons = (offer_message or {}).get("reply_markup", {}).get("inline_keyboard", [[{}]])
        link = buttons[0][0].get("url", "") if buttons and buttons[0] else ""
        check("/#/w/" in link, "бот: предложение времени с кнопкой «Записаться на это время»", offer_message)
        token = link.rsplit("/", 1)[-1] if "/#/w/" in link else ""
    mail_link = await mailed_link("booking.waitlist_offer", vera_email)
    if mail_link is not None:
        check("/#/w/" in mail_link, "письмо Вере: «Освободилось время» со ссылкой #/w/<токен>", mail_link)
        token = token or mail_link.rsplit("/", 1)[-1]
    if not token:
        print("  (пропущено: нет ни лога сервера, ни мока Telegram — ссылку листа ожидания не достать)")
        await finish()
        return
    got = (await waiter.call("booking.waitlistGet", {"token": token})).get("payload", {}).get("offer", {})
    check(got.get("status") == "offered" and at(got.get("start", "")) == at(start) and bool(got.get("expires_at"))
          and got.get("page_title") == page.get("title") and got.get("guest_name") == "Вера",
          "booking.waitlistGet: предложение offered, время, срок держания", got)
    booked_before = len(owner_events("waitlist.booked"))
    claimed = await waiter.call("booking.waitlistClaim", {"token": token}, timeout=40)
    info = claimed.get("payload", {})
    check(claimed.get("type") == "ok" and info.get("booking", {}).get("guest_name") == "Вера"
          and info.get("manage_url", "").endswith("/#/m/" + info.get("token", "-")),
          "booking.waitlistClaim: обычная запись (ссылка управления)", claimed)
    events = await wait_events("waitlist.booked", booked_before + 1)
    check(len(events) == booked_before + 1 and events[-1].get("payload", {}).get("entry_id") == entry.get("id"),
          "владелец получил waitlist.booked", events[-1:])
    after = (await waiter.call("booking.waitlistGet", {"token": token})).get("payload", {}).get("offer", {})
    check(after.get("status") == "claimed", "booking.waitlistGet после записи: claimed", after)
    twice = await waiter.call("booking.waitlistClaim", {"token": token}, timeout=40)
    check(twice.get("code") == "conflict", "повторный booking.waitlistClaim — conflict", twice)

    # Выход из листа (из письма) и удаление владельцем.
    leaver_email = f"s3.leave.{RUN}@example.com"
    await waiter.call("booking.waitlistJoin", {"slug": slug, "guest_name": "Лев", "guest_email": leaver_email})
    join_link = await mailed_link("booking.waitlist_joined", leaver_email)
    if join_link:
        left = await waiter.call("booking.waitlistLeave", {"token": join_link.rsplit("/", 1)[-1]})
        check(left.get("payload", {}).get("entry", {}).get("status") == "left",
              "booking.waitlistLeave по ссылке из письма — left", left)
    removed_email = f"s3.removed.{RUN}@example.com"
    removed = await waiter.call("booking.waitlistJoin", {"slug": slug, "guest_name": "Рита", "guest_email": removed_email})
    gone = await anna.call("booking.waitlistRemove",
                           {"entry_id": removed.get("payload", {}).get("entry", {}).get("id", 0)})
    check(gone.get("payload", {}).get("entry", {}).get("status") == "left", "booking.waitlistRemove — left", gone)

    # F10: «Мой налог» — оплата получена → tax.receipt и задача; чек отмечен; возврат.
    enabled = await anna.call("prefs.set", {"npd_receipts": True})
    check(enabled.get("payload", {}).get("npd_receipts") is True, "prefs.set npd_receipts = true", enabled)
    paid_page = (await anna.call("booking.pageCreate", dict(base, title=f"Самозанятый {RUN}", price=150000,
                                                           prepay="full",
                                                           payment_url="https://pay.example.com/e2e"))).get(
        "payload", {}).get("page", {})
    pages.append(paid_page.get("id", 0))
    pay_view = (await guest.call("booking.get", {"slug": paid_page.get("slug", "")}, timeout=40)).get(
        "payload", {}).get("page", {})
    pay_slots = [s.get("start", "") for s in pay_view.get("slots", []) if at(s.get("start", "")) > time.time() + 3 * 3600]
    if not pay_slots:
        check(False, "есть свободное окно для записи с оплатой", pay_view.get("slots", [])[:2])
        await finish()
        return
    paid_booking = await guest.call("booking.book", {"slug": paid_page.get("slug", ""), "start": pay_slots[-1],
                                                     "guest_name": "Тимур"}, timeout=40)
    paid_token = paid_booking.get("payload", {}).get("token", "")
    paid_list = (await anna.call("booking.list", {"page_id": paid_page.get("id", 0)})).get("payload", {}).get("bookings", [])
    paid_id = paid_list[0].get("id", 0) if paid_list else 0
    receipts_before = len(owner_events("tax.receipt"))
    marked = await anna.call("booking.paymentSet", {"booking_id": paid_id, "status": "paid"})
    check(marked.get("payload", {}).get("booking", {}).get("payment", {}).get("npd") is True,
          "оплата получена: payment.npd (владельцу напомнили выдать чек)", marked)
    events = await wait_events("tax.receipt", receipts_before + 1)
    receipt_event = events[-1].get("payload", {}) if events else {}
    service = paid_page.get("title", "")
    check(len(events) == receipts_before + 1 and receipt_event.get("booking_id") == paid_id
          and receipt_event.get("amount") == 150000 and receipt_event.get("client") == "Тимур"
          and receipt_event.get("text") == f"Наименование: {service}; Сумма: 1 500 ₽; Покупатель: физическое лицо"
          and receipt_event.get("url") == "https://lknpd.nalog.ru/",
          "tax.receipt: текст для «Мой налог», сумма, клиент", receipt_event)
    tasks = (await anna.call("tasks.list", {"limit": 200})).get("payload", {}).get("tasks", [])
    check(any(t.get("title") == f"Выдать чек: {service}, 1 500 ₽" and t.get("status") == "pending" for t in tasks),
          "задача «Выдать чек: <услуга>, <сумма>»", [t.get("title") for t in tasks][:5])
    pending = (await anna.call("tax.pending", {})).get("payload", {}).get("items", [])
    check(any(i.get("kind") == "booking" and i.get("id") == paid_id for i in pending),
          "tax.pending: оплата без отмеченного чека", pending[:2])
    receipt_url = f"https://lknpd.nalog.ru/api/v1/receipt/{RUN}/print"
    done = await anna.call("tax.receiptDone", {"booking_id": paid_id, "receipt_url": receipt_url})
    check(done.get("payload", {}).get("booking", {}).get("payment", {}).get("receipt_url") == receipt_url,
          "tax.receiptDone: receipt_url сохранён", done)
    bad_url = await anna.call("tax.receiptDone", {"booking_id": paid_id, "receipt_url": "http://insecure.example"})
    check(bad_url.get("code") == "bad_request", "tax.receiptDone: не https — bad_request", bad_url)
    managed = (await guest.call("booking.manage", {"token": paid_token})).get("payload", {}).get("booking", {})
    check(managed.get("receipt_url") == receipt_url and "npd" not in managed.get("payment", {}),
          "гость видит ссылку на чек (служебные поля скрыты)", managed.get("payment"))
    pending = (await anna.call("tax.pending", {})).get("payload", {}).get("items", [])
    check(all(i.get("id") != paid_id for i in pending), "tax.pending: после отметки чека — пусто", pending[:2])
    refunds_before = len(owner_events("tax.refund"))
    await anna.call("booking.paymentSet", {"booking_id": paid_id, "status": "refunded"})
    events = await wait_events("tax.refund", refunds_before + 1)
    check(len(events) == refunds_before + 1 and events[-1].get("payload", {}).get("receipt_url") == receipt_url,
          "возврат оплаты с чеком — tax.refund «Аннулируйте чек»", events[-1:])

    await finish()


async def main() -> int:
    print(f"Selfvoy E2E → {SERVER_URL} (прогон {RUN})\n")

    # Google mock (этап 9): поднимается только при SELFVOY_E2E_GOOGLE_MOCK=1,
    # C++-сервер при этом запущен с SELFVOY_GOOGLE_* на 127.0.0.1:<порт mock>.
    google_mock: GoogleMock | None = None
    if os.environ.get("SELFVOY_E2E_GOOGLE_MOCK") == "1":
        google_mock = GoogleMock(int(os.environ.get("SELFVOY_E2E_GOOGLE_MOCK_PORT", "9081")))
        await google_mock.start()

    # Push mock (этап 13): шлюз вебхуков, ловит apns-конверты.
    push_mock: PushMock | None = None
    if os.environ.get("SELFVOY_E2E_PUSH_MOCK") == "1":
        push_mock = PushMock(int(os.environ.get("SELFVOY_E2E_PUSH_MOCK_PORT", "9082")))
        await push_mock.start()

    # Telegram mock (F2): Bot API бота selfvoy_e2e_bot.
    telegram_mock: TelegramMock | None = None
    if os.environ.get("SELFVOY_E2E_TELEGRAM_MOCK") == "1":
        telegram_mock = TelegramMock(int(os.environ.get("SELFVOY_E2E_TELEGRAM_MOCK_PORT", "9083")))
        await telegram_mock.start()

    # ЮKassa mock (F4): API магазинов владельцев.
    yookassa_mock: YooKassaMock | None = None
    if os.environ.get("SELFVOY_E2E_YOOKASSA_MOCK") == "1":
        yookassa_mock = YooKassaMock(int(os.environ.get("SELFVOY_E2E_YOOKASSA_MOCK_PORT", "9084")))
        await yookassa_mock.start()

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
            "password": "selfvoy1234",
            "display_name": name,
        })
        check(registered.get("type") == "ok", f"регистрация {name}", registered)
        check(registered.get("payload", {}).get("requires_verification") is True,
              f"{name}: требуется подтверждение email")
        code = registered.get("payload", {}).get("email_code", "")
        check(len(code) == 6, f"{name}: выдан шестизначный код", code)
        verified = await client.call("auth.verifyEmail", {"email": email, "code": code})
        check(verified.get("payload", {}).get("verified") is True, f"{name}: email подтверждён")
        logged = await client.call("auth.login", {"email": email, "password": "selfvoy1234"})
        check(logged.get("type") == "ok", f"вход {name}", logged)
        check(logged.get("payload", {}).get("user", {}).get("role") in ("user", "moderator", "admin"),
              f"{name}: в user есть role", logged.get("payload", {}).get("user"))
        return logged.get("payload", {}).get("token", "")

    token = await register_and_login(anna, f"anna.{RUN}@example.com", "Анна")
    check(len(token.split(".")) == 3, "выдан JWT из трёх частей", token[:24])

    # Токен предъявляется заново — проверяет, что сессия записана в таблицу sessions
    fresh = Client("fresh")
    await fresh.connect()
    reauth = await fresh.call("auth.token", {"token": token})
    check(reauth.get("type") == "ok", "токен принят повторно (сессия есть в БД)", reauth)
    check(reauth.get("payload", {}).get("user", {}).get("email") == f"anna.{RUN}@example.com",
          "auth.token на свежем соединении отдаёт user", reauth.get("payload"))
    check(reauth.get("payload", {}).get("user", {}).get("role") == "user",
          "auth.token: user.role = user", reauth.get("payload", {}).get("user"))
    await fresh.close()

    # Управление сессиями: второй вход даёт вторую сессию; список, отзыв, проверка.
    extra = Client("extra")
    await extra.connect()
    second_login = await extra.call("auth.login", {"email": f"anna.{RUN}@example.com",
                                                   "password": "selfvoy1234"})
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
    # Открытое соединение отозванной сессии получает session.revoked и закрывается сразу.
    await asyncio.sleep(0.3)
    check(len(extra.events_of("session.revoked")) == 1,
          "отозванная сессия: открытому соединению пришло session.revoked", extra.events)
    check(extra.ws.close_code is not None, "отозванная сессия: соединение закрыто сервером",
          extra.ws.close_code)
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

    # Онбординг-опрос: день рождения + аллергии, флаг onboarded ставит сервер.
    survey = await anna.call("prefs.set", {"birthday": "1995-04-18",
                                           "allergies": ["орехи", "лактоза"]})
    check(survey.get("payload", {}).get("birthday") == "1995-04-18"
          and survey.get("payload", {}).get("allergies") == ["орехи", "лактоза"]
          and survey.get("payload", {}).get("onboarded") is True,
          "онбординг: birthday/allergies сохранены, onboarded=true",
          survey.get("payload"))

    # Профиль: имя/«о себе»/аватар — profile.update меняет только переданные поля.
    profile = await anna.call("profile.update", {"bio": "Делаю стартап про ИИ-агентов",
                                                 "avatar_url": "data:image/png;base64,iVBORw0KGgo="})
    profile_user = profile.get("payload", {}).get("user", {})
    check(profile.get("type") == "ok" and profile_user.get("bio") == "Делаю стартап про ИИ-агентов"
          and profile_user.get("display_name") == "Анна"
          and profile_user.get("avatar_url", "").startswith("data:image/png"),
          "profile.update: bio и аватар сохранены, имя не тронуто", profile)
    me_profile = await anna.call("auth.me", {})
    check(me_profile.get("payload", {}).get("bio") == "Делаю стартап про ИИ-агентов",
          "auth.me отдаёт bio", me_profile.get("payload", {}).get("bio"))
    bad_avatar = await anna.call("profile.update", {"avatar_url": "javascript:alert(1)"})
    check(bad_avatar.get("type") == "error" and bad_avatar.get("code") == "bad_request",
          "profile.update: недопустимый аватар отклонён", bad_avatar)
    empty_name = await anna.call("profile.update", {"display_name": "   "})
    check(empty_name.get("type") == "error", "profile.update: пустое имя отклонено", empty_name)

    # Приватность Selfvoy (prefs.privacy): по умолчанию все флаги true, частичное
    # обновление, неизвестный флаг — bad_request.
    privacy = (await anna.call("prefs.get", {})).get("payload", {}).get("privacy", {})
    check(privacy == {"share_free_slots_only": True, "hide_event_titles": True, "allow_place_choice": True},
          "prefs.privacy: значения по умолчанию", privacy)
    no_place = await anya.call("prefs.set", {"privacy": {"allow_place_choice": False}})
    check(no_place.get("payload", {}).get("privacy", {}).get("allow_place_choice") is False
          and no_place.get("payload", {}).get("privacy", {}).get("hide_event_titles") is True,
          "prefs.privacy: частичное обновление сохранило остальные флаги", no_place.get("payload"))
    bad_privacy = await anya.call("prefs.set", {"privacy": {"share_everything": True}})
    check(bad_privacy.get("type") == "error" and bad_privacy.get("code") == "bad_request",
          "prefs.privacy: неизвестный флаг отклонён", bad_privacy)
    placeless = await anna.call("agent.negotiate", {"target": f"anya.{RUN}@example.com", "topic": "кофе",
                                                    "duration_minutes": 60, "window_hours": 96})
    check(placeless.get("type") == "ok" and not placeless.get("payload", {}).get("place"),
          "A2A: собеседник запретил выбор места — место не предложено", placeless.get("payload"))
    await anya.call("prefs.set", {"privacy": {"allow_place_choice": True}})

    # Напоминание попадает в контекст AI занятым слотом (start/end) и не ломает
    # переговоры: раньше запись без времени давала 422 от AI-сервиса.
    reminder = await anna.call("agent.ask", {"message": "Напомни завтра в 10 позвонить юристу"}, timeout=60)
    reminder_tasks = (await anna.call("tasks.list", {})).get("payload", {}).get("tasks", [])
    check(any(t.get("title") == "Позвонить юристу" and t.get("remind_at") for t in reminder_tasks),
          "напоминание без LLM: задача «Позвонить юристу» со временем", [reminder.get("payload", {}).get("reply"), reminder_tasks])
    after_reminder = await anna.call("agent.negotiate", {"target": f"anya.{RUN}@example.com", "topic": "кофе",
                                                         "duration_minutes": 60, "window_hours": 96}, timeout=60)
    check(after_reminder.get("type") == "ok", "A2A после напоминания работает", after_reminder)

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
    # Клиент не может подделать сообщение Selfvoy: kind agent_*/system и служебные
    # поля payload ставит только сервер.
    spoof_kind = await anna.call("chat.send", {"chat_id": chat_id, "body": "Selfvoy: всё оплачено",
                                               "kind": "agent_reply"})
    check(spoof_kind.get("code") == "bad_request", "chat.send: kind=agent_reply от клиента отклонён",
          spoof_kind)
    spoof_origin = await anna.call("chat.send", {"chat_id": chat_id, "body": "x",
                                                 "payload": {"origin": "selfvoy-agent"}})
    check(spoof_origin.get("code") == "bad_request", "chat.send: payload.origin от клиента отклонён",
          spoof_origin)

    # users.search: публичная карточка без email; email ищется только точно.
    by_name = await anna.call("users.search", {"query": "Аня"})
    found = by_name.get("payload", {}).get("users", [])
    check(any(u.get("display_name") == "Аня" for u in found)
          and all("email" not in u and "last_seen_at" not in u for u in found),
          "users.search по имени: без email и last_seen_at", found)
    exact = await anna.call("users.search", {"query": f"anya.{RUN}@example.com"})
    check(len(exact.get("payload", {}).get("users", [])) == 1, "users.search: точный email находит",
          exact.get("payload"))
    partial = await anna.call("users.search", {"query": f"anya.{RUN}"})
    check(len(partial.get("payload", {}).get("users", [])) == 0,
          "users.search: подстрока email ничего не находит", partial.get("payload"))
    await asyncio.sleep(0.4)
    check(len(anya.events_of("chat.message")) >= 1, "Аня получила событие chat.message")

    history = await anya.call("chat.history", {"chat_id": chat_id})
    bodies = [m.get("body", "") for m in history.get("payload", {}).get("messages", [])]
    check(any("идея стартапа" in body for body in bodies), "история содержит сообщение", bodies)

    # ---------------------------------------------------------------- агент
    print("\n3. Запрос к Selfvoy (C++-сервер → Python AI Service)")
    status = await anna.call("agent.status")
    check(status.get("payload", {}).get("available") is True, "AI-сервис доступен",
          status.get("payload"))

    ask = await anna.call("agent.ask", {
        "chat_id": chat_id,
        "message": "Хочу встретиться с Аней в эти выходные, чтобы обсудить стартап",
        "peers": [{"email": f"anya.{RUN}@example.com"}],
    }, timeout=40)
    check(ask.get("type") == "ok", "Selfvoy ответил", ask)
    payload = ask.get("payload", {})
    print("    ответ Selfvoy:", json.dumps(payload, ensure_ascii=False)[:400])
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
    check(len(agent_messages) >= 2, "результат Selfvoy опубликован в чате",
          len(agent_messages))

    # ------------------------------------- приглашение по ссылке (invite.*)
    print("\n3a. Приглашение по ссылке: встреча с человеком без Selfvoy")
    created = await anna.call("invite.create", {
        "topic": f"Кофе по стартапу {RUN}",
        "duration_minutes": 45,
        "window_hours": 168,
        "note": "Можно у метро",
        "guest_name": "Борис",
    }, timeout=40)
    check(created.get("type") == "ok", "invite.create: ссылка создана", created)
    created_payload = created.get("payload", {})
    invite = created_payload.get("invite", {})
    token = created_payload.get("token", "")
    check(len(token) == 43, "токен ссылки — 43 символа base64url", len(token))
    url = created_payload.get("url", "")
    check(url == "" or url.endswith("/#/i/" + token), "url ведёт на #/i/<токен> (или пуст без SELFVOY_PUBLIC_URL)", url)
    offered = invite.get("slots", [])
    check(1 <= len(offered) <= 6, "предложено 1–6 слотов", offered)

    def minutes_of(slot_item: Dict[str, Any]) -> float:
        start = datetime.fromisoformat(slot_item["start"].replace("Z", "+00:00"))
        end = datetime.fromisoformat(slot_item["end"].replace("Z", "+00:00"))
        return (end - start).total_seconds() / 60

    check(all(minutes_of(item) == 45 for item in offered), "все слоты по 45 минут", offered)
    listed_invites = await anna.call("invite.list")
    invites_payload = listed_invites.get("payload", {})
    check(any(i.get("id") == invite.get("id") for i in invites_payload.get("invites", [])),
          "invite.list содержит приглашение", invites_payload)
    check(token not in json.dumps(invites_payload), "в списке владельца токена нет")

    # Гость — отдельное соединение без входа.
    guest = Client("guest")
    await guest.connect()
    seen = await guest.call("invite.get", {"token": token})
    check(seen.get("type") == "ok", "invite.get без входа", seen)
    guest_view = seen.get("payload", {}).get("invite", {})
    check(guest_view.get("owner_name") == "Анна" and guest_view.get("status") == "open",
          "гость видит организатора и статус open", guest_view)
    check(f"anna.{RUN}@example.com" not in json.dumps(seen), "email владельца гостю не отдаётся")
    missing = await guest.call("invite.get", {"token": "A" * 43})
    check(missing.get("code") == "not_found", "неизвестный токен — not_found", missing)
    no_auth = await guest.call("invite.create", {"topic": "чужое"})
    check(no_auth.get("code") == "unauthorized", "invite.create без входа — unauthorized", no_auth)

    chosen_start = (guest_view.get("slots") or offered or [{}])[0].get("start", "")
    accepted = await guest.call("invite.respond", {
        "token": token, "action": "accept", "start": chosen_start,
        "guest_name": "Борис", "guest_email": f"boris.{RUN}@example.com", "comment": "Буду вовремя",
    })
    check(accepted.get("type") == "ok", "гость выбрал время", accepted)
    accepted_view = accepted.get("payload", {}).get("invite", {})
    check(accepted_view.get("status") == "accepted"
          and accepted_view.get("chosen", {}).get("start") == chosen_start,
          "приглашение закрыто выбранным временем", accepted_view)
    again = await guest.call("invite.respond", {"token": token, "action": "decline", "guest_name": "Борис"})
    check(again.get("code") == "conflict", "повторный ответ — conflict", again)

    await asyncio.sleep(0.3)
    invite_events = [e for e in anna.events_of("notification.new")
                     if e.get("payload", {}).get("kind") == "invite.accepted"]
    check(bool(invite_events), "владелец получил notification.new invite.accepted",
          [e.get("payload", {}).get("kind") for e in anna.events_of("notification.new")])
    owner_tasks = (await anna.call("tasks.list", {})).get("payload", {}).get("tasks", [])
    meeting = [t for t in owner_tasks if t.get("title") == f"Встреча: Кофе по стартапу {RUN} — Борис"]
    def instant(value: str) -> datetime | None:
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00"))
        except (AttributeError, ValueError):
            return None

    check(bool(meeting) and instant(meeting[0].get("due_at", "")) == instant(chosen_start),
          "у владельца задача-встреча на выбранное время", [t.get("title") for t in owner_tasks])

    # Отказ и отмена.
    second_invite = await anna.call("invite.create", {"topic": f"Обед {RUN}", "duration_minutes": 30},
                                    timeout=40)
    second_token = second_invite.get("payload", {}).get("token", "")
    second_view = (await guest.call("invite.get", {"token": second_token})).get("payload", {}).get("invite", {})
    check(all(s.get("start") != chosen_start for s in second_view.get("slots", [])),
          "занятое время не предлагается в другом приглашении", second_view.get("slots"))
    declined = await guest.call("invite.respond", {"token": second_token, "action": "decline",
                                                   "guest_name": "Вера", "comment": "Уезжаю"})
    check(declined.get("payload", {}).get("invite", {}).get("status") == "declined",
          "гость отказался", declined)
    third_invite = await anna.call("invite.create", {"topic": f"Прогулка {RUN}"}, timeout=40)
    third_id = third_invite.get("payload", {}).get("invite", {}).get("id", 0)
    cancelled = await anna.call("invite.cancel", {"invite_id": third_id})
    check(cancelled.get("payload", {}).get("invite", {}).get("status") == "cancelled",
          "владелец отменил приглашение", cancelled)
    late = await guest.call("invite.respond", {
        "token": third_invite.get("payload", {}).get("token", ""), "action": "decline", "guest_name": "Гость",
    })
    check(late.get("code") == "conflict", "ответ на отменённое — conflict", late)
    foreign = await anya.call("invite.cancel", {"invite_id": third_id})
    check(foreign.get("code") == "not_found", "чужое приглашение не отменить", foreign)
    await guest.close()

    # ------------------------------------- страницы записи (booking.*) и рефералы
    print("\n3a+. Страница записи: гость записывается сам, перенос и отмена; рефералы")
    bad_page = await anna.call("booking.pageCreate", {"title": "", "duration_minutes": 60})
    check(bad_page.get("code") == "bad_request", "booking.pageCreate без названия — bad_request", bad_page)
    page_created = await anna.call("booking.pageCreate", {
        "title": f"Консультация {RUN}", "description": "Разбор проекта", "duration_minutes": 60,
        "days_ahead": 21, "max_per_day": 3, "notice_hours": 2, "max_series": 3,
    })
    check(page_created.get("type") == "ok", "booking.pageCreate: страница создана", page_created)
    page = page_created.get("payload", {}).get("page", {})
    slug = page.get("slug", "")
    check(len(slug) == 10 and all(c in "abcdefghijklmnopqrstuvwxyz0123456789" for c in slug),
          "адрес страницы — 10 символов [a-z0-9]", slug)
    pages = (await anna.call("booking.pageList")).get("payload", {}).get("pages", [])
    check(any(p.get("slug") == slug for p in pages), "booking.pageList содержит страницу", pages)

    guest = Client("booking-guest")
    await guest.connect()
    seen_page = await guest.call("booking.get", {"slug": slug}, timeout=40)
    check(seen_page.get("type") == "ok", "booking.get без входа", seen_page)
    guest_page = seen_page.get("payload", {}).get("page", {})
    page_slots = guest_page.get("slots", [])
    check(guest_page.get("owner_name") == "Анна" and bool(page_slots),
          "гость видит организатора и свободное время", guest_page)
    check(all(s.get("start") != chosen_start for s in page_slots),
          "время встречи из приглашения на странице не предлагается", page_slots[:3])
    check(f"anna.{RUN}@example.com" not in json.dumps(seen_page), "email владельца гостю не отдаётся")
    check(len(guest_page.get("ref", "")) == 8, "в виде гостя есть реферальный код владельца", guest_page.get("ref"))
    unknown_page = await guest.call("booking.get", {"slug": "zzzzzzzzzz"})
    check(unknown_page.get("code") == "not_found", "неизвестная страница — not_found", unknown_page)
    owner_only = await guest.call("booking.pageList")
    check(owner_only.get("code") == "unauthorized", "booking.pageList без входа — unauthorized", owner_only)

    first_slot = page_slots[0].get("start", "") if page_slots else ""
    booked = await guest.call("booking.book", {
        "slug": slug, "start": first_slot, "guest_name": "Глеб",
        "guest_email": f"gleb.{RUN}@example.com", "comment": "Про запуск", "count": 2,
    }, timeout=40)
    check(booked.get("type") == "ok", "гость записался на серию из двух встреч", booked)
    manage_token = booked.get("payload", {}).get("token", "")
    booked_view = booked.get("payload", {}).get("booking", {})
    occurrences = booked_view.get("occurrences", [])
    check(len(manage_token) == 43 and len(occurrences) == 2, "выдан токен ссылки управления, две встречи",
          booked_view)
    if len(occurrences) == 2:
        gap = instant(occurrences[1].get("start", "")) - instant(occurrences[0].get("start", ""))
        check(gap.total_seconds() == 7 * 86400, "вторая встреча серии — через неделю", occurrences)
    taken = await guest.call("booking.book", {"slug": slug, "start": first_slot, "guest_name": "Дина"},
                             timeout=40)
    check(taken.get("code") == "conflict", "занятое время второй раз не записать", taken)

    await asyncio.sleep(0.3)
    booking_events = [e for e in anna.events_of("notification.new")
                      if e.get("payload", {}).get("kind") == "booking.created"]
    check(bool(booking_events), "владелец получил notification.new booking.created",
          [e.get("payload", {}).get("kind") for e in anna.events_of("notification.new")])
    owner_tasks = (await anna.call("tasks.list", {"limit": 200})).get("payload", {}).get("tasks", [])
    booking_tasks = [t for t in owner_tasks if t.get("title") == f"Запись: Консультация {RUN} — Глеб"]
    check(len(booking_tasks) == 2, "у владельца задачи на обе встречи серии", [t.get("title") for t in owner_tasks])
    owner_bookings = (await anna.call("booking.list")).get("payload", {}).get("bookings", [])
    check(any(b.get("guest_email") == f"gleb.{RUN}@example.com" for b in owner_bookings),
          "booking.list: владелец видит запись и email гостя", owner_bookings)

    managed = await guest.call("booking.manage", {"token": manage_token}, timeout=40)
    managed_view = managed.get("payload", {}).get("booking", {})
    check(managed_view.get("can_cancel") is True and managed_view.get("can_reschedule") is True
          and managed_view.get("can_change") is True and managed_view.get("reschedules_left") == 3,
          "ссылка управления: можно перенести (3 раза) и отменить", managed_view)
    move_to = next((s.get("start") for s in managed_view.get("slots", []) if s.get("start") != first_slot), "")
    moved = await guest.call("booking.reschedule", {"token": manage_token, "start": move_to}, timeout=40)
    moved_view = moved.get("payload", {}).get("booking", {})
    check(moved.get("type") == "ok" and any(o.get("start") == move_to for o in moved_view.get("occurrences", [])),
          "ближайшая встреча перенесена", moved)
    check(moved_view.get("reschedules_left") == 2, "переносов осталось 2", moved_view.get("reschedules_left"))
    refreed = (await guest.call("booking.get", {"slug": slug}, timeout=40)).get("payload", {}).get("page", {})
    check(any(s.get("start") == first_slot for s in refreed.get("slots", [])),
          "после переноса прежнее время снова свободно", refreed.get("slots", [])[:3])
    cancelled_one = await guest.call("booking.cancel", {
        "token": manage_token, "occurrence": occurrences[1].get("start", "") if len(occurrences) == 2 else "",
    })
    check(cancelled_one.get("payload", {}).get("booking", {}).get("status") == "active",
          "отменена одна встреча серии, запись активна", cancelled_one)
    cancelled_all = await guest.call("booking.cancel", {"token": manage_token, "reason": "Планы изменились"})
    cancelled_view = cancelled_all.get("payload", {}).get("booking", {})
    check(cancelled_view.get("status") == "cancelled" and cancelled_view.get("cancelled_by") == "guest",
          "гость отменил запись целиком", cancelled_view)
    twice = await guest.call("booking.cancel", {"token": manage_token})
    check(twice.get("code") == "conflict", "повторная отмена — conflict", twice)
    resent = await guest.call("booking.resendLink", {"slug": slug, "email": f"nobody.{RUN}@example.com"})
    check(resent.get("payload", {}).get("sent") is True,
          "booking.resendLink: одинаковый ответ для неизвестного email", resent)
    after_tasks = (await anna.call("tasks.list", {"limit": 200})).get("payload", {}).get("tasks", [])
    check(not any(t.get("title") == f"Запись: Консультация {RUN} — Глеб" for t in after_tasks),
          "задачи отменённых встреч удалены", [t.get("title") for t in after_tasks])

    # Владелец отменяет запись; пауза и удаление страницы.
    owner_side = await guest.call("booking.book", {"slug": slug, "start": first_slot, "guest_name": "Ева"},
                                  timeout=40)
    eva_id = 0
    for item in (await anna.call("booking.list")).get("payload", {}).get("bookings", []):
        if item.get("guest_name") == "Ева":
            eva_id = item.get("id", 0)
    check(owner_side.get("type") == "ok" and eva_id > 0, "вторая запись (Ева)", owner_side)
    foreign_cancel = await anya.call("booking.ownerCancel", {"booking_id": eva_id})
    check(foreign_cancel.get("code") == "not_found", "чужую запись не отменить", foreign_cancel)
    by_owner = await anna.call("booking.ownerCancel", {"booking_id": eva_id})
    check(by_owner.get("payload", {}).get("booking", {}).get("cancelled_by") == "owner",
          "владелец отменил запись", by_owner)
    paused = await anna.call("booking.pageUpdate", {"page_id": page.get("id", 0), "status": "paused"})
    check(paused.get("payload", {}).get("page", {}).get("status") == "paused", "страница на паузе", paused)
    paused_book = await guest.call("booking.book", {"slug": slug, "start": first_slot, "guest_name": "Ева"},
                                   timeout=40)
    check(paused_book.get("code") == "conflict", "на паузе записаться нельзя", paused_book)
    deleted = await anna.call("booking.pageDelete", {"page_id": page.get("id", 0)})
    check(deleted.get("payload", {}).get("deleted") is True, "страница удалена", deleted)
    gone = await guest.call("booking.get", {"slug": slug})
    check(gone.get("code") == "not_found", "удалённая страница — not_found", gone)
    await guest.close()

    await f6_slots_post_and_ics(anna, anya)

    # Рефералы: регистрация по коду Анны засчитывается после подтверждения email.
    referral = (await anna.call("referral.get")).get("payload", {})
    ref_code = referral.get("code", "")
    check(len(ref_code) == 8 and ref_code == guest_page.get("ref"), "referral.get: код Анны", referral)
    newcomer = Client("referral-new")
    await newcomer.connect()
    friend_email = f"friend.{RUN}@example.com"
    friend = await newcomer.call("auth.register", {
        "email": friend_email, "password": "selfvoy1234", "display_name": "Фёдор",
        "ref": ref_code.lower(), "ref_source": "booking",
    })
    check(friend.get("type") == "ok", "регистрация по реферальному коду", friend)
    pending_ref = (await anna.call("referral.get")).get("payload", {})
    check(pending_ref.get("joined", 0) >= 1 and pending_ref.get("verified", -1) == 0,
          "приглашённый виден, но ещё не подтверждён", pending_ref)
    await newcomer.call("auth.verifyEmail", {"email": friend_email,
                                             "code": friend.get("payload", {}).get("email_code", "")})
    await asyncio.sleep(0.3)
    done_ref = (await anna.call("referral.get")).get("payload", {})
    check(done_ref.get("verified") == 1 and done_ref.get("referrals", [{}])[0].get("display_name") == "Фёдор",
          "после подтверждения email приглашённый засчитан", done_ref)
    check(friend_email not in json.dumps(done_ref), "email приглашённого пригласившему не отдаётся")
    joined_events = [e for e in anna.events_of("notification.new")
                     if e.get("payload", {}).get("kind") == "referral.joined"]
    check(bool(joined_events), "Анна получила notification.new referral.joined",
          [e.get("payload", {}).get("kind") for e in anna.events_of("notification.new")])
    # Награда (F3): обоим +30 дней Pro, один раз.
    month_later = time.time() + 30 * 86400

    def near_month(plan: Dict[str, Any]) -> bool:
        until = instant(plan.get("until", ""))
        return plan.get("name") == "pro" and until is not None and abs(until.timestamp() - month_later) < 600

    check("+1 месяц Pro" in (joined_events[-1].get("payload", {}).get("body", "") if joined_events else ""),
          "referral.joined: «+1 месяц Pro» в тексте", joined_events[-1:] if joined_events else [])
    anna_plan = (await anna.call("auth.me")).get("payload", {}).get("plan", {})
    friend_login = await newcomer.call("auth.login", {"email": friend_email, "password": "selfvoy1234"})
    friend_plan = friend_login.get("payload", {}).get("user", {}).get("plan", {})
    check(near_month(anna_plan) and near_month(friend_plan), "награда: у обоих Pro примерно на 30 дней",
          {"anna": anna_plan, "friend": friend_plan})
    check(done_ref.get("rewarded") == 1 and done_ref.get("reward_days") == 30 and done_ref.get("reward_cap") == 12
          and done_ref.get("plan", {}).get("name") == "pro", "referral.get: rewarded=1, reward_days, reward_cap, plan",
          done_ref)
    await asyncio.sleep(0.3)
    reward_events = [e for e in newcomer.events_of("notification.new")
                     if e.get("payload", {}).get("kind") == "referral.reward"]
    friend_notes = (await newcomer.call("notifications.list")).get("payload", {}).get("notifications", [])
    check(any(n.get("kind") == "referral.reward" and n.get("title") == "Месяц Pro в подарок" for n in friend_notes),
          "приглашённый получил referral.reward «Месяц Pro в подарок»", friend_notes[:3] or reward_events)
    await newcomer.call("auth.verifyEmail", {"email": friend_email,
                                             "code": friend.get("payload", {}).get("email_code", "")})
    again_ref = (await anna.call("referral.get")).get("payload", {})
    again_plan = (await anna.call("auth.me")).get("payload", {}).get("plan", {})
    check(again_ref.get("rewarded") == 1 and again_plan.get("until") == anna_plan.get("until"),
          "повторная верификация не начисляет награду дважды", {"ref": again_ref.get("rewarded"),
                                                               "until": again_plan.get("until")})
    await newcomer.close()

    # ------------------------------------------------------ Telegram-бот (F2)
    print("\n3b. Telegram-бот: привязка владельца и гостя, уведомления, /stop")
    me_plan = (await anna.call("auth.me")).get("payload", {}).get("plan", {})
    check(me_plan.get("name") in ("free", "pro") and "until" in me_plan and "enforced" in me_plan,
          "auth.me: в user есть plan {name, until, enforced}", me_plan)
    bot_name = (await anna.call("server.info")).get("payload", {}).get("telegram_bot", "")
    if telegram_mock is None or not bot_name:
        link_off = await anna.call("telegram.link")
        if not bot_name:
            check(link_off.get("code") == "bad_request", "бот не настроен: telegram.link — bad_request", link_off)
        print("  — пропуск сценария Telegram: нужны SELFVOY_E2E_TELEGRAM_MOCK=1 и SELFVOY_TELEGRAM_* у сервера")
    else:
        tg = telegram_mock
        owner_chat, guest_chat, stranger_chat = 5550001, 5550002, 5550009
        check(bot_name == "selfvoy_e2e_bot", "server.info: telegram_bot", bot_name)
        tg_status = (await anna.call("telegram.status")).get("payload", {})
        check(tg_status.get("enabled") is True and tg_status.get("linked") is False
              and tg_status.get("bot_username") == "selfvoy_e2e_bot", "telegram.status: бот включён, чат не привязан",
              tg_status)
        tg_link = (await anna.call("telegram.link")).get("payload", {})
        tg_url = tg_link.get("url", "")
        owner_payload = tg_url.split("?start=", 1)[1] if "?start=" in tg_url else ""
        check(tg_url.startswith("https://t.me/selfvoy_e2e_bot?start=o_") and len(owner_payload) <= 64
              and bool(tg_link.get("expires_at")), "telegram.link: deep link владельца на 15 минут", tg_link)

        # Подделка и «продлённый» срок отвергаются, чат не привязывается.
        forged = owner_payload[:-1] + ("A" if owner_payload[-1:] != "A" else "B")
        mark = len(tg.sent)
        tg.text(stranger_chat, "/start " + forged)
        rejected = await tg.wait_for("sendMessage", stranger_chat, mark)
        check(rejected is not None and "недействительна" in rejected.get("text", ""),
              "поддельная подпись владельца отвергнута", rejected)
        parts = owner_payload.split("_", 3)
        if len(parts) == 4:
            mark = len(tg.sent)
            tg.text(stranger_chat, "/start " + "_".join([parts[0], parts[1], str(int(time.time()) - 60), parts[3]]))
            expired = await tg.wait_for("sendMessage", stranger_chat, mark)
            check(expired is not None and "недействительна" in expired.get("text", ""),
                  "ссылка с изменённым (прошедшим) сроком отвергнута", expired)
        check((await anna.call("telegram.status")).get("payload", {}).get("linked") is False,
              "после поддельных ссылок чат не привязан")

        mark = len(tg.sent)
        tg.text(owner_chat, "/start " + owner_payload, username="anna_e2e")
        linked_reply = await tg.wait_for("sendMessage", owner_chat, mark)
        check(linked_reply is not None and linked_reply.get("text", "").startswith("Готово: сюда будут приходить"),
              "бот ответил владельцу после /start o_…", linked_reply)
        tg_status = (await anna.call("telegram.status")).get("payload", {})
        check(tg_status.get("linked") is True and tg_status.get("username") == "anna_e2e",
              "telegram.status.linked после привязки", tg_status)

        # Новая запись — уведомление владельцу в Telegram; гость получает telegram_url.
        tg_page = (await anna.call("booking.pageCreate", {
            "title": f"Telegram {RUN}", "duration_minutes": 30, "days_ahead": 14, "max_per_day": 4,
            "notice_hours": 1,
        })).get("payload", {}).get("page", {})
        tg_guest = Client("tg-guest")
        await tg_guest.connect()
        tg_slots = (await tg_guest.call("booking.get", {"slug": tg_page.get("slug", "")}, timeout=40)) \
            .get("payload", {}).get("page", {}).get("slots", [])
        mark = len(tg.sent)
        tg_booked = await tg_guest.call("booking.book", {
            "slug": tg_page.get("slug", ""), "start": tg_slots[0].get("start", "") if tg_slots else "",
            "guest_name": "Тимур", "guest_email": f"timur.{RUN}@example.com",
        }, timeout=40)
        tg_booked_payload = tg_booked.get("payload", {})
        guest_url = tg_booked_payload.get("telegram_url", "")
        tg_token = tg_booked_payload.get("token", "")
        check(guest_url.startswith("https://t.me/selfvoy_e2e_bot?start=g_")
              and tg_booked_payload.get("booking", {}).get("telegram_linked") is False,
              "booking.book: telegram_url гостя, чат ещё не привязан", tg_booked_payload)
        owner_note = await tg.wait_for("sendMessage", owner_chat, mark, contains="Тимур")
        check(owner_note is not None and f"Telegram {RUN}" in owner_note.get("text", ""),
              "владелец получил в Telegram уведомление о новой записи", owner_note)

        # Гость: поддельная подпись — отказ; настоящая ссылка — привязка и ближайшая встреча.
        guest_payload = guest_url.split("?start=", 1)[1] if "?start=" in guest_url else ""
        mark = len(tg.sent)
        tg.text(stranger_chat, "/start " + guest_payload[:-2] + ("xx" if guest_payload[-2:] != "xx" else "yy"))
        guest_forged = await tg.wait_for("sendMessage", stranger_chat, mark)
        check(guest_forged is not None and "недействительна" in guest_forged.get("text", ""),
              "поддельная ссылка гостя отвергнута", guest_forged)
        mark = len(tg.sent)
        tg.text(guest_chat, "/start " + guest_payload)
        guest_reply = await tg.wait_for("sendMessage", guest_chat, mark)
        check(guest_reply is not None and guest_reply.get("text", "").startswith("Готово: напоминания о записи")
              and "Ближайшая встреча" in guest_reply.get("text", ""),
              "гость подключил Telegram: ответ с ближайшей встречей", guest_reply)
        tg_view = (await tg_guest.call("booking.manage", {"token": tg_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(tg_view.get("telegram_linked") is True and tg_view.get("telegram_url") == guest_url,
              "booking.manage: telegram_linked и telegram_url", tg_view)

        # «Приду» (F1): из чужого чата — отказ.
        tg_id = 0
        for item in (await anna.call("booking.list", {"page_id": tg_page.get("id", 0)})) \
                .get("payload", {}).get("bookings", []):
            tg_id = item.get("id", 0)
        start_unix = int(instant(tg_view.get("occurrences", [{}])[0].get("start", "")).timestamp()) \
            if tg_view.get("occurrences") else 0
        mark = len(tg.sent)
        tg.callback(stranger_chat, f"cf:{tg_id}:{start_unix}")
        foreign_cb = await tg.wait_for("answerCallbackQuery", None, mark)
        check(foreign_cb is not None and "не для этого чата" in foreign_cb.get("text", ""),
              "кнопка «Приду» из чужого чата отвергнута", foreign_cb)

        # Отмена владельцем — извещение гостю и в Telegram; /stop отвязывает.
        mark = len(tg.sent)
        await anna.call("booking.ownerCancel", {"booking_id": tg_id})
        guest_notice = await tg.wait_for("sendMessage", guest_chat, mark, contains="Организатор отменил встречу")
        check(guest_notice is not None and "Тимур" not in guest_notice.get("text", ""),
              "извещение об отмене пришло гостю в Telegram (без имени гостя)", guest_notice)
        mark = len(tg.sent)
        tg.text(guest_chat, "/stop")
        stop_reply = await tg.wait_for("sendMessage", guest_chat, mark)
        check(stop_reply is not None and "больше не пишу" in stop_reply.get("text", ""), "/stop: бот подтвердил",
              stop_reply)
        tg_view = (await tg_guest.call("booking.manage", {"token": tg_token}, timeout=40)) \
            .get("payload", {}).get("booking", {})
        check(tg_view.get("telegram_linked") is False, "после /stop чат гостя отвязан", tg_view)
        await tg_guest.close()

        # 403 (бот заблокирован) — чат владельца отвязывается автоматически.
        tg.blocked.add(owner_chat)
        trigger = Client("tg-guest2")
        await trigger.connect()
        slots2 = (await trigger.call("booking.get", {"slug": tg_page.get("slug", "")}, timeout=40)) \
            .get("payload", {}).get("page", {}).get("slots", [])
        await trigger.call("booking.book", {"slug": tg_page.get("slug", ""),
                                            "start": slots2[0].get("start", "") if slots2 else "",
                                            "guest_name": "Ульяна"}, timeout=40)
        await trigger.close()
        unlinked_by_403 = False
        for _ in range(50):
            if (await anna.call("telegram.status")).get("payload", {}).get("linked") is False:
                unlinked_by_403 = True
                break
            await asyncio.sleep(0.2)
        check(unlinked_by_403, "403 от Telegram: чат владельца отвязан")
        tg.blocked.discard(owner_chat)

        # Снова привязать и отвязать через telegram.unlink.
        relink = (await anna.call("telegram.link")).get("payload", {}).get("url", "")
        mark = len(tg.sent)
        tg.text(owner_chat, "/start " + relink.split("?start=", 1)[-1], username="anna_e2e")
        await tg.wait_for("sendMessage", owner_chat, mark)
        check((await anna.call("telegram.status")).get("payload", {}).get("linked") is True,
              "повторная привязка владельца")
        tg_unlinked = await anna.call("telegram.unlink")
        check(tg_unlinked.get("payload", {}).get("linked") is False
              and (await anna.call("telegram.status")).get("payload", {}).get("linked") is False,
              "telegram.unlink отвязывает чат", tg_unlinked)
        check(not tg.bad_token and tg.polls >= 1, "бот опрашивает getUpdates с верным токеном",
              {"polls": tg.polls, "bad": tg.bad_token[:2]})
        await anna.call("booking.pageDelete", {"page_id": tg_page.get("id", 0)})

    await s1b_reminders_confirm_branding(anna, telegram_mock)
    await f7_ai_admin(anna, telegram_mock)

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
        "message": "Отправь письмо team@selfvoy.io с темой встреча",
    }, timeout=40)
    deny_results = deny_ask.get("payload", {}).get("results", [])
    check(any(r.get("denied") for r in deny_results),
          "send_email в режиме deny отклонён без исполнения", deny_results)

    # Режим ask: возвращаем send_email в ask — намерение уходит в подтверждение,
    # затем отклоняем его через confirmation.deny.
    await anna.call("permissions.set", {"tool": "send_email", "mode": "ask"})
    # Прямой tool.run тоже проходит барьер: ask → отложенное действие, не исполнение.
    direct = await anna.call("tool.run", {"tool": "send_email",
                                          "args": {"to": "team@selfvoy.io", "subject": "x", "body": "y"}})
    check(direct.get("type") == "ok" and direct.get("payload", {}).get("requires_confirmation") is True
          and direct.get("payload", {}).get("confirmation_id", 0) > 0,
          "tool.run send_email в режиме ask требует подтверждения", direct)
    if direct.get("payload", {}).get("confirmation_id"):
        await anna.call("confirmation.deny", {"id": direct["payload"]["confirmation_id"]})
    ask_again = await anna.call("agent.ask", {
        "message": "Отправь письмо team@selfvoy.io с темой встреча",
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

    # ------------------------------------------- уведомления и push (этап 13)
    print("\n3e. Уведомления и push-доставка (этап 13)")

    # Входы Анны уже создали уведомления login.new.
    listed = await anna.call("notifications.list", {})
    items = listed.get("payload", {}).get("notifications", [])
    check(any(n.get("kind") == "login.new" for n in items),
          "notifications.list содержит login.new", [n.get("kind") for n in items])
    check(listed.get("payload", {}).get("unread", 0) >= 1, "есть непрочитанные",
          listed.get("payload", {}).get("unread"))

    # Регистрация push-устройства (webhook-платформа ловится PushMock).
    reg = await anna.call("devices.push.register",
                          {"platform": "webhook", "token": "e2e-device-1"})
    device_id = reg.get("payload", {}).get("id", 0)
    check(reg.get("type") == "ok" and device_id > 0,
          "devices.push.register принял токен", reg)
    devices = await anna.call("devices.push.list", {})
    device_items = devices.get("payload", {}).get("devices", [])
    check(len(device_items) == 1 and "token" not in device_items[0],
          "devices.push.list без токена", device_items)

    # Напоминание с прошедшим сроком → уведомление task.due + push-конверт.
    envelopes_before = len(push_mock.envelopes) if push_mock else 0
    await anna.call("tasks.create", {"title": "Полить цветы",
                                     "remind_at": "2020-01-01T00:00:00.000Z"})
    due2 = await anna.call("tasks.due", {})
    check(due2.get("payload", {}).get("sent", 0) >= 1, "планировщик сработал повторно",
          due2.get("payload"))
    await asyncio.sleep(0.3)

    unread = await anna.call("notifications.list", {"unread": True})
    unread_items = unread.get("payload", {}).get("notifications", [])
    check(any(n.get("kind") == "task.due" for n in unread_items),
          "уведомление task.due создано", [n.get("kind") for n in unread_items])

    if push_mock is not None:
        check(len(push_mock.envelopes) > envelopes_before,
              "push-шлюз получил конверт", len(push_mock.envelopes))
        if push_mock.envelopes:
            envelope = push_mock.envelopes[-1]
            check(envelope.get("token") == "e2e-device-1"
                  and envelope.get("kind") == "task.due"
                  and envelope.get("apns", {}).get("payload", {})
                      .get("aps", {}).get("alert", {}).get("title") == "Напоминание",
                  "конверт: токен, kind и aps.alert", envelope)

    # Прочитать все → непрочитанных нет.
    read_all = await anna.call("notifications.read", {})
    check(read_all.get("payload", {}).get("unread", -1) == 0,
          "notifications.read очистил непрочитанные", read_all.get("payload"))

    # Отзыв устройства.
    revoked_device = await anna.call("devices.push.revoke", {"id": device_id})
    check(revoked_device.get("payload", {}).get("revoked") is True,
          "devices.push.revoke отозвал устройство", revoked_device.get("payload"))

    # Этап 11 (Android): платформа 'fcm' → конверт с секцией fcm (FCM HTTP v1).
    fcm_reg = await anna.call("devices.push.register",
                              {"platform": "fcm", "token": "e2e-fcm-device-1"})
    fcm_device_id = fcm_reg.get("payload", {}).get("id", 0)
    check(fcm_reg.get("type") == "ok" and fcm_device_id > 0,
          "devices.push.register принял fcm-токен", fcm_reg)
    envelopes_before_fcm = len(push_mock.envelopes) if push_mock else 0
    await anna.call("tasks.create", {"title": "Проверить FCM",
                                     "remind_at": "2020-01-02T00:00:00.000Z"})
    await anna.call("tasks.due", {})
    await asyncio.sleep(0.3)
    if push_mock is not None:
        fcm_envelopes = [e for e in push_mock.envelopes[envelopes_before_fcm:]
                         if e.get("platform") == "fcm"]
        check(len(fcm_envelopes) >= 1, "push-шлюз получил fcm-конверт",
              len(push_mock.envelopes) - envelopes_before_fcm)
        if fcm_envelopes:
            message = fcm_envelopes[-1].get("fcm", {}).get("message", {})
            check(message.get("token") == "e2e-fcm-device-1"
                  and message.get("notification", {}).get("title") == "Напоминание"
                  and message.get("android", {}).get("notification", {})
                      .get("channel_id") == "selfvoy"
                  and "url" in fcm_envelopes[-1].get("fcm", {}),
                  "fcm-конверт: token, notification, android.channel_id, url",
                  fcm_envelopes[-1])
    await anna.call("devices.push.revoke", {"id": fcm_device_id})

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
          "STT-провайдер — mock (без SELFVOY_STT_URL)", stt_payload.get("provider"))
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

    # auth.refresh на неавторизованном соединении: новая пара токенов, и это же
    # соединение сразу авторизовано (клиент продлевает истёкший токен без переподключения).
    renew = Client("renew")
    await renew.connect()
    first = await renew.call("auth.login", {"email": vera_email, "password": "selfvoy1234"})
    vera_refresh = first.get("payload", {}).get("refresh_token", "")
    await renew.close()
    renew = Client("renew2")
    await renew.connect()
    before = await renew.call("auth.me", {})
    check(before.get("type") == "error", "без токена auth.me отклонён", before)
    refreshed = await renew.call("auth.refresh", {"refresh_token": vera_refresh})
    check(refreshed.get("type") == "ok" and refreshed.get("payload", {}).get("refresh_token") not in ("", vera_refresh),
          "auth.refresh выдал новую пару (ротация)", refreshed)
    after = await renew.call("auth.me", {})
    check(after.get("type") == "ok" and after.get("payload", {}).get("email") == vera_email,
          "после auth.refresh соединение авторизовано", after)
    await renew.close()

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

    # Повторный setup2fa при включённой 2FA не перезаписывает секрет.
    resetup = await vera.call("auth.setup2fa", {})
    check(resetup.get("code") == "bad_request", "2FA: setup2fa при включённой 2FA отклонён", resetup)

    status_on = await vera.call("auth.status2fa", {})
    check(status_on.get("payload", {}).get("enabled") is True,
          "2FA: включена после подтверждения", status_on.get("payload"))
    check(status_on.get("payload", {}).get("recovery_codes_left") == 10,
          "2FA: 10 резервных кодов доступно", status_on.get("payload"))

    # Вход без кода (и без доверенного устройства) → requires_2fa.
    gate = Client("vera-gate")
    await gate.connect()
    blocked = await gate.call("auth.login", {"email": vera_email, "password": "selfvoy1234"})
    check(blocked.get("code") == "requires_2fa",
          "2FA: вход без кода отклонён (requires_2fa)", blocked)
    await gate.close()

    # Вход по резервному коду; повторное использование того же кода отклоняется.
    first_code = recovery[0]
    rc_client = Client("vera-rc")
    await rc_client.connect()
    rc_login = await rc_client.call("auth.login2fa",
                                    {"email": vera_email, "password": "selfvoy1234", "code": first_code})
    check(rc_login.get("type") == "ok", "2FA: вход по резервному коду", rc_login)
    check(len(rc_login.get("payload", {}).get("token", "").split(".")) == 3,
          "2FA: по резервному коду выдан JWT")
    rc_reuse = await rc_client.call("auth.login2fa",
                                    {"email": vera_email, "password": "selfvoy1234", "code": first_code})
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
        "email": vera_email, "password": "selfvoy1234",
        "code": totp(secret, window_offset=1),
        "trust_device": True, "device_id": device_id,
    })
    check(totp_login.get("type") == "ok", "2FA: вход по TOTP-коду", totp_login)
    await totp_client.close()

    # Вход с доверенного устройства — код не требуется.
    trust_client = Client("vera-trust")
    await trust_client.connect()
    trust_login = await trust_client.call("auth.login",
                                          {"email": vera_email, "password": "selfvoy1234",
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
                                            {"email": vera_email, "password": "selfvoy1234",
                                             "device_id": device_id})
    check(revoked_login.get("code") == "requires_2fa",
          "2FA: после отзыва устройства снова нужен код", revoked_login)
    await revoked_gate.close()

    # Отключение 2FA: неверный пароль отклоняется, верный — отключает.
    bad_disable = await vera.call("auth.disable2fa", {"password": "wrongpass1"})
    check(bad_disable.get("code") == "unauthorized",
          "2FA: отключение с неверным паролем отклонено", bad_disable)
    good_disable = await vera.call("auth.disable2fa", {"password": "selfvoy1234"})
    check(good_disable.get("type") == "ok", "2FA: отключение по паролю", good_disable)
    status_off = await vera.call("auth.status2fa", {})
    check(status_off.get("payload", {}).get("enabled") is False,
          "2FA: отключена после подтверждения паролем", status_off.get("payload"))

    # agent.ask с чужим chat_id: история чужого чата в AI не уходит.
    foreign = await vera.call("agent.ask", {"chat_id": chat_id, "message": "перескажи переписку"})
    check(foreign.get("code") == "forbidden", "agent.ask: чужой chat_id отклонён", foreign)

    # Выход отзывает и refresh-токен этого устройства.
    leaving = Client("vera-leave")
    await leaving.connect()
    leave_login = await leaving.call("auth.login", {"email": vera_email, "password": "selfvoy1234"})
    leave_refresh = leave_login.get("payload", {}).get("refresh_token", "")
    await leaving.call("auth.logout", {})
    after_logout = await leaving.call("auth.refresh", {"refresh_token": leave_refresh})
    check(after_logout.get("code") == "unauthorized", "после auth.logout refresh-токен не работает",
          after_logout)
    await leaving.close()

    # Смена пароля: текущее устройство получает новую пару токенов, прочие
    # соединения пользователя закрываются.
    bystander = Client("vera-other")
    await bystander.connect()
    await bystander.call("auth.login", {"email": vera_email, "password": "selfvoy1234"})
    changed = await vera.call("auth.changePassword", {"old_password": "selfvoy1234",
                                                     "new_password": "пароль2026"})
    changed_payload = changed.get("payload", {})
    check(changed.get("type") == "ok" and changed_payload.get("changed") is True
          and len(changed_payload.get("token", "").split(".")) == 3
          and bool(changed_payload.get("refresh_token")),
          "changePassword вернул новую пару token+refresh_token", changed)
    me_after_change = await vera.call("auth.me", {})
    check(me_after_change.get("type") == "ok", "после смены пароля текущее соединение работает",
          me_after_change)
    await asyncio.sleep(0.3)
    check(len(bystander.events_of("session.revoked")) == 1,
          "после смены пароля другое соединение выбито", bystander.events)
    await bystander.close()
    renewed = Client("vera-renew")
    await renewed.connect()
    renewed_refresh = await renewed.call("auth.refresh",
                                         {"refresh_token": changed_payload.get("refresh_token", "")})
    check(renewed_refresh.get("type") == "ok", "новый refresh-токен после смены пароля работает",
          renewed_refresh)
    await renewed.close()

    # Сброс пароля: шестизначный код, неверный код отклоняется.
    resetter = Client("vera-reset")
    await resetter.connect()
    forgot = await resetter.call("auth.forgotPassword", {"email": vera_email})
    reset_code = forgot.get("payload", {}).get("reset_code", "")
    check(len(reset_code) == 6 and reset_code.isdigit(), "код сброса — 6 цифр", reset_code)
    wrong_code = "000000" if reset_code != "000000" else "111111"
    bad_reset = await resetter.call("auth.resetPassword", {"email": vera_email, "code": wrong_code,
                                                           "new_password": "сброс2026"})
    check(bad_reset.get("code") == "bad_request", "неверный код сброса отклонён", bad_reset)
    good_reset = await resetter.call("auth.resetPassword", {"email": vera_email, "code": reset_code,
                                                            "new_password": "сброс2026"})
    check(good_reset.get("payload", {}).get("reset") is True, "пароль сброшен шестизначным кодом",
          good_reset)
    relogin = await resetter.call("auth.login", {"email": vera_email, "password": "сброс2026"})
    check(relogin.get("type") == "ok", "вход с новым паролем (кириллица+цифры)", relogin)
    await resetter.close()
    await asyncio.sleep(0.3)
    check(len(vera.events_of("session.revoked")) == 1, "сброс пароля закрыл открытые соединения",
          vera.events_of("session.revoked"))
    await vera.close()

    # ------------------------------------------------ интеграции (этап 9)
    if google_mock is not None:
        print("\n5. Интеграции: Google OAuth 2.0 + PKCE (Календарь + Gmail)")
        import http.client
        from urllib.parse import parse_qs, urlparse

        async def google_connect(provider: str) -> Dict[str, Any]:
            """Полный OAuth-поток: begin → consent (mock 302) → callback."""
            begin = await anna.call("integrations.begin", {
                "provider": provider,
                "redirect_uri": f"http://127.0.0.1:{google_mock.port}/cb",
            })
            begin_payload = begin.get("payload", {})
            check(begin.get("type") == "ok", f"OAuth {provider}: begin", begin)
            parsed = urlparse(begin_payload.get("authorize_url", ""))
            check("code_challenge_method=S256" in parsed.query,
                  f"OAuth {provider}: PKCE S256 в ссылке", begin_payload.get("authorize_url"))
            # «Заходим» на consent-экран: mock делает 302 → redirect_uri?code&state.
            # Запрос в отдельном потоке: http.client блокирующий, а mock живёт
            # в том же event loop.
            def fetch_consent() -> tuple:
                conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80,
                                                  timeout=10)
                conn.request("GET", f"{parsed.path}?{parsed.query}")
                response = conn.getresponse()
                location = response.getheader("Location", "")
                conn.close()
                return response.status, location

            status, location = await asyncio.to_thread(fetch_consent)
            check(status == 302 and "code=" in location,
                  f"OAuth {provider}: consent-редирект с code", location)
            redirect_query = parse_qs(urlparse(location).query)
            done = await anna.call("integrations.callback", {
                "provider": provider,
                "code": redirect_query.get("code", [""])[0],
                "state": redirect_query.get("state", [""])[0],
            })
            check(done.get("type") == "ok", f"OAuth {provider}: callback с токенами", done)
            check(done.get("payload", {}).get("account") == "e2e-user@gmail.com",
                  f"OAuth {provider}: аккаунт из id_token", done.get("payload"))
            return done.get("payload", {})

        calendar_conn = await google_connect("google_calendar")

        # State одноразовый: повторный/чужой callback отклоняется.
        replay = await anna.call("integrations.callback", {
            "provider": "google_calendar", "code": "whatever", "state": "does-not-exist"})
        check(replay.get("code") == "bad_request", "OAuth: недействительный state отклонён", replay)

        listed = await anna.call("integrations.list")
        connections = listed.get("payload", {}).get("connections", [])
        check(any(c.get("provider") == "google_calendar" and c.get("status") == "active"
                  for c in connections), "интеграция активна в списке", connections)
        check("token_encrypted" not in json.dumps(listed) and "access-cal" not in json.dumps(listed),
              "integrations.list: токены наружу не отдаются", listed)

        # Синхронизация: mock отвечает 401 на первичный токен → сервер
        # обновляет access_token по refresh_token и повторяет запрос.
        sync = await anna.call("integrations.sync", {"id": calendar_conn.get("id")})
        check(sync.get("type") == "ok", "sync календаря (с авто-обновлением токена)", sync)
        check(sync.get("payload", {}).get("events") == 2,
              "sync: получено 2 события", sync.get("payload"))
        check(sync.get("payload", {}).get("facts") == 2,
              "sync: оба события сохранены в память", sync.get("payload"))
        check(google_mock.refreshed == 1,
              "refresh_token: ровно одно обновление", google_mock.refreshed)

        # Расписание легло в память: check_calendar берёт данные из Google.
        calendar_tool = await anna.call("tool.run", {"tool": "check_calendar", "args": {}})
        check(calendar_tool.get("payload", {}).get("source") == "google_calendar",
              "check_calendar: источник — Google", calendar_tool.get("payload"))
        check(len(calendar_tool.get("payload", {}).get("busy", [])) == 2,
              "check_calendar: 2 события из Google", calendar_tool.get("payload"))

        # Gmail: подключаем и отправляем письмо через реальный API-контракт.
        gmail_conn = await google_connect("google_gmail")
        allow_email = await anna.call("permissions.set", {"tool": "send_email", "mode": "allow"})
        check(allow_email.get("type") == "ok", "разрешение: send_email → allow", allow_email)
        sent = await anna.call("tool.run", {"tool": "send_email", "args": {
            "to": "friend@example.com", "subject": "E2E", "body": "Проверка интеграции"}})
        check(sent.get("payload", {}).get("provider") == "google_gmail",
              "send_email: отправлено через Gmail API", sent.get("payload"))
        check(sent.get("payload", {}).get("message_id") == "msg-e2e",
              "send_email: id сообщения от Gmail", sent.get("payload"))
        raw = google_mock.sent_raw[-1] if google_mock.sent_raw else ""
        decoded = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)).decode("utf-8", "replace")
        check("To: friend@example.com" in decoded and "Проверка интеграции" in decoded,
              "send_email: корректное RFC 2822 письмо", decoded[:80])

        # Отзыв доступа: у провайдера и локально.
        revoked = await anna.call("integrations.revoke", {"id": calendar_conn.get("id")})
        check(revoked.get("type") == "ok", "revoke: календарь отключён", revoked)
        check(google_mock.revoked_tokens == ["refresh-e2e"],
              "revoke: refresh_token отправлен Google", google_mock.revoked_tokens)
        again = await anna.call("integrations.revoke", {"id": calendar_conn.get("id")})
        check(again.get("code") == "not_found", "revoke: повторный отзыв — not_found", again)
        after = await anna.call("integrations.list")
        providers = [c.get("provider") for c in after.get("payload", {}).get("connections", [])]
        check(providers == ["google_gmail"], "список: остался только Gmail", providers)
        gmail_revoked = await anna.call("integrations.revoke", {"id": gmail_conn.get("id")})
        check(gmail_revoked.get("type") == "ok", "revoke: Gmail отключён", gmail_revoked)

    # ------------------------------------------------- администрирование
    print("\n5a. Администрирование (admin.*)")
    denied = await anna.call("admin.stats")
    check(denied.get("code") == "forbidden", "admin.stats: обычному пользователю — forbidden", denied)
    denied_users = await anna.call("admin.users", {"query": RUN})
    check(denied_users.get("code") == "forbidden", "admin.users: обычному пользователю — forbidden",
          denied_users)

    admin_email = os.environ.get("SELFVOY_E2E_ADMIN_EMAIL", "").strip().lower()
    admin_password = os.environ.get("SELFVOY_E2E_ADMIN_PASSWORD", "")
    if not (admin_email and admin_password):
        print("  — пропуск сценария администратора: задайте SELFVOY_E2E_ADMIN_EMAIL и "
              "SELFVOY_E2E_ADMIN_PASSWORD (а серверу — SELFVOY_ADMIN_EMAILS с этим адресом)")
    else:
        admin = Client("admin")
        await admin.connect()
        admin_login = await admin.call("auth.login", {"email": admin_email, "password": admin_password})
        if admin_login.get("code") == "unauthorized":
            # Аккаунта ещё нет: регистрируем (код подтверждения отдаёт dev-драйвер почты).
            registered = await admin.call("auth.register", {"email": admin_email, "password": admin_password,
                                                            "display_name": "Администратор"})
            code = registered.get("payload", {}).get("email_code", "")
            await admin.call("auth.verifyEmail", {"email": admin_email, "code": code})
            admin_login = await admin.call("auth.login", {"email": admin_email, "password": admin_password})
        admin_user = admin_login.get("payload", {}).get("user", {})
        check(admin_login.get("type") == "ok" and admin_user.get("role") == "admin",
              "администратор из SELFVOY_ADMIN_EMAILS вошёл с ролью admin", admin_login)

        stats = await admin.call("admin.stats")
        stats_payload = stats.get("payload", {})
        check(stats.get("type") == "ok" and all(key in stats_payload for key in (
            "users_total", "users_verified", "users_disabled", "users_2fa", "users_new_7d",
            "moderators", "admins", "sessions_active", "online_now")),
            "admin.stats: все счётчики на месте", stats_payload)
        check(stats_payload.get("admins", 0) >= 1 and stats_payload.get("online_now", 0) >= 2,
              "admin.stats: есть администратор, онлайн ≥ 2", stats_payload)
        listed = await admin.call("admin.users", {"query": RUN, "role": "", "status": "",
                                                  "offset": 0, "limit": 50})
        listed_emails = [u.get("email") for u in listed.get("payload", {}).get("users", [])]
        check(listed.get("payload", {}).get("total", 0) >= 2
              and f"anna.{RUN}@example.com" in listed_emails,
              "admin.users: поиск по прогону находит Анну", listed_emails)

        # Свежий пользователь: блокировка выбивает его открытое соединение и закрывает вход.
        victim_email = f"victim.{RUN}@example.com"
        victim = Client("victim")
        await victim.connect()
        await register_and_login(victim, victim_email, "Нарушитель")
        found = await admin.call("admin.users", {"query": victim_email})
        victim_id = (found.get("payload", {}).get("users") or [{}])[0].get("id", 0)
        check(victim_id > 0, "admin.users: нарушитель найден по email", found.get("payload"))
        blocked = await admin.call("admin.setActive", {"user_id": victim_id, "active": False,
                                                       "reason": "e2e: проверка блокировки"})
        check(blocked.get("type") == "ok"
              and blocked.get("payload", {}).get("user", {}).get("is_active") is False,
              "admin.setActive(false): пользователь заблокирован", blocked)
        try:
            await asyncio.wait_for(victim.ws.wait_closed(), timeout=5)
        except asyncio.TimeoutError:
            pass
        check(victim.ws.close_code == 4001, "открытое соединение заблокированного закрыто (4001)",
              victim.ws.close_code)
        await victim.close()
        retry = Client("retry")
        await retry.connect()
        relogin = await retry.call("auth.login", {"email": victim_email, "password": "selfvoy1234"})
        check(relogin.get("code") == "forbidden", "заблокированный не может войти", relogin)
        unblocked = await admin.call("admin.setActive", {"user_id": victim_id, "active": True, "reason": ""})
        check(unblocked.get("payload", {}).get("user", {}).get("is_active") is True,
              "admin.setActive(true): разблокирован", unblocked)
        relogin = await retry.call("auth.login", {"email": victim_email, "password": "selfvoy1234"})
        check(relogin.get("type") == "ok", "после разблокировки вход работает", relogin)

        # Модератор: видит обзор, но не меняет роли, не трогает админов и не читает журнал.
        promoted = await admin.call("admin.setRole", {"user_id": victim_id, "role": "moderator"})
        check(promoted.get("payload", {}).get("user", {}).get("role") == "moderator",
              "admin.setRole: назначен модератор", promoted)
        mod_stats = await retry.call("admin.stats")
        check(mod_stats.get("type") == "ok", "модератор видит admin.stats", mod_stats)
        mod_audit = await retry.call("admin.audit", {})
        check(mod_audit.get("code") == "forbidden", "модератору admin.audit — forbidden", mod_audit)
        mod_role = await retry.call("admin.setRole", {"user_id": victim_id, "role": "admin"})
        check(mod_role.get("code") == "forbidden", "модератор не меняет роли", mod_role)
        mod_block = await retry.call("admin.setActive", {"user_id": admin_user.get("id"), "active": False,
                                                         "reason": "бунт"})
        check(mod_block.get("code") == "forbidden", "модератор не блокирует администратора", mod_block)
        own_role = await admin.call("admin.setRole", {"user_id": admin_user.get("id"), "role": "user"})
        check(own_role.get("code") == "forbidden", "свою роль менять нельзя", own_role)

        journal = await admin.call("admin.audit", {"user_id": victim_id, "before_id": 0, "limit": 50})
        kinds = [entry.get("kind") for entry in journal.get("payload", {}).get("entries", [])]
        check("admin_user_disabled" in kinds and "admin_role_changed" in kinds,
              "admin.audit: блокировка и смена роли в журнале", kinds[:6])

        # Отзыв сессий: соединение модератора закрыто, роль снимаем обратно.
        revoked = await admin.call("admin.revokeSessions", {"user_id": victim_id})
        check(revoked.get("payload", {}).get("revoked", 0) >= 1, "admin.revokeSessions: сессии отозваны",
              revoked)
        try:
            await asyncio.wait_for(retry.ws.wait_closed(), timeout=5)
        except asyncio.TimeoutError:
            pass
        check(retry.ws.close_code == 4001, "соединение после отзыва сессий закрыто", retry.ws.close_code)
        await retry.close()
        demoted = await admin.call("admin.setRole", {"user_id": victim_id, "role": "user"})
        check(demoted.get("payload", {}).get("user", {}).get("role") == "user", "роль снята обратно",
              demoted)

        # Тариф Pro: admin.setPlan (только admin), plan в user.
        anna_found = await admin.call("admin.users", {"query": f"anna.{RUN}@example.com"})
        anna_id = (anna_found.get("payload", {}).get("users") or [{}])[0].get("id", 0)
        plan_set = await admin.call("admin.setPlan", {"user_id": anna_id, "days": 30})
        set_plan = plan_set.get("payload", {}).get("user", {}).get("plan", {})
        check(plan_set.get("type") == "ok" and set_plan.get("name") == "pro" and bool(set_plan.get("until")),
              "admin.setPlan: Анне выдан Pro на 30 дней", plan_set)
        anna_plan = (await anna.call("auth.me")).get("payload", {}).get("plan", {})
        until_ok = False
        if anna_plan.get("until"):
            left = instant(anna_plan["until"]).timestamp() - time.time()
            until_ok = 29 * 86400 < left <= 30 * 86400 + 120
        check(anna_plan.get("name") == "pro" and until_ok, "auth.me: plan.name = pro, until ≈ +30 дней", anna_plan)
        plan_denied = await anna.call("admin.setPlan", {"user_id": anna_id, "days": 365})
        check(plan_denied.get("code") == "forbidden", "admin.setPlan обычному пользователю — forbidden",
              plan_denied)
        plan_bad = await admin.call("admin.setPlan", {"user_id": anna_id, "days": 4000})
        check(plan_bad.get("code") == "bad_request", "admin.setPlan: days вне 0–3650 — bad_request", plan_bad)
        plan_reset = await admin.call("admin.setPlan", {"user_id": anna_id, "days": 0})
        reset_plan = plan_reset.get("payload", {}).get("user", {}).get("plan", {})
        check(reset_plan.get("name") == "free" and reset_plan.get("until") == "",
              "admin.setPlan days=0: снова Free", plan_reset)
        plan_journal = await admin.call("admin.audit", {"user_id": anna_id, "before_id": 0, "limit": 20})
        check(any(e.get("kind") == "admin_plan_changed" for e in plan_journal.get("payload", {}).get("entries", [])),
              "admin.audit: смена тарифа в журнале", plan_journal.get("payload", {}).get("entries", [])[:3])
        await admin.close()

    await s2a_payments(anna, yookassa_mock)
    await s2b_packages(anna, yookassa_mock)
    await s3_waitlist_rebook_tax(anna, telegram_mock)

    # -------------------------------------------------------------- итоги
    # ---------------------------------------- безопасность (этап 14)
    print("\n5b. Сценарии безопасности (этап 14)")

    # SQL-инъекция против реального PostgreSQL: текст хранится буквально
    # (параметризованные запросы), таблица жива.
    sqli = "'); DROP TABLE memories; -- <script>alert(1)</script>"
    evil_memory = await anna.call("memory.add", {"text": sqli, "kind": "fact"})
    check(evil_memory.get("payload", {}).get("saved", 0) == 1,
          "SQLi-текст сохранён в память буквально", evil_memory.get("payload"))
    memory_back = await anna.call("memory.list", {"query": "DROP"})
    texts = [m.get("text") for m in memory_back.get("payload", {}).get("entries", [])]
    check(sqli in texts, "SQLi round-trip без искажений", texts[:1])
    alive = await anna.call("memory.list", {})
    check(alive.get("type") == "ok", "таблица memories жива после инъекции",
          alive.get("type"))

    # Инъекция в поиск пользователей: пустой результат, без ошибки.
    inject_search = await anna.call("users.search", {"query": "' OR '1'='1"})
    check(inject_search.get("type") == "ok"
          and len(inject_search.get("payload", {}).get("users", [])) == 0,
          "users.search: инъекция даёт пустой результат", inject_search.get("payload"))

    # Изоляция памяти: Аня не видит воспоминания Анны.
    anya_memory = await anya.call("memory.list", {})
    anya_texts = [m.get("text") for m in anya_memory.get("payload", {}).get("entries", [])]
    check(sqli not in anya_texts, "Аня не видит память Анны", len(anya_texts))

    # Брутфорс: серия неверных паролей блокирует вход по email.
    for _ in range(8):
        await anya.call("auth.login", {"email": f"anna.{RUN}@example.com",
                                       "password": "wrong-password"})
    blocked = await anya.call("auth.login", {"email": f"anna.{RUN}@example.com",
                                             "password": "selfvoy1234"})
    check(blocked.get("code") == "forbidden",
          "вход заблокирован после серии неверных паролей", blocked.get("code"))

    # sessions.list не отдаёт ни токенов, ни их хэшей.
    sessions_now = await anna.call("sessions.list", {})
    session_items_now = sessions_now.get("payload", {}).get("sessions", [])
    check(len(session_items_now) >= 1
          and all("token" not in item for item in session_items_now),
          "sessions.list без токенов", session_items_now[:1])

    print("\n6. Состояние сервера")
    info = await anna.call("server.info")
    server_payload = info.get("payload", {})
    check(server_payload.get("status") == "ok", "healthcheck сервера", server_payload.get("status"))
    check("postgresql" in server_payload.get("database", ""), "сервер работает с PostgreSQL",
          server_payload.get("database"))
    check(server_payload.get("connections", {}).get("connections", 0) >= 2,
          "ConnectionManager видит обе сессии", server_payload.get("connections"))

    await anna.close()
    await anya.close()
    if google_mock is not None:
        await google_mock.stop()
    if push_mock is not None:
        await push_mock.stop()
    if telegram_mock is not None:
        await telegram_mock.stop()
    if yookassa_mock is not None:
        await yookassa_mock.stop()

    print(f"\nпроверок: {checks}, ошибок: {len(failures)}")
    if failures:
        for failure in failures:
            print("  ✗", failure)
        return 1
    print("E2E: OK")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
