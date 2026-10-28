#!/usr/bin/env python3
"""Проверка настоящей LLM в AI-сервисе Selfvoy: отвечает ли модель и как.

Отправляет в /v1/agent/run несколько типичных запросов и печатает для
каждого: кто ответил (поле ``llm``: имя модели или «…-fallback», если
модель не ответила и сработали локальные правила), время ответа, намерение,
действия и начало ответа. Код выхода 0 — все ответы от настоящей модели.

Только стандартная библиотека: запускается где угодно, в т.ч. внутри
контейнера AI-сервиса (``deploy/selfvoy.sh llm-check``).

    SELFVOY_AI_TOKEN=... python3 tools/llm_check.py [http://127.0.0.1:8000]
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

URL = (sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SELFVOY_AI_URL", "http://127.0.0.1:8000")).rstrip("/")
TOKEN = os.environ.get("SELFVOY_AI_TOKEN", "")


def call(method: str, path: str, body: dict | None = None, timeout: float = 60.0) -> tuple[int, dict, float]:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    request = urllib.request.Request(URL + path, data=data, method=method)
    request.add_header("Content-Type", "application/json")
    if TOKEN:
        request.add_header("X-Selfvoy-Token", TOKEN)
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8") or "{}")
            return response.status, payload, time.monotonic() - started
    except urllib.error.HTTPError as error:
        text = error.read().decode("utf-8", "replace")
        try:
            payload = json.loads(text)
        except ValueError:
            payload = {"detail": text[:200]}
        return error.code, payload, time.monotonic() - started
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        return 0, {"detail": str(error)}, time.monotonic() - started


def context(message: str, calendar: list | None = None, memory: list | None = None) -> dict:
    now = datetime.now(timezone.utc).replace(microsecond=0)
    return {
        "user_id": 1,
        "display_name": "Анна",
        "timezone": "Europe/Moscow",
        "now": now.isoformat(),
        "message": message,
        "calendar": calendar or [],
        "memory": memory or [],
    }


def main() -> int:
    status, health, _ = call("GET", "/healthz", timeout=10)
    if status != 200:
        print(f"AI-сервис недоступен по {URL}: {status} {health.get('detail', '')}")
        return 2
    model = health.get("llm", "?")
    print(f"AI-сервис: {URL}, модель: {model}")
    if model == "mock":
        print("Настоящая LLM не подключена: задайте SELFVOY_LLM_PROVIDER=openai и OPENAI_API_KEY "
              "(для другого провайдера — ещё OPENAI_BASE_URL) и перезапустите AI-сервис.")
        return 3

    now = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
    busy = [{"title": "Созвон с командой", "start": (now + timedelta(days=1, hours=2)).isoformat(),
             "end": (now + timedelta(days=1, hours=3)).isoformat()}]
    cases = [
        ("напоминание", context("Напомни завтра в 9 утра позвонить маме")),
        ("расписание", context("Что у меня завтра? Есть свободное время после обеда?", calendar=busy)),
        ("встреча", context("Найди 30 минут для встречи с Петром на этой неделе", calendar=busy)),
        ("память", context("Запомни: я не ем мясо и люблю итальянскую кухню")),
        ("разговор", context("Как мне лучше спланировать неделю, если много учеников?",
                             memory=[{"kind": "fact", "text": "Анна — репетитор по английскому"}])),
    ]

    failures = 0
    for title, ctx in cases:
        status, answer, seconds = call("POST", "/v1/agent/run", {"context": ctx, "execute": False, "dry_run": True})
        used = answer.get("llm", "?")
        real = status == 200 and not str(used).endswith("-fallback") and used != "mock"
        failures += 0 if real else 1
        mark = "OK " if real else "НЕТ"
        print(f"\n[{mark}] {title}: HTTP {status}, {seconds:.1f} с, ответила: {used}")
        if status != 200:
            print(f"      ошибка: {answer.get('detail', answer)}")
            continue
        tools = ", ".join(action.get("tool", "?") for action in answer.get("actions", [])) or "—"
        memory = "; ".join(item.get("text", "") for item in answer.get("memory_updates", [])) or "—"
        reply = " ".join(str(answer.get("reply", "")).split())
        print(f"      намерение: {answer.get('intent')} (уверенность {answer.get('confidence')})")
        print(f"      действия: {tools}")
        print(f"      в память: {memory}")
        print(f"      ответ: {reply[:300]}{'…' if len(reply) > 300 else ''}")

    print()
    if failures:
        print(f"Настоящая модель не ответила в {failures} из {len(cases)} запросов. Частые причины: неверный "
              "ключ, нет доступа к API провайдера из сети сервера, исчерпан баланс, неверный OPENAI_BASE_URL "
              "или имя модели (SELFVOY_LLM_MODEL). Подробности — в логе AI-сервиса.")
        return 1
    print(f"Все {len(cases)} запросов обработала настоящая модель ({model}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
