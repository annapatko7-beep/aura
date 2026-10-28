#!/usr/bin/env python3
"""Демо-данные для проверки клиентов на dev-стеке (tools/dev_stack.sh).

Создаёт три аккаунта — Алекс Морган (alex@selfvoy.local), Дима Волков и Мария Чен —
с настройками, задачами Алекса, чатом с Димой, запросом к Selfvoy и одним
раундом переговоров. Нужен сервер с SELFVOY_MAIL_DRIVER=dev (код подтверждения
приходит в ответе auth.register).

Пароль берётся из окружения, в коде его нет:
    SELFVOY_DEMO_PASSWORD=...  python3 tools/demo_seed.py [ws://127.0.0.1:9000]
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import uuid
from datetime import datetime, timedelta, timezone

import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SELFVOY_SERVER", "ws://127.0.0.1:9000")
PASSWORD = os.environ.get("SELFVOY_DEMO_PASSWORD", "")


class Client:
    def __init__(self) -> None:
        self.ws = None
        self.replies: dict[str, asyncio.Queue] = {}

    async def connect(self) -> None:
        self.ws = await websockets.connect(URL, max_size=8 << 20)
        asyncio.create_task(self.pump())

    async def pump(self) -> None:
        async for raw in self.ws:
            message = json.loads(raw)
            if message.get("type") != "event" and message.get("id") in self.replies:
                await self.replies[message["id"]].put(message)

    async def call(self, kind: str, payload: dict | None = None) -> dict:
        request_id = uuid.uuid4().hex[:8]
        queue: asyncio.Queue = asyncio.Queue()
        self.replies[request_id] = queue
        await self.ws.send(json.dumps({"id": request_id, "type": kind, "payload": payload or {}},
                                      ensure_ascii=False))
        reply = await asyncio.wait_for(queue.get(), 60)
        if reply.get("type") == "error":
            print(f"  ! {kind}: {reply.get('code')} {reply.get('message')}")
        return reply


async def account(name: str, email: str, prefs: dict) -> Client:
    client = Client()
    await client.connect()
    reply = await client.call("auth.register", {"display_name": name, "email": email, "password": PASSWORD})
    code = reply.get("payload", {}).get("email_code")
    if code:
        await client.call("auth.verifyEmail", {"email": email, "code": code})
    await client.call("auth.login", {"email": email, "password": PASSWORD, "device": "seed",
                                     "device_id": "seed-" + email})
    await client.call("prefs.set", prefs)
    return client


def iso(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


async def main() -> None:
    if len(PASSWORD) < 8:
        sys.exit("Задайте SELFVOY_DEMO_PASSWORD: 8+ символов, буквы и цифры")
    now = datetime.now().astimezone()

    alex = await account("Алекс Морган", "alex@selfvoy.local", {
        "city": "Москва", "diet": [], "transport": "metro", "budget_limit": 1000,
        "work_hours": list(range(10, 19)), "preferred_hours": list(range(13, 21)), "onboarded": True,
    })
    dima = await account("Дима Волков", "dima@selfvoy.local", {
        "city": "Москва", "diet": ["gluten_free"], "transport": "car", "onboarded": True,
        "work_hours": list(range(9, 18)),
    })
    await account("Мария Чен", "maria@selfvoy.local", {"city": "Москва", "diet": ["vegetarian"], "onboarded": True})

    def at(days: int, hour: int, minute: int = 0) -> str:
        moment = (now + timedelta(days=days)).replace(hour=hour, minute=minute, second=0, microsecond=0)
        return iso(moment)

    later = min(now.hour + 3, 23)
    tasks = [
        ("Подготовить презентацию к ревью Q3", at(0, later), at(0, later - 1, 45)),
        ("Ответить на письмо инвестора", at(0, 23, 30), None),
        ("Разослать приглашения на демо-день", at(2, 12), None),
        ("Посмотреть метрики маркетинговой кампании", at(3, 15), None),
    ]
    for title, due, remind in tasks:
        payload = {"title": title, "due_at": due}
        if remind:
            payload["remind_at"] = remind
        await alex.call("tasks.create", payload)
    done = await alex.call("tasks.create", {"title": "Ревью дизайна питч-дека", "due_at": at(-1, 16)})
    done_payload = done.get("payload", {})
    await alex.call("tasks.complete", {"id": done_payload.get("id") or done_payload.get("task", {}).get("id")})

    chat = await alex.call("chat.open", {"contact": "dima@selfvoy.local"})
    chat_id = chat["payload"]["chat"]["id"]
    await alex.call("chat.send", {"chat_id": chat_id, "body": "Дима, привет! Давай на выходных обсудим стартап?"})
    await dima.call("chat.send", {"chat_id": chat_id, "body": "Давай! Пусть наши Selfvoy договорятся."})
    await alex.call("chat.open", {"contact": "maria@selfvoy.local"})

    await alex.call("agent.ask", {"message": "Хочу встретиться с Димой в эти выходные и обсудить стартап"})
    await alex.call("agent.negotiate", {"target": "dima@selfvoy.local", "topic": "обсудить стартап",
                                        "duration_minutes": 90, "window_hours": 96})
    print("демо-данные готовы: alex@selfvoy.local, dima@selfvoy.local, maria@selfvoy.local")


if __name__ == "__main__":
    asyncio.run(main())
