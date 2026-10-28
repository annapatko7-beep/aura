"""Контракты данных между C++-сервером и Python AI Service.

C++-сервер (AgentManager / MemoryManager) общается с сервисом по HTTP и
ожидает ровно эти JSON-структуры.
"""

from __future__ import annotations

import logging
import math
from datetime import datetime, timezone
from typing import Annotated, Any, Dict, List, Optional
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from pydantic import (
    AfterValidator,
    BaseModel,
    BeforeValidator,
    Field,
    ValidationError,
    model_validator,
)

log = logging.getLogger("selfvoy.models")


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def as_aware(value: datetime) -> datetime:
    """Время без пояса считаем UTC: иначе сравнение с utcnow() падает (500)."""
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)


#: datetime, всегда с часовым поясом (наивное время → UTC).
AwareDatetime = Annotated[datetime, AfterValidator(as_aware)]

#: Допустимые виды записей долговременной памяти (CHECK в schema.sql).
MEMORY_KINDS = ("fact", "preference", "schedule", "contact")

# Пределы входных данных: сервер шлёт намного меньше, это защита от 500/DoS.
MAX_DURATION_MINUTES = 24 * 60
MAX_WINDOW_HOURS = 31 * 24
MAX_SLOTS = 50
#: Пределы списков и текста запроса (защита от CPU-DoS: 20 000 событий — десятки
#: секунд поиска слотов). C++-сервер шлёт меньше: календарь он режет до 2000
#: событий, историю чата — 20 последних сообщений, память — 20 записей
#: (AgentManager::ask, MemoryManager::context), сообщение чата — до 8000 символов.
MAX_CALENDAR_EVENTS = 2000
MAX_HISTORY_ITEMS = 50
MAX_MEMORY_ENTRIES = 50
MAX_CONTACTS = 500
MAX_PEERS = 20
MAX_MESSAGE_CHARS = 8000


def normalize_timezone(value: Any) -> str:
    """Имя IANA-пояса; неизвестное/мусорное значение → UTC (а не 500)."""
    name = str(value or "").strip()
    if not name or len(name) > 64:
        return "UTC"
    try:
        ZoneInfo(name)
    except (ZoneInfoNotFoundError, ValueError, OSError):
        return "UTC"
    return name


class CalendarEvent(BaseModel):
    """Занятой слот пользователя (приходит из UserMemory/календаря C++-сервера)."""

    title: str = "event"
    start: AwareDatetime
    end: AwareDatetime

    @model_validator(mode="after")
    def _end_after_start(self) -> "CalendarEvent":
        if self.end < self.start:
            raise ValueError("end раньше start")
        return self


def _drop_invalid_events(value: Any) -> Any:
    """Календарь: запись без start/end пропускается, а не роняет весь запрос (422).

    Одна неполная запись (например, напоминание без времени) иначе ломала бы
    переговоры и диалог целиком. Сервер такие записи не шлёт; это страховка.
    """
    if not isinstance(value, list):
        return value
    # Предел — до разбора каждой записи: иначе огромный список валидируется целиком.
    if len(value) > MAX_CALENDAR_EVENTS:
        raise ValueError(f"календарь длиннее {MAX_CALENDAR_EVENTS} событий")
    kept: List[Any] = []
    for item in value:
        try:
            kept.append(CalendarEvent.model_validate(item))
        except ValidationError:
            log.warning("пропущена запись календаря без корректных start/end: %r", item)
    return kept


#: Список занятых слотов (не больше MAX_CALENDAR_EVENTS) с пропуском неполных записей.
Calendar = Annotated[
    List[CalendarEvent],
    BeforeValidator(_drop_invalid_events),
    Field(max_length=MAX_CALENDAR_EVENTS),
]


def _hours(value: Any) -> Any:
    """Часы суток 0..23; мусорные элементы отбрасываем (а не роняем запрос)."""
    if not isinstance(value, list):
        return value
    kept: List[int] = []
    for item in value:
        if isinstance(item, bool):
            continue
        if isinstance(item, str) and item.strip().isdigit():
            item = int(item.strip())
        if isinstance(item, int) and 0 <= item <= 23:
            kept.append(item)
    return kept


def _coordinate(limit: float):
    def check(value: Any) -> Optional[float]:
        if value is None or isinstance(value, bool):
            return None
        try:
            number = float(value)
        except (TypeError, ValueError):
            return None
        if not math.isfinite(number) or abs(number) > limit:
            return None
        return number

    return check


class Preferences(BaseModel):
    """Срез UserPreferences."""

    diet: List[str] = Field(default_factory=list)
    transport: str = "walk"
    preferred_hours: Annotated[List[int], BeforeValidator(_hours)] = Field(
        default_factory=lambda: [10, 11, 12, 16, 17, 18, 19]
    )
    budget_limit: float = Field(default=0.0, allow_inf_nan=False)
    city: str = ""
    work_hours: Annotated[List[int], BeforeValidator(_hours)] = Field(
        default_factory=lambda: [9, 10, 11, 12, 13, 14, 15, 16, 17, 18]
    )
    # Домашняя точка — нужна, чтобы искать место «на полпути» между двумя агентами Selfvoy.
    # Некорректные координаты превращаются в None (место ищется без середины пути).
    lat: Annotated[Optional[float], BeforeValidator(_coordinate(90.0))] = None
    lon: Annotated[Optional[float], BeforeValidator(_coordinate(180.0))] = None

    # Параметры цифрового двойника (Digital Twin):
    commute_radius_km: float = Field(default=15.0, ge=1.0, le=200.0)
    noise_preference: str = Field(default="any")  # quiet | lively | any
    social_buffer_minutes: int = Field(default=15, ge=0, le=180)
    meeting_type_budget: Dict[str, float] = Field(default_factory=dict)


class MemoryEntry(BaseModel):
    kind: str = "fact"  # fact | preference | schedule | contact
    text: str
    weight: float = Field(default=1.0, allow_inf_nan=False)
    tags: List[str] = Field(default_factory=list)
    updated_at: AwareDatetime = Field(default_factory=utcnow)


class AgentContext(BaseModel):
    """Всё, что C++-сервер знает о пользователе на момент запроса."""

    user_id: int = 0
    display_name: str = ""
    email: str = ""
    now: Optional[AwareDatetime] = None
    timezone: Annotated[str, BeforeValidator(normalize_timezone)] = "Europe/Moscow"
    message: str = Field(default="", max_length=MAX_MESSAGE_CHARS)
    history: List[Dict[str, Any]] = Field(default_factory=list, max_length=MAX_HISTORY_ITEMS)
    memory: List[MemoryEntry] = Field(default_factory=list, max_length=MAX_MEMORY_ENTRIES)
    preferences: Preferences = Field(default_factory=Preferences)
    calendar: Calendar = Field(default_factory=list)
    contacts: List[Dict[str, Any]] = Field(default_factory=list, max_length=MAX_CONTACTS)


class ToolCall(BaseModel):
    id: str = "a1"
    tool: str
    args: Dict[str, Any] = Field(default_factory=dict)


class ToolResult(BaseModel):
    id: str = "a1"
    tool: str
    ok: bool = True
    data: Dict[str, Any] = Field(default_factory=dict)
    error: str = ""


class Step(BaseModel):
    title: str
    detail: str = ""
    tool: Optional[str] = None
    at: Optional[datetime] = None


class Plan(BaseModel):
    goal: str = ""
    intent: str = "chat"
    steps: List[Step] = Field(default_factory=list)
    constraints: Dict[str, Any] = Field(default_factory=dict)


class Slot(BaseModel):
    start: AwareDatetime
    end: AwareDatetime
    score: float = 0.0
    reason: str = ""
    #: Местная дата владельца «YYYY-MM-DD» (только у слотов /v1/agent/slots):
    #: по ней сервер считает лимит записей в день страницы записи.
    day: Optional[str] = None
    #: Местное время владельца «YYYY-MM-DDTHH:MM» (для писем и подписей на сервере).
    local: Optional[str] = None


class A2AProposal(BaseModel):
    """Предложение, которое Selfvoy отправляет Selfvoy другого человека."""

    target: str = ""
    intent: str = "schedule_meeting"
    topic: str = ""
    slots: List[Slot] = Field(default_factory=list)
    place: Dict[str, Any] = Field(default_factory=dict)
    message: str = ""


class AgentRequest(BaseModel):
    context: AgentContext = Field(default_factory=AgentContext)
    # Исполнитель действий — C++-сервер (ToolManager + tool_permissions +
    # подтверждения). AI-сервис по умолчанию только планирует; execute=true —
    # явная просьба вызывающего, и даже тогда внешние действия здесь не исполняются.
    execute: bool = False
    dry_run: bool = False
    peers: List[AgentContext] = Field(default_factory=list, max_length=MAX_PEERS)


class NegotiateRequest(BaseModel):
    """Запрос Agent-to-Agent: один Selfvoy спрашивает другой."""

    initiator: AgentContext
    responder: AgentContext
    intent: str = "schedule_meeting"
    topic: str = Field(default="", max_length=MAX_MESSAGE_CHARS)
    duration_minutes: int = Field(default=60, ge=1, le=MAX_DURATION_MINUTES)
    window_hours: int = Field(default=96, ge=1, le=MAX_WINDOW_HOURS)
    # prefs.privacy.allow_place_choice обеих сторон: false — место не предлагаем.
    choose_place: bool = True
    multi_turn: bool = True
    session_id: str = ""
    round_number: int = 1



#: Пределы для слотов приглашения по ссылке (invite.create) и страницы записи
#: (booking.get на сервере).
OFFER_MIN_DURATION = 15
OFFER_MAX_DURATION = 240
OFFER_MAX_SLOTS = 400
OFFER_MAX_PER_DAY = 16
#: Минимальный зазор до начала встречи: от 0 до 7 суток (по умолчанию час).
OFFER_MAX_LEAD_MINUTES = 7 * 24 * 60
#: Окно страницы записи — до 60 дней вперёд.
OFFER_MAX_WINDOW_HOURS = 60 * 24


class SlotsRequest(BaseModel):
    """Слоты владельца: приглашение по ссылке и страница записи (POST /v1/agent/slots).

    Ищутся только в рабочих часах владельца в его поясе (context.timezone),
    без пересечения с context.calendar, не раньше чем через lead_minutes.

    * ``per_day`` — сколько непересекающихся слотов предлагать в местный день;
    * ``weekends`` — ``None``: выходные только если в будни пусто (приглашение),
      ``True``: будни и выходные вместе, ``False``: только будни (страница записи).
    """

    context: AgentContext
    duration_minutes: int = Field(default=60, ge=OFFER_MIN_DURATION, le=OFFER_MAX_DURATION)
    window_hours: int = Field(default=168, ge=1, le=OFFER_MAX_WINDOW_HOURS)
    max_slots: int = Field(default=6, ge=1, le=OFFER_MAX_SLOTS)
    per_day: int = Field(default=2, ge=1, le=OFFER_MAX_PER_DAY)
    lead_minutes: int = Field(default=60, ge=0, le=OFFER_MAX_LEAD_MINUTES)
    weekends: Optional[bool] = None


class SlotsResponse(BaseModel):
    slots: List[Slot] = Field(default_factory=list)


#: Серия страницы записи — до 12 встреч.
SERIES_MAX_COUNT = 12


class SeriesRequest(BaseModel):
    """Еженедельная серия в поясе владельца (POST /v1/agent/series).

    Повторы сохраняют местное время начала (10:00 по Москве остаётся 10:00
    после перехода на летнее/зимнее время), а не сдвигаются на ровно 168 часов.
    """

    timezone: str = "UTC"
    start: AwareDatetime
    duration_minutes: int = Field(default=60, ge=OFFER_MIN_DURATION, le=OFFER_MAX_DURATION)
    count: int = Field(default=1, ge=1, le=SERIES_MAX_COUNT)
    interval_days: int = Field(default=7, ge=1, le=31)


class SeriesResponse(BaseModel):
    occurrences: List[Slot] = Field(default_factory=list)


class NegotiateResponse(BaseModel):
    accepted: bool = False
    status: str = "AGREED"
    slot: Optional[Slot] = None
    place: Dict[str, Any] = Field(default_factory=dict)
    message: str = ""
    counter_slots: List[Slot] = Field(default_factory=list)
    rounds_log: List[Dict[str, Any]] = Field(default_factory=list)
    session_id: str = ""



class AgentResponse(BaseModel):
    reply: str = ""
    intent: str = "chat"
    confidence: float = 0.5
    plan: Plan = Field(default_factory=Plan)
    actions: List[ToolCall] = Field(default_factory=list)
    results: List[ToolResult] = Field(default_factory=list)
    memory_updates: List[MemoryEntry] = Field(default_factory=list)
    a2a: Optional[A2AProposal] = None
    llm: str = "mock"
