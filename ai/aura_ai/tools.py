"""tools.py — общие функции для работы с инструментами.

Каждый инструмент — обычная функция с валидацией аргументов. Бэкенд выполнения
выбирается настройкой ``AURA_TOOLS_BACKEND``:

* ``sandbox`` — детерминированные ответы (тесты, офлайн, демо);
* ``http``    — реальные API (карты, почта, бронирование) через HTTP-клиент.

C++ ToolManager повторяет этот же контракт со своей стороны, поэтому действие
можно выполнить и на сервере, и здесь — результат одинаковый по структуре.
"""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any, Callable, Dict, List, Optional, Sequence

from .models import AgentContext, Preferences, utcnow
from .planner import TimeWindow, free_slots


class ToolError(RuntimeError):
    pass


@dataclass
class ToolSpec:
    name: str
    description: str
    required: tuple = ()
    handler: Optional[Callable[..., Dict[str, Any]]] = None


class ToolBackends:
    """Внешние вызовы инструментов. Sandbox-режим детерминирован и не ходит в сеть."""

    def __init__(self, mode: str = "sandbox", urls: Optional[Dict[str, str]] = None):
        self.mode = mode if mode in ("sandbox", "http") else "sandbox"
        self.urls = urls or {}

    # --- helpers ---------------------------------------------------------
    def _post(self, key: str, payload: Dict[str, Any], fallback: Dict[str, Any]) -> Dict[str, Any]:
        url = self.urls.get(key)
        if self.mode != "http" or not url:
            return fallback
        import httpx  # ленивый импорт

        try:
            response = httpx.post(url, json=payload, timeout=10)
            response.raise_for_status()
            data = response.json()
            return data if isinstance(data, dict) else {"raw": data}
        except (httpx.HTTPError, ValueError) as exc:
            # HTTPError — сеть/статус, ValueError — не-JSON в ответе.
            return {"ok": False, "error": str(exc), "fallback": fallback}

    @staticmethod
    def _stable_id(*parts: Any) -> str:
        raw = "|".join(str(p) for p in parts)
        return hashlib.sha1(raw.encode("utf-8")).hexdigest()[:10]


# --- кафе и бронирование ---------------------------------------------------

CAFE_DATABASE = (
    {"name": "Кофе на полпути", "city": "Керкраде", "tags": ["coffee", "quiet", "vegan"], "rating": 4.7, "price": 2, "lat": 50.861, "lon": 6.064},
    {"name": "Aurora Roasters", "city": "Керкраде", "tags": ["coffee", "workspace", "gluten_free"], "rating": 4.6, "price": 3, "lat": 50.857, "lon": 6.071},
    {"name": "Grey Garden", "city": "Херлен", "tags": ["brunch", "vegan", "quiet"], "rating": 4.5, "price": 2, "lat": 50.888, "lon": 5.978},
    {"name": "Steak House Nord", "city": "Херлен", "tags": ["dinner", "meat"], "rating": 4.4, "price": 4, "lat": 50.885, "lon": 5.982},
    {"name": "Matcha Point", "city": "Маастрихт", "tags": ["coffee", "vegan", "workspace"], "rating": 4.8, "price": 3, "lat": 50.849, "lon": 5.691},
)


def _score_cafe(cafe: Dict[str, Any], prefs: Preferences, midpoint: Optional[Dict[str, float]] = None) -> float:
    score = cafe["rating"] / 5.0
    if prefs.diet:
        hits = sum(1 for tag in cafe["tags"] if tag in prefs.diet)
        score += 0.2 * hits
        if "meat" in cafe["tags"] and any(d in prefs.diet for d in ("vegan", "vegetarian")):
            score -= 0.5
    if prefs.budget_limit and cafe["price"] > prefs.budget_limit:
        score -= 0.3
    if midpoint:
        distance = math.hypot(cafe["lat"] - midpoint["lat"], cafe["lon"] - midpoint["lon"])
        score += max(0.0, 0.3 - distance)
    return round(score, 3)


def find_cafe(
    prefs: Preferences,
    city: str = "",
    diet: Optional[Sequence[str]] = None,
    at: Optional[datetime] = None,
    midpoint: Optional[Dict[str, float]] = None,
    limit: int = 3,
    backends: Optional[ToolBackends] = None,
    context: Optional[AgentContext] = None,
) -> Dict[str, Any]:
    """Подбирает кафе: диета, бюджет, близость к середине пути между двумя людьми."""
    effective = prefs.model_copy(update={"diet": list(diet) if diet else prefs.diet})
    candidates = [c for c in CAFE_DATABASE if not city or c["city"].lower() == city.lower()]
    if not candidates:
        candidates = list(CAFE_DATABASE)
    ranked = sorted(candidates, key=lambda c: -_score_cafe(c, effective, midpoint))[: max(1, limit)]
    payload = {
        "query": {"city": city or prefs.city, "diet": effective.diet, "at": at.isoformat() if at else None},
        "results": [
            {**cafe, "score": _score_cafe(cafe, effective, midpoint)} for cafe in ranked
        ],
    }
    backend = backends or ToolBackends()
    return backend._post("maps", payload, payload)


def book_table(
    place: str,
    at: datetime,
    people: int = 2,
    backends: Optional[ToolBackends] = None,
    context: Optional[AgentContext] = None,
) -> Dict[str, Any]:
    """Бронь столика. В sandbox-режиме возвращает детерминированный номер брони."""
    if not place:
        raise ToolError("book_table: нужно имя места (place)")
    if people < 1:
        raise ToolError("book_table: people должно быть >= 1")
    confirmation = (backends or ToolBackends())._stable_id(place, at.isoformat(), people)
    payload = {
        "place": place,
        "at": at.isoformat(),
        "people": people,
        "confirmation": f"AURA-{confirmation.upper()}",
    }
    return (backends or ToolBackends())._post("booking", payload, payload)


# --- коммуникации ----------------------------------------------------------

def send_message(
    to: str,
    text: str,
    chat_id: Optional[int] = None,
    backends: Optional[ToolBackends] = None,
    context: Optional[AgentContext] = None,
) -> Dict[str, Any]:
    if not to:
        raise ToolError("send_message: не указан получатель")
    if not text:
        raise ToolError("send_message: пустой текст")
    message_id = (backends or ToolBackends())._stable_id(to, text, utcnow().isoformat())
    return {
        "delivered": True,
        "to": to,
        "chat_id": chat_id,
        "message_id": message_id,
        "text": text,
        "sent_at": utcnow().isoformat(),
    }


def send_email(
    to: str,
    subject: str,
    body: str,
    backends: Optional[ToolBackends] = None,
    context: Optional[AgentContext] = None,
) -> Dict[str, Any]:
    if not to or "@" not in to:
        raise ToolError("send_email: нужен корректный адрес получателя")
    payload = {"to": to, "subject": subject or "Без темы", "body": body, "sent_at": utcnow().isoformat()}
    return (backends or ToolBackends())._post("email", {"**payload": payload, **payload}, payload)


def create_note(text: str, title: str = "", context: Optional[AgentContext] = None) -> Dict[str, Any]:
    if not text.strip():
        raise ToolError("create_note: пустая заметка")
    return {
        "note_id": ToolBackends._stable_id(text),
        "title": title or text.split("\n", 1)[0][:48],
        "text": text,
        "created_at": utcnow().isoformat(),
    }


def create_reminder(
    text: str,
    at: Optional[datetime] = None,
    lead_minutes: int = 15,
    context: Optional[AgentContext] = None,
) -> Dict[str, Any]:
    if not text.strip():
        raise ToolError("create_reminder: пустой текст")
    fire = at or (utcnow() + timedelta(hours=1))
    return {
        "reminder_id": ToolBackends._stable_id(text, fire.isoformat()),
        "text": text,
        "fire_at": fire.isoformat(),
        "notify_at": (fire - timedelta(minutes=max(0, lead_minutes))).isoformat(),
    }


# --- календарь и время -----------------------------------------------------

def check_calendar(
    context: AgentContext,
    window: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Показывает занятость пользователя в окне."""
    start = _parse_dt((window or {}).get("start")) or utcnow()
    end = _parse_dt((window or {}).get("end")) or (start + timedelta(hours=24))
    busy = [
        {"title": e.title, "start": e.start.isoformat(), "end": e.end.isoformat()}
        for e in context.calendar
        if e.end >= start and e.start <= end
    ]
    return {"window": {"start": start.isoformat(), "end": end.isoformat()}, "busy": busy}


def suggest_time(
    context: AgentContext,
    window: Optional[Dict[str, Any]] = None,
    duration_minutes: int = 60,
    limit: int = 3,
) -> Dict[str, Any]:
    """Свободные слоты пользователя внутри окна."""
    start = _parse_dt((window or {}).get("start")) or utcnow()
    end = _parse_dt((window or {}).get("end")) or (start + timedelta(hours=24))
    slots = free_slots(
        TimeWindow(start=start, end=end, label=(window or {}).get("label", "окно")),
        context.calendar,
        context.preferences,
        duration_minutes=duration_minutes,
        max_slots=limit,
    )
    return {
        "duration_minutes": duration_minutes,
        "slots": [s.model_dump(mode="json") for s in slots],
    }


def _parse_dt(value: Any) -> Optional[datetime]:
    if not value:
        return None
    if isinstance(value, datetime):
        # Без часового пояса считать UTC: иначе сравнение с utcnow() падает
        # с «can't compare offset-naive and offset-aware datetimes».
        return value if value.tzinfo else value.replace(tzinfo=timezone.utc)
    try:
        parsed = datetime.fromisoformat(str(value))
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


# --- реестр ----------------------------------------------------------------

REGISTRY: Dict[str, ToolSpec] = {
    name: ToolSpec(name=name, description=description, required=required)
    for name, description, required in (
        ("find_cafe", "Подобрать кафе под диеты, бюджет и середину пути", ("prefs",)),
        ("book_table", "Забронировать столик", ("place", "at")),
        ("send_message", "Отправить сообщение контакту или в чат", ("to", "text")),
        ("send_email", "Отправить письмо", ("to",)),
        ("create_note", "Создать заметку", ("text",)),
        ("create_reminder", "Создать напоминание", ("text",)),
        ("check_calendar", "Показать занятость в окне", ()),
        ("suggest_time", "Предложить свободные слоты", ()),
    )
}

HANDLERS: Dict[str, Callable[..., Dict[str, Any]]] = {
    "find_cafe": find_cafe,
    "book_table": book_table,
    "send_message": send_message,
    "send_email": send_email,
    "create_note": create_note,
    "create_reminder": create_reminder,
    "check_calendar": check_calendar,
    "suggest_time": suggest_time,
}


class ToolManager:
    """Выполняет действия модели: валидация → handler → результат."""

    def __init__(self, backends: Optional[ToolBackends] = None):
        self.backends = backends or ToolBackends()
        self.log: List[Dict[str, Any]] = []

    def list(self) -> List[Dict[str, Any]]:
        return [
            {"name": spec.name, "description": spec.description, "required": list(spec.required)}
            for spec in REGISTRY.values()
        ]

    def run(
        self,
        name: str,
        args: Dict[str, Any],
        context: AgentContext,
        extra: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        if name not in HANDLERS:
            raise ToolError(f"неизвестный инструмент: {name}")
        spec = REGISTRY[name]
        payload: Dict[str, Any] = dict(args or {})
        payload.update(extra or {})
        for key in spec.required:
            if key == "prefs":
                payload.setdefault("prefs", context.preferences)
            elif key not in payload:
                raise ToolError(f"{name}: не хватает аргумента '{key}'")
        handler = HANDLERS[name]
        kwargs: Dict[str, Any] = {}
        import inspect

        params = inspect.signature(handler).parameters
        for key, value in payload.items():
            if key in params:
                kwargs[key] = value
        if "prefs" in params:
            prefs = kwargs.get("prefs")
            kwargs["prefs"] = prefs if isinstance(prefs, Preferences) else context.preferences
        if "context" in params:
            kwargs["context"] = context
        if "backends" in params:
            kwargs["backends"] = self.backends
        if name == "book_table":
            kwargs["at"] = _parse_dt(kwargs.get("at")) or (utcnow() + timedelta(hours=2))
        if name == "create_reminder":
            kwargs["at"] = _parse_dt(kwargs.get("at"))
        if name == "find_cafe":
            kwargs["at"] = _parse_dt(kwargs.get("at"))

        result = handler(**kwargs)
        entry = {"tool": name, "args": payload, "result": result, "at": utcnow().isoformat()}
        self.log.append(entry)
        return result


def serialize(result: Any) -> str:
    return json.dumps(result, ensure_ascii=False, default=str)
