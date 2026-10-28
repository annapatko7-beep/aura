#!/usr/bin/env python3
"""Сверка Android-клиента с серверным протоколом (этап 11).

Достаёт из Kotlin-исходников (android/) все строковые литералы, похожие на типы
WS-сообщений ("домен.действие"), и проверяет, что каждый из них зарегистрирован
в server/src/server.cpp (registerHandler) или является известным событием/видом
уведомлений. Аналог tools/check_ios_protocol.py — обратная связь без Android SDK:
ловит опечатки в именах хендлеров и разъехавшиеся строковые константы.

Запуск:  python3 tools/check_android_protocol.py
Код возврата: 0 — неизвестных типов нет, 1 — есть.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Типы-сообщения выглядят как "слово.слово" (auth.login, tasks.create, ...).
# Ровно одна точка: отличает их от обратных идентификаторов ("app.selfvoy.kit").
TYPE_RE = re.compile(r'"([a-z][a-z0-9]*\.[a-z0-9]+)"')

# Легитимные строки с одной точкой, не являющиеся типами запросов.
# (Идентификаторы с двумя точками — "app.selfvoy.kit", "com.google.firebase..." —
# регулярное выражение не матчит: в них больше одной точки.)
ALLOWED: set[str] = {
    # applicationId / пакет приложения — одна точка, но не тип запроса
    "app.selfvoy",
}

# Серверные события: их клиент не отправляет, только принимает.
EVENTS = {"chat.message", "task.due", "notification.new", "session.revoked"}

# Виды уведомлений (этап 13): значения NotificationRecord.kind — часть
# протокола, но не типы запросов. Проверяются по create() в server.cpp.
KINDS = {
    "task.due",
    "confirmation.requested",
    "a2a.proposal",
    "login.new",
    "twofactor.enabled",
    "twofactor.disabled",
    "referral.joined",
    "referral.reward",
    "booking.confirmed",
    "booking.payment",
    # F1–F6: события записей и пакетов владельца (SelfvoyNoticeKind, Android)
    "booking.created",
    "booking.rescheduled",
    "booking.cancelled",
    "package.purchased",
    "package.payment",
    "package.low",
    "waitlist.joined",
    "waitlist.booked",
    "tax.receipt",
    "tax.refund",
    # F7: вопрос гостя ИИ-администратору, переданный специалисту
    "telegram.question",
}


def server_types() -> set[str]:
    text = (ROOT / "server/src/server.cpp").read_text(encoding="utf-8")
    return set(re.findall(r'registerHandler\("([^"]+)"', text))


def server_kinds() -> set[str]:
    """Виды уведомлений, которые сервер реально создаёт (create(userId, "kind", ...))."""
    kinds: set[str] = set()
    for path in sorted((ROOT / "server/src").glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        kinds |= set(re.findall(r'create\(\s*[^,]+,\s*"([a-z][a-z0-9.]+)"', text))
    return kinds


def kotlin_candidates() -> dict[str, list[str]]:
    """Кандидаты-типы из Kotlin-исходников: тип → файлы, где встретился."""
    found: dict[str, list[str]] = {}
    for path in sorted((ROOT / "android").rglob("*.kt")):
        relative = str(path.relative_to(ROOT))
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("//"):
                continue
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
    for candidate, files in sorted(kotlin_candidates().items()):
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
    print(f"типов в Android-клиенте: {len(kotlin_candidates())}")
    if unknown:
        print("неизвестные типы сообщений:")
        print("\n".join(unknown))
        return 1

    # Динамический префикс tasks.*: все три продолжения должны существовать.
    for suffix in ("complete", "cancel", "reopen"):
        if f"tasks.{suffix}" not in registered:
            print(f"на сервере нет tasks.{suffix}")
            return 1

    # Платформа push-устройства Android должна приниматься сервером.
    notifications = (ROOT / "server/src/notificationsmanager.cpp").read_text(encoding="utf-8")
    if '"fcm"' not in notifications:
        print("сервер не принимает платформу fcm (notificationsmanager.cpp)")
        return 1

    print("OK: все типы сообщений Android-клиента известны серверу")
    return 0


if __name__ == "__main__":
    sys.exit(main())
