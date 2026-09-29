"""planner.py — планирование задач (напоминания, встречи, слоты в расписании).

Модуль полностью детерминированный: никаких внешних вызовов, поэтому его
поведение покрыто юнит-тестами (ai/tests/test_planner.py).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .models import CalendarEvent, Preferences, Slot, utcnow

WEEKDAY_ALIASES: Dict[str, int] = {
    "понедельник": 0, "в понедельник": 0, "пн": 0,
    "вторник": 1, "во вторник": 1, "вт": 1,
    "среда": 2, "в среду": 2, "ср": 2,
    "четверг": 3, "в четверг": 3, "чт": 3,
    "пятница": 4, "в пятницу": 4, "пт": 4,
    "суббота": 5, "в субботу": 5, "сб": 5,
    "воскресенье": 6, "в воскресенье": 6, "вск": 6, "вс": 6,
}

WEEKDAY_NAMES = ("понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье")

# Сильные сигналы в начале фразы: «напомни купить кофе» — это напоминание,
# а не встреча, даже если слово «кофе» есть в словаре встреч.
INTENT_PREFIXES: Tuple[Tuple[str, str], ...] = (
    ("reminder", r"^\s*(напомни|напомните|не забудь|не забудьте)"),
    ("note", r"^\s*(запиши|запишите|сделай заметку|законспектируй)"),
    ("find_place", r"^\s*(найди|найдите|подбери|подберите)"),
    ("email", r"^\s*(отправь письмо|напиши письмо|отправьте письмо)"),
    ("message", r"^\s*(напиши|отправь|скинь|ответь)"),
    ("memory", r"^\s*(запомни|запомните)"),
)

INTENT_KEYWORDS: Dict[str, Tuple[str, ...]] = {
    "schedule_meeting": (
        "встрет", "созвон", "встреча", "кофе", "обсудить", "обсудим", "meeting",
        "пересечься", "увидеться", "свидание", "переговоры", "созвониться",
    ),
    "reminder": ("напомни", "напомнить", "напоминание", "не забыть", "reminder", "не забудь"),
    "note": ("заметк", "запиши", "записать", "конспект", "note"),
    "email": ("почт", "письмо", "email", "e-mail", "напиши письмо", "отправь письмо"),
    "message": ("напиши", "отправь", "сообщение", "скинь", "message", "ответь"),
    "find_place": ("найди", "подбери", "где", "кафе", "кофейн", "ресторан", "место", "place"),
    "budget": ("бюджет", "потрат", "деньги", "стоимость", "budget", "оплати"),
    "memory": ("запомни", "помни", "запомнить", "я люблю", "я не люблю", "у меня аллергия"),
}

DAY_PARTS: Dict[str, Tuple[int, int]] = {
    "утром": (8, 12), "утро": (8, 12),
    "днём": (12, 17), "днем": (12, 17), "после обеда": (13, 18),
    "вечером": (17, 23), "вечер": (17, 23),
    "ночью": (0, 6),
}

TIME_RE = re.compile(r"\b(\d{1,2})[:.](\d{2})\b")
IN_N_RE = re.compile(r"через\s+(\d+)\s*(минут\w*|час\w*|день|дня|дней|недел\w*)")
DURATION_RE = re.compile(r"\bна\s+(\d+)\s*(минут\w*|час\w*|ч\b)")


@dataclass
class TimeWindow:
    """Окно времени, которое упомянул пользователь."""

    start: datetime
    end: datetime
    label: str = ""
    source: str = "inferred"

    def as_dict(self) -> Dict[str, str]:
        return {
            "start": self.start.isoformat(),
            "end": self.end.isoformat(),
            "label": self.label,
            "source": self.source,
        }


@dataclass
class Understanding:
    """Результат разбора свободной фразы."""

    text: str
    intent: str
    window: Optional[TimeWindow] = None
    duration_minutes: int = 60
    person: str = ""
    topic: str = ""
    keywords: List[str] = field(default_factory=list)

    def as_dict(self) -> Dict[str, object]:
        return {
            "text": self.text,
            "intent": self.intent,
            "window": self.window.as_dict() if self.window else None,
            "duration_minutes": self.duration_minutes,
            "person": self.person,
            "topic": self.topic,
            "keywords": list(self.keywords),
        }


def _as_aware(dt: datetime) -> datetime:
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


def detect_intent(text: str) -> str:
    """Определяет намерение по ключевым словам. Возвращает один из INTENT_KEYWORDS."""
    low = (text or "").lower()
    if not low.strip():
        return "chat"
    for intent, pattern in INTENT_PREFIXES:
        if re.match(pattern, low):
            return intent
    scores: List[Tuple[int, str]] = []
    for intent, words in INTENT_KEYWORDS.items():
        hits = sum(1 for w in words if w in low)
        if hits:
            scores.append((hits, intent))
    if not scores:
        return "chat"
    # Стабильная сортировка: больше совпадений → выше; при равенстве — порядок словаря.
    scores.sort(key=lambda item: (-item[0], list(INTENT_KEYWORDS).index(item[1])))
    return scores[0][1]


def parse_duration(text: str, default: int = 60) -> int:
    low = (text or "").lower()
    match = DURATION_RE.search(low)
    if match:
        value = int(match.group(1))
        unit = match.group(2)
        return value * 60 if unit.startswith(("час", "ч")) else value
    if "полчаса" in low:
        return 30
    if "на час" in low:
        return 60
    return default


def extract_person(text: str) -> str:
    """'встретиться с Аней' → 'Аней' (грубо, но достаточно для поиска контакта)."""
    match = re.search(r"\bс\s+([А-ЯЁA-Z][а-яёa-zA-Z]{1,30})\b", text or "")
    if match:
        return match.group(1)
    match = re.search(r"\bс другом\b|\bс подругой\b", (text or "").lower())
    return match.group(0).replace("с ", "") if match else ""


def extract_topic(text: str) -> str:
    match = re.search(r"(?:обсудить|про|о|по поводу)\s+([а-яёa-zA-Z0-9 ,\-]{3,60})", (text or "").lower())
    return match.group(1).strip(" .,") if match else ""


def next_weekday(now: datetime, weekday: int) -> datetime:
    """Ближайшая дата с указанным номером дня недели (0 = понедельник)."""
    delta = (weekday - now.weekday()) % 7
    day = now + timedelta(days=delta)
    return day.replace(hour=0, minute=0, second=0, microsecond=0)


def parse_time_window(text: str, now: Optional[datetime] = None) -> Optional[TimeWindow]:
    """Разбирает русские временные выражения в окно [start, end)."""
    now = _as_aware(now or utcnow())
    low = (text or "").lower()
    if not low.strip():
        return None

    base_day = now.replace(hour=0, minute=0, second=0, microsecond=0)
    day_start = base_day
    label = ""
    source = "phrase"

    if "послезавтра" in low:
        day_start = base_day + timedelta(days=2)
        label = "послезавтра"
    elif "завтра" in low:
        day_start = base_day + timedelta(days=1)
        label = "завтра"
    elif "выходн" in low:
        # «в эти выходные»: ближайшая суббота (если сегодня сб/вс — сегодняшний день)
        offset = (5 - now.weekday()) % 7
        day_start = base_day + timedelta(days=offset)
        label = "выходные"
    elif "на следующей неделе" in low or "следующая неделя" in low:
        day_start = base_day + timedelta(days=7 - now.weekday())
        label = "следующая неделя"
    elif "на этой неделе" in low or "эта неделя" in low:
        label = "эта неделя"
    elif "сегодня" in low:
        label = "сегодня"
    else:
        for alias, weekday in WEEKDAY_ALIASES.items():
            if re.search(rf"(^|[^а-яё]){re.escape(alias)}([^а-яё]|$)", low):
                day_start = next_weekday(now, weekday)
                label = WEEKDAY_NAMES[weekday]
                break

    day_end = day_start + timedelta(days=2 if label == "выходные" else (7 if "неделе" in label or "неделя" in label else 1))

    # Ограничение по времени суток / конкретному часу
    start, end = day_start, day_end
    time_match = TIME_RE.search(low)
    if time_match and label:
        hour, minute = int(time_match.group(1)), int(time_match.group(2))
        start = day_start.replace(hour=min(hour, 23), minute=min(minute, 59))
        end = start + timedelta(hours=3)
    elif time_match and not label:
        hour, minute = int(time_match.group(1)), int(time_match.group(2))
        candidate = now.replace(hour=min(hour, 23), minute=min(minute, 59), second=0, microsecond=0)
        if candidate <= now:
            candidate += timedelta(days=1)
        start, end = candidate, candidate + timedelta(hours=3)
        label = label or "ближайшее время"
    else:
        for part, (from_h, to_h) in DAY_PARTS.items():
            if part in low:
                start = day_start.replace(hour=from_h)
                end = day_start.replace(hour=to_h)
                label = label or part
                break

    # «через N часов/дней» — относительное окно
    in_match = IN_N_RE.search(low)
    if in_match:
        value = int(in_match.group(1))
        unit = in_match.group(2)
        if unit.startswith("минут"):
            start = now + timedelta(minutes=value)
        elif unit.startswith("час"):
            start = now + timedelta(hours=value)
        elif unit.startswith("недел"):
            start = now + timedelta(weeks=value)
        else:
            start = now + timedelta(days=value)
        end = start + timedelta(hours=4)
        label = label or f"через {value} {unit}"
        source = "relative"

    if end <= start:
        end = start + timedelta(hours=3)
    if start < now:
        start = now
        if end <= start:
            end = start + timedelta(hours=3)

    return TimeWindow(start=start, end=end, label=label or "ближайшее время", source=source)


def understand(text: str, now: Optional[datetime] = None) -> Understanding:
    """Полный разбор фразы: намерение + окно + длительность + персона + тема."""
    return Understanding(
        text=text,
        intent=detect_intent(text),
        window=parse_time_window(text, now),
        duration_minutes=parse_duration(text),
        person=extract_person(text),
        topic=extract_topic(text),
        keywords=[w for w in re.findall(r"[а-яёa-z]{4,}", (text or "").lower())],
    )


def _busy_intervals(calendar: Sequence[CalendarEvent]) -> List[Tuple[datetime, datetime]]:
    return sorted((_as_aware(e.start), _as_aware(e.end)) for e in calendar)


def _overlaps(a_start: datetime, a_end: datetime, busy: Iterable[Tuple[datetime, datetime]]) -> bool:
    return any(a_start < b_end and b_start < a_end for b_start, b_end in busy)


def score_slot(
    start: datetime,
    prefs: Preferences,
    busy: Sequence[Tuple[datetime, datetime]],
    work_hours: bool = True,
) -> float:
    """Оценка слота: предпочтения по часам > рабочие часы > близость к сейчас."""
    score = 0.5
    if start.hour in (prefs.preferred_hours or []):
        score += 0.25
    if work_hours and start.hour in (prefs.work_hours or []):
        score += 0.1
    if start.weekday() >= 5:
        score -= 0.05
    if _overlaps(start, start + timedelta(minutes=30), busy):
        score -= 0.4
    score += max(0.0, 0.1 - _hours_from_now(start) / 240.0)
    return round(max(0.0, min(1.0, score)), 3)


def _hours_from_now(start: datetime) -> float:
    return max(0.0, (start - utcnow()).total_seconds() / 3600.0)


def free_slots(
    window: TimeWindow,
    calendar: Sequence[CalendarEvent],
    prefs: Preferences,
    duration_minutes: int = 60,
    step_minutes: int = 30,
    max_slots: int = 5,
) -> List[Slot]:
    """Ищет свободные слоты внутри окна, не пересекающиеся с календарём."""
    busy = _busy_intervals(calendar)
    duration = timedelta(minutes=max(15, duration_minutes))
    step = timedelta(minutes=max(15, step_minutes))
    results: List[Slot] = []
    cursor = window.start
    # Округляем вверх до получаса, чтобы слоты выглядели по-человечески.
    if cursor.minute % 30:
        cursor = cursor.replace(minute=30, second=0, microsecond=0)
    guard = 0
    while cursor + duration <= window.end and guard < 500:
        guard += 1
        if not _overlaps(cursor, cursor + duration, busy):
            score = score_slot(cursor, prefs, busy)
            reason = "свободно"
            if cursor.hour in (prefs.preferred_hours or []):
                reason = "удобное время"
            results.append(
                Slot(start=cursor, end=cursor + duration, score=score, reason=reason)
            )
        cursor += step
    results.sort(key=lambda s: (-s.score, s.start))
    return results[:max_slots]


def intersect_slots(
    a: Sequence[Slot],
    b: Sequence[Slot],
    duration_minutes: int = 60,
) -> List[Slot]:
    """Пересечение свободных слотов двух Аур (ядро Agent-to-Agent)."""
    duration = timedelta(minutes=max(15, duration_minutes))
    merged: List[Slot] = []
    for left in a:
        for right in b:
            start = max(left.start, right.start)
            end = min(left.end, right.end)
            if end - start >= duration:
                merged.append(
                    Slot(
                        start=start,
                        end=start + duration,
                        score=round((left.score + right.score) / 2.0, 3),
                        reason="общее окно в графиках",
                    )
                )
    merged.sort(key=lambda s: (-s.score, s.start))
    return merged
