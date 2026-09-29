#!/usr/bin/env python3
"""Сверка iOS-клиента с серверным протоколом.

Достаёт из Swift-исходников (ios/) все строковые литералы, похожие на типы
WS-сообщений ("домен.действие"), и проверяет, что каждый из них зарегистрирован
в server/src/server.cpp (registerHandler) или является известным событием.
Обратная связь без macOS/Xcode: ловит опечатки в именах хендлеров.

Запуск:  python3 tools/check_ios_protocol.py
Код возврата: 0 — неизвестных типов нет, 1 — есть.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Типы-сообщения выглядят как "слово.слово" (auth.login, tasks.create, ...).
# Ровно одна точка: отличает их от SF Symbols ("arrow.up.circle.fill") и
# обратных идентификаторов ("ai.aura.app").
TYPE_RE = re.compile(r'"([a-z][a-z0-9]*\.[a-z0-9]+)"')

# Легитимные строки с одной точкой, не являющиеся типами запросов.
# SF Symbols (иконки) в Swift-коде клиента.
ALLOWED: set[str] = {
    "exclamationmark.shield",
    "lock.shield",
    "person.2",
    "arrow.right.square",
}

# Серверные события: их клиент не отправляет, только принимает.
EVENTS = {"chat.message", "task.due", "notification.new"}

# Виды уведомлений (этап 13): значения NotificationRecord.kind, часть
# протокола, но не типы запросов. Проверяются по create() в server.cpp.
KINDS = {
    "task.due",
    "confirmation.requested",
    "a2a.proposal",
    "login.new",
    "twofactor.enabled",
    "twofactor.disabled",
}


def server_types() -> set[str]:
    text = (ROOT / "server/src/server.cpp").read_text(encoding="utf-8")
    return set(re.findall(r'registerHandler\("([^"]+)"', text))


def server_kinds() -> set[str]:
    """Виды уведомлений, которые сервер реально создаёт (create(userId, "kind", ...))."""
    text = (ROOT / "server/src/server.cpp").read_text(encoding="utf-8")
    return set(re.findall(r'create\(\s*[^,]+,\s*"([a-z][a-z0-9.]+)"', text))


SYSTEM_IMAGE_RE = re.compile(r'systemImage(?:Name)?:\s*"[^"]*"')


def swift_candidates() -> dict[str, list[str]]:
    """Кандидаты-типы из Swift-исходников: тип → файлы, где встретился."""
    found: dict[str, list[str]] = {}
    for path in sorted((ROOT / "ios").rglob("*.swift")):
        relative = str(path.relative_to(ROOT))
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("//"):
                continue
            # Иконки SF Symbols ("lock.shield" и т.п.) — не типы сообщений.
            line = SYSTEM_IMAGE_RE.sub("", line)
            for match in TYPE_RE.findall(line):
                found.setdefault(match, []).append(relative)
    return found


def main() -> int:
    registered = server_types()
    if not registered:
        print("не удалось прочитать registerHandler из server/src/server.cpp")
        return 1

    kinds = server_kinds()
    unknown: list[str] = []
    for candidate, files in sorted(swift_candidates().items()):
        if candidate in ALLOWED or candidate in EVENTS:
            continue
        if candidate in KINDS:
            # Вид уведомления должен реально создаваться сервером.
            if candidate not in kinds:
                unknown.append(f"  {candidate}  (kind: сервер его не создаёт)")
            continue
        if candidate in registered:
            continue
        # Составные типы вида "tasks." + status формируются динамически —
        # их префиксы проверяем отдельно.
        if any(candidate.startswith(known + ".") for known in registered):
            continue
        unknown.append(f"  {candidate}  ({', '.join(sorted(set(files)))})")

    print(f"хендлеров сервера: {len(registered)}")
    print(f"типов в iOS-клиенте: {len(swift_candidates())}")
    if unknown:
        print("неизвестные типы сообщений:")
        print("\n".join(unknown))
        return 1

    # Динамический префикс tasks.*: все три продолжения должны существовать.
    for suffix in ("complete", "cancel", "reopen"):
        if f"tasks.{suffix}" not in registered:
            print(f"на сервере нет tasks.{suffix}")
            return 1

    print("OK: все типы сообщений iOS-клиента известны серверу")
    return 0


if __name__ == "__main__":
    sys.exit(main())
