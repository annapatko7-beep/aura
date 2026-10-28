"""planner.py — планирование задач (напоминания, встречи, слоты в расписании).

Модуль полностью детерминированный: никаких внешних вызовов, поэтому его
поведение покрыто юнит-тестами (ai/tests/test_planner.py).
"""

from __future__ import annotations

import re
from bisect import bisect_right
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Dict, Iterable, List, Optional, Sequence, Tuple
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from .models import MAX_DURATION_MINUTES, CalendarEvent, Preferences, Slot, utcnow

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
# «в 10», «в 7 вечера», «в 19 часов» — час без минут.
HOUR_RE = re.compile(r"(?:^|\s)в\s+(\d{1,2})(?:\s*(?:час\w*|ч\b))?(?:\s+(утра|дня|вечера|ночи))?(?=$|[\s,.!?])")
IN_N_RE = re.compile(r"через\s+(\d+)\s*(минут\w*|час\w*|день|дня|дней|недел\w*)")
DURATION_RE = re.compile(r"\bна\s+(\d+)\s*(минут\w*|час\w*|ч\b)")
#: «через N …» дальше 10 лет вперёд — не время, а мусор (иначе OverflowError → 500).
MAX_RELATIVE_DAYS = 3650
#: Больше цифр в числе фразы не разбираем: int() на >4300 цифрах — ValueError.
_MAX_NUMBER_DIGITS = 9


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


def _small_int(digits: str) -> Optional[int]:
    """Число из фразы; слишком длинное (> _MAX_NUMBER_DIGITS цифр) → None («не разобрано»)."""
    if not digits or len(digits) > _MAX_NUMBER_DIGITS:
        return None
    try:
        return int(digits)
    except ValueError:
        return None


def parse_duration(text: str, default: int = 60) -> int:
    low = (text or "").lower()
    match = DURATION_RE.search(low)
    if match:
        value = _small_int(match.group(1))
        unit = match.group(2)
        if value is not None:
            minutes = value * 60 if unit.startswith(("час", "ч")) else value
            # «на 99999999 часов» — не длительность встречи: зажимаем в разумный предел.
            return max(1, min(minutes, MAX_DURATION_MINUTES))
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


def _clock_time(low: str, has_day: bool = True) -> Optional[Tuple[int, int]]:
    """Время суток из фразы: «9:30» / «9.30» или «в 10», «в 7 вечера».

    Голое «в 10» — время, только если рядом назван день («завтра в 10»):
    иначе «в 2 раза» превратилось бы в 2 часа ночи.
    """
    match = TIME_RE.search(low)
    if match:
        return min(int(match.group(1)), 23), min(int(match.group(2)), 59)
    match = HOUR_RE.search(low)
    if not match:
        return None
    hour = int(match.group(1))
    part = match.group(2)
    explicit = part is not None or re.search(r"^\s*в\s+\d{1,2}\s*(?:час|ч\b)", match.group(0)) is not None
    if hour > 23 or not (explicit or has_day):
        return None
    if part in ("дня", "вечера") and hour < 12:
        hour += 12
    elif part == "ночи" and hour == 12:
        hour = 0
    return hour, 0


# Служебные слова напоминания: «Напомни завтра в 10 позвонить юристу» → «Позвонить юристу».
_REMINDER_NOISE = (
    r"^\s*(?:напомни(?:те)?|не\s+забудь(?:те)?)(?:\s+мне)?\b",
    r"\b(?:послезавтра|завтра|сегодня)\b",
    r"\bв\s+\d{1,2}[:.]\d{2}(?:\s+(?:утра|дня|вечера|ночи))?",
    r"\bв\s+\d{1,2}(?:\s*(?:час\w*|ч\b))?(?:\s+(?:утра|дня|вечера|ночи))?(?=$|[\s,.!?])",
    r"\b\d{1,2}[:.]\d{2}\b",
    r"\bчерез\s+\d+\s*(?:минут\w*|час\w*|день|дня|дней|недел\w*)",
    r"\b(?:утром|днём|днем|вечером|ночью)\b",
    r"\b(?:в|во)\s+(?:понедельник|вторник|среду|четверг|пятницу|субботу|воскресенье)\b",
)


def reminder_text(text: str) -> str:
    """Суть напоминания без «напомни» и указаний времени; если ничего не осталось — исходная фраза."""
    value = text or ""
    for pattern in _REMINDER_NOISE:
        value = re.sub(pattern, " ", value, flags=re.IGNORECASE)
    value = re.sub(r"^[\s,.:;\-—]*(?:(?:о\s+том,?\s+)?чтобы?|что)\b", " ", value, flags=re.IGNORECASE)
    value = re.sub(r"\s+", " ", value).strip(" ,.:;-—")
    if not value:
        return (text or "").strip()
    return value[0].upper() + value[1:]


def _relative_offset(digits: str, unit: str) -> Optional[timedelta]:
    """«через N минут/часов/дней/недель» → смещение; дальше MAX_RELATIVE_DAYS — None.

    Огромное N из текста пользователя («через 99999999 недель», число из тысяч
    цифр) не разбирается как время, а не роняет запрос (OverflowError/ValueError).
    """
    value = _small_int(digits)
    if value is None:
        return None
    if unit.startswith("минут"):
        minutes = value
    elif unit.startswith("час"):
        minutes = value * 60
    elif unit.startswith("недел"):
        minutes = value * 7 * 24 * 60
    else:
        minutes = value * 24 * 60
    # Сравниваем в целых минутах: timedelta(weeks=10^9) сам бросает OverflowError.
    if minutes > MAX_RELATIVE_DAYS * 24 * 60:
        return None
    return timedelta(minutes=minutes)


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
    clock = _clock_time(low, has_day=bool(label))
    if clock and label:
        hour, minute = clock
        start = day_start.replace(hour=hour, minute=minute)
        end = start + timedelta(hours=3)
    elif clock and not label:
        hour, minute = clock
        candidate = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
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
    relative = _relative_offset(in_match.group(1), in_match.group(2)) if in_match else None
    if relative is not None:
        try:
            relative_start = now + relative
            relative_end = relative_start + timedelta(hours=4)
        except OverflowError:
            # «Сейчас» у самой границы datetime.max — относительное время не разобрано.
            pass
        else:
            start, end = relative_start, relative_end
            label = label or f"через {int(in_match.group(1))} {in_match.group(2)}"
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
    if isinstance(busy, BusyIndex):
        return busy.overlaps(a_start, a_end)
    return any(a_start < b_end and b_start < a_end for b_start, b_end in busy)


class BusyIndex:
    """Занятость календаря для быстрых проверок пересечения (bisect вместо перебора).

    События один раз отфильтровываются по окну поиска [start, end), сортируются
    и сливаются в непересекающиеся интервалы; тогда концы возрастают, и проверка
    «[a, b) пересекает занятость» — один bisect: O(log n) вместо O(n) на шаг.
    Семантика та же, что у ``_overlaps``: a < конец события и начало < b
    (событие нулевой длины внутри (a, b) тоже занимает время).
    """

    __slots__ = ("starts", "ends")

    def __init__(
        self,
        calendar: Sequence[CalendarEvent],
        start: Optional[datetime] = None,
        end: Optional[datetime] = None,
    ):
        intervals: List[Tuple[datetime, datetime]] = []
        for event in calendar:
            b_start, b_end = _as_aware(event.start), _as_aware(event.end)
            # Событие, целиком лежащее вне окна, ни с одним кандидатом не пересечётся.
            if start is not None and b_end <= start:
                continue
            if end is not None and b_start >= end:
                continue
            intervals.append((b_start, b_end))
        intervals.sort()
        starts: List[datetime] = []
        ends: List[datetime] = []
        for b_start, b_end in intervals:
            if ends and b_start <= ends[-1]:
                if b_end > ends[-1]:
                    ends[-1] = b_end
            else:
                starts.append(b_start)
                ends.append(b_end)
        self.starts = starts
        self.ends = ends

    def __len__(self) -> int:
        return len(self.starts)

    def __iter__(self):
        return iter(zip(self.starts, self.ends))

    def overlaps(self, a_start: datetime, a_end: datetime) -> bool:
        """[a_start, a_end) пересекает хоть одно занятое время (a_start < a_end)."""
        index = bisect_right(self.ends, a_start)
        return index < len(self.starts) and self.starts[index] < a_end


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
    duration = timedelta(minutes=max(15, duration_minutes))
    step = timedelta(minutes=max(15, step_minutes))
    results: List[Slot] = []
    cursor = window.start
    # Округляем вверх до получаса, чтобы слоты выглядели по-человечески.
    if cursor.minute % 30:
        cursor = cursor.replace(minute=30, second=0, microsecond=0)
    # Занятость — один раз на окно кандидатов; score_slot смотрит ещё 30 минут
    # от начала слота, поэтому правая граница с запасом.
    try:
        busy_until: Optional[datetime] = window.end + max(duration, timedelta(minutes=30))
    except OverflowError:  # окно у самой границы datetime.max — без правого фильтра
        busy_until = None
    busy = BusyIndex(calendar, min(cursor, window.start), busy_until)
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
    """Пересечение свободных слотов двух агентов Selfvoy (ядро Agent-to-Agent)."""
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


# ------------------------------------------------- слоты для приглашения по ссылке
#: Минимальный зазор между «сейчас» и началом предлагаемой встречи.
OFFER_LEAD = timedelta(hours=1)
#: Шаг кандидатов (по местному времени владельца: :00 и :30).
OFFER_STEP_MINUTES = 30
#: Рабочие часы по умолчанию, если у владельца список пуст (или весь был мусором).
DEFAULT_WORK_HOURS = tuple(range(9, 19))


def _zone(tz_name: str) -> ZoneInfo:
    """IANA-пояс владельца; неизвестное имя → UTC (а не исключение)."""
    try:
        return ZoneInfo(tz_name or "UTC")
    except (ZoneInfoNotFoundError, ValueError, OSError):
        return ZoneInfo("UTC")


def _ceil_local_half_hour(moment: datetime, zone: ZoneInfo) -> datetime:
    """Округляет момент вверх до :00/:30 по местному времени; результат в UTC."""
    local = moment.astimezone(zone)
    rounded = local.replace(second=0, microsecond=0)
    if rounded < local:
        rounded += timedelta(minutes=1)
    extra = rounded.minute % OFFER_STEP_MINUTES
    if extra:
        rounded += timedelta(minutes=OFFER_STEP_MINUTES - extra)
    return rounded.astimezone(timezone.utc)


def _within_work_hours(start: datetime, end: datetime, zone: ZoneInfo, hours: frozenset) -> bool:
    """Вся встреча [start, end) лежит в рабочих часах владельца (местное время).

    Проверяем каждую четверть часа и последний миг встречи: смещения поясов
    кратны 15 минутам, поэтому начало каждого затронутого часа попадает в сетку.
    """
    probe = start
    quarter = timedelta(minutes=15)
    while probe < end:
        if probe.astimezone(zone).hour not in hours:
            return False
        probe += quarter
    return (end - timedelta(microseconds=1)).astimezone(zone).hour in hours


# per_day больше этого — запрос страницы записи (все варианты дня), иначе приглашение.
BOOKING_PER_DAY_THRESHOLD = 2


def offer_slots(
    window: TimeWindow,
    calendar: Sequence[CalendarEvent],
    prefs: Preferences,
    tz_name: str,
    duration_minutes: int = 60,
    max_slots: int = 6,
    per_day: int = 2,
    now: Optional[datetime] = None,
    lead: timedelta = OFFER_LEAD,
    weekends: Optional[bool] = None,
) -> List[Slot]:
    """Слоты владельца для приглашения по ссылке (гость без Selfvoy выбирает один).

    Правила (см. спецификацию invite.*, раздел «AI-сервис»):
    * кандидаты каждые 30 минут, не раньше now + lead (по умолчанию 1 час;
      округление вверх до :00/:30 по местному времени владельца) и целиком
      внутри окна;
    * вся встреча [start, end) — в рабочих часах владельца (prefs.work_hours,
      местные часы) в ЕГО поясе tz_name (неизвестный пояс → UTC): часы 9..18
      означают начало не раньше 09:00 и конец не позже 19:00;
    * выходные (сб/вс по местному времени): weekends=None — пропускаются, если без
      них есть хоть один слот; True — наравне с буднями; False — никогда;
    * без пересечения с событиями календаря;
    * не больше per_day слотов в местный день; внутри дня сначала предпочтительные
      часы (prefs.preferred_hours, местные), затем раньше; слоты одного дня не
      перекрываются; по дням — равномерно (по кругу: сначала лучший слот каждого
      дня, потом второй);
    * итог хронологический, не больше max_slots; start/end — aware UTC, day —
      местная дата владельца (YYYY-MM-DD).
    """
    zone = _zone(tz_name)
    now = _as_aware(now or utcnow())
    duration = timedelta(minutes=max(15, int(duration_minutes)))
    step = timedelta(minutes=OFFER_STEP_MINUTES)
    max_slots = max(0, int(max_slots))
    per_day = max(1, int(per_day))
    if max_slots == 0:
        return []

    work = frozenset(prefs.work_hours or DEFAULT_WORK_HOURS)
    preferred = frozenset(prefs.preferred_hours or [])

    window_start = _as_aware(window.start)
    window_end = _as_aware(window.end)
    busy = BusyIndex(calendar, window_start, window_end)
    lead = max(timedelta(0), lead)
    cursor = _ceil_local_half_hour(max(window_start, now + lead), zone)

    weekdays: List[Tuple[datetime, datetime]] = []
    weekend_slots: List[Tuple[datetime, datetime]] = []
    guard = 0
    while cursor + duration <= window_end and guard < 5000:
        guard += 1
        end = cursor + duration
        if _within_work_hours(cursor, end, zone, work) and not _overlaps(cursor, end, busy):
            target = weekend_slots if cursor.astimezone(zone).weekday() >= 5 else weekdays
            target.append((cursor, end))
        cursor += step

    if weekends is None:
        candidates = weekdays or weekend_slots
    elif weekends:
        candidates = sorted(weekdays + weekend_slots)
    else:
        candidates = weekdays
    if not candidates:
        return []

    # Группировка по местному дню владельца; внутри дня — предпочтительные часы вперёд.
    by_day: Dict[object, List[Tuple[datetime, datetime]]] = {}
    for start, end in candidates:
        by_day.setdefault(start.astimezone(zone).date(), []).append((start, end))

    picks_by_day: List[List[Tuple[datetime, datetime]]] = []
    for day in sorted(by_day):
        ranked = sorted(
            by_day[day],
            key=lambda item: (item[0].astimezone(zone).hour not in preferred, item[0]),
        )
        chosen: List[Tuple[datetime, datetime]] = []
        for start, end in ranked:
            if len(chosen) >= per_day:
                break
            if _overlaps(start, end, chosen):
                continue
            chosen.append((start, end))
        picks_by_day.append(chosen)

    selected: List[Tuple[datetime, datetime]] = []
    if per_day > BOOKING_PER_DAY_THRESHOLD:
        # Страница записи: ближайшие дни целиком (все их варианты), дальше — пока
        # хватает max_slots. «По кругу» на окне в 60 дней оставило бы по паре
        # вариантов даже на завтра.
        for chosen in picks_by_day:
            for item in chosen:
                if len(selected) >= max_slots:
                    break
                selected.append(item)
    else:
        # Приглашение: первый (лучший) слот каждого дня, затем второй и т.д.
        for rank in range(per_day):
            for chosen in picks_by_day:
                if rank < len(chosen) and len(selected) < max_slots:
                    selected.append(chosen[rank])
    selected.sort()

    result: List[Slot] = []
    for start, end in selected:
        local = start.astimezone(zone)
        is_preferred = local.hour in preferred
        score = 0.6 + (0.3 if is_preferred else 0.0)
        if local.weekday() >= 5:
            score -= 0.05
        hours_ahead = max(0.0, (start - now).total_seconds() / 3600.0)
        score += max(0.0, 0.05 - hours_ahead / 2000.0)
        if local.weekday() >= 5:
            reason = "выходной день" if weekends else "выходной день (в будни свободных окон нет)"
        elif is_preferred:
            reason = "удобное время"
        else:
            reason = "рабочее время"
        result.append(
            Slot(
                start=start.astimezone(timezone.utc),
                end=end.astimezone(timezone.utc),
                score=round(max(0.0, min(1.0, score)), 3),
                reason=reason,
                day=local.date().isoformat(),
                local=local.strftime("%Y-%m-%dT%H:%M"),
            )
        )
    return result


# ------------------------------------------------- серия встреч страницы записи
def weekly_series(
    start: datetime,
    tz_name: str,
    duration_minutes: int,
    count: int,
    interval_days: int = 7,
) -> List[Slot]:
    """Повторы встречи через interval_days в том же МЕСТНОМ времени владельца.

    Первая встреча — ровно start (в том числе второе из двух одинаковых местных
    времён при переходе на зимнее: fold не теряется). Дальше берётся местная дата +
    interval_days и то же местное время; несуществующее время (переход на летнее)
    нормализуется вперёд через UTC, неоднозначное (переход на зимнее) — первое из
    двух (fold=0).
    """
    zone = _zone(tz_name)
    origin = _as_aware(start).astimezone(timezone.utc)
    first = origin.astimezone(zone)
    duration = timedelta(minutes=int(duration_minutes))
    result: List[Slot] = []
    for index in range(max(1, int(count))):
        if index == 0:
            # Ровно start: пересборка из местного времени с fold=0 сдвинула бы
            # второе «01:30» дня перехода на зимнее время на час раньше.
            begin = origin
        else:
            wall = first.replace(tzinfo=None, fold=0) + timedelta(days=interval_days * index)
            local = wall.replace(tzinfo=zone, fold=0)
            begin = local.astimezone(timezone.utc)
        # Несуществующее местное время: astimezone «проскакивает» вперёд — так и оставляем,
        # но местную подпись берём из фактического момента.
        actual = begin.astimezone(zone)
        result.append(
            Slot(
                start=begin,
                end=begin + duration,
                day=actual.date().isoformat(),
                local=actual.strftime("%Y-%m-%dT%H:%M"),
            )
        )
    return result
