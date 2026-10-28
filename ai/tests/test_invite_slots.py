"""Слоты владельца для приглашения по ссылке: planner.offer_slots и POST /v1/agent/slots."""

from datetime import datetime, timedelta, timezone

import pytest
from fastapi.testclient import TestClient

from selfvoy_ai import app as app_module
from selfvoy_ai import memory as memory_module
from selfvoy_ai import planner
from selfvoy_ai.agent import SelfvoyAgent
from selfvoy_ai.config import Settings, reset_settings
from selfvoy_ai.llm import MockLLM
from selfvoy_ai.models import (
    OFFER_MAX_PER_DAY,
    OFFER_MAX_SLOTS,
    OFFER_MAX_WINDOW_HOURS,
    CalendarEvent,
    Preferences,
)
from selfvoy_ai.tools import ToolManager

MONDAY = datetime(2026, 9, 14, 0, 0, tzinfo=timezone.utc)  # понедельник, 03:00 по Москве
WORK = list(range(9, 19))  # 9..18 → встреча с 09:00 до 19:00


def _prefs(work=None, preferred=None) -> Preferences:
    return Preferences(work_hours=WORK if work is None else work, preferred_hours=preferred or [])


def _window(start: datetime, hours: int) -> planner.TimeWindow:
    return planner.TimeWindow(start=start, end=start + timedelta(hours=hours))


def _offer(now=MONDAY, hours=24, calendar=(), prefs=None, tz="UTC", duration=60, max_slots=50, per_day=50,
           start=None):
    return planner.offer_slots(
        _window(start or now, hours),
        list(calendar),
        prefs or _prefs(),
        tz,
        duration_minutes=duration,
        max_slots=max_slots,
        per_day=per_day,
        now=now,
    )


def _event(start: datetime, end: datetime) -> CalendarEvent:
    return CalendarEvent(title="занято", start=start, end=end)


# ------------------------------------------------------------ часовой пояс
def test_moscow_work_hours_map_to_utc():
    slots = _offer(tz="Europe/Moscow")
    assert slots
    # 09:00..19:00 МСК = 06:00..16:00 UTC
    assert slots[0].start == datetime(2026, 9, 14, 6, 0, tzinfo=timezone.utc)
    assert slots[-1].start == datetime(2026, 9, 14, 15, 0, tzinfo=timezone.utc)
    for slot in slots:
        assert slot.start.utcoffset() == timedelta(0)
        assert slot.start >= datetime(2026, 9, 14, 6, 0, tzinfo=timezone.utc)
        assert slot.end <= datetime(2026, 9, 14, 16, 0, tzinfo=timezone.utc)


def test_unknown_timezone_falls_back_to_utc():
    slots = _offer(tz="Mars/Olympus")
    assert slots[0].start == datetime(2026, 9, 14, 9, 0, tzinfo=timezone.utc)
    assert slots == _offer(tz="UTC")


def test_preferred_hours_are_local():
    # per_day=1 и предпочтение 15:00 МСК → слот 12:00 UTC
    slots = _offer(tz="Europe/Moscow", prefs=_prefs(preferred=[15]), per_day=1)
    assert [s.start for s in slots] == [datetime(2026, 9, 14, 12, 0, tzinfo=timezone.utc)]
    assert slots[0].reason == "удобное время"


def test_half_hour_offset_timezone_rounds_locally():
    # Индия UTC+5:30: слоты на :00/:30 местного времени, 09:00 IST = 03:30 UTC
    slots = _offer(tz="Asia/Kolkata", now=MONDAY - timedelta(hours=6))
    assert slots[0].start == datetime(2026, 9, 14, 3, 30, tzinfo=timezone.utc)


# ---------------------------------------------------- рабочие часы и календарь
def test_meeting_must_end_within_work_hours():
    # Весь день до 18:00 занят: остаётся только 18:00–19:00 (18:30 вышло бы за 19:00).
    busy = [_event(MONDAY.replace(hour=9), MONDAY.replace(hour=18))]
    slots = _offer(calendar=busy, hours=20)
    assert [(s.start.hour, s.end.hour) for s in slots] == [(18, 19)]


def test_non_contiguous_work_hours():
    # Обед 13:00–14:00 не рабочий: двухчасовая встреча не может его захватить.
    work = [9, 10, 11, 12, 14, 15, 16, 17]
    slots = _offer(prefs=_prefs(work=work), duration=120, hours=20)
    for slot in slots:
        hours = {(slot.start + timedelta(minutes=m)).hour for m in range(0, 120, 15)}
        assert 13 not in hours and hours <= set(work)


def test_busy_events_are_excluded():
    busy = [_event(MONDAY.replace(hour=10), MONDAY.replace(hour=12, minute=30))]
    slots = _offer(calendar=busy, duration=45)
    assert slots
    for slot in slots:
        assert slot.end <= busy[0].start or slot.start >= busy[0].end


def test_empty_work_hours_use_default():
    slots = _offer(prefs=_prefs(work=[]))
    assert slots[0].start.hour == 9 and slots[-1].end.hour == 19


# ------------------------------------------------------------------ выходные
def test_weekends_skipped_when_weekdays_available():
    friday_evening = datetime(2026, 9, 18, 20, 0, tzinfo=timezone.utc)
    slots = _offer(now=friday_evening, hours=72)  # сб, вс, пн
    assert slots
    assert {s.start.weekday() for s in slots} == {0}


def test_weekend_fallback_when_no_weekday_slots():
    saturday = datetime(2026, 9, 19, 0, 0, tzinfo=timezone.utc)
    slots = _offer(now=saturday, hours=48, per_day=2)
    assert {s.start.weekday() for s in slots} == {5, 6}
    assert all("выходной" in s.reason for s in slots)


def test_weekend_is_local_day():
    # 15:00 UTC пятницы — уже суббота в Токио: в окне только сб/вс по Токио.
    friday_late = datetime(2026, 9, 18, 15, 0, tzinfo=timezone.utc)  # 00:00 сб в Токио
    slots = _offer(now=friday_late, hours=48, tz="Asia/Tokyo", per_day=1)
    assert slots
    assert all(s.start.astimezone(planner._zone("Asia/Tokyo")).weekday() >= 5 for s in slots)


# ---------------------------------------------------------- лимиты и порядок
def test_per_day_cap_and_spread():
    slots = _offer(hours=72, per_day=2, max_slots=12)  # пн, вт, ср
    per_day = {}
    for slot in slots:
        per_day[slot.start.date()] = per_day.get(slot.start.date(), 0) + 1
    assert per_day and max(per_day.values()) <= 2
    assert len(per_day) == 3
    # слоты одного дня не перекрываются
    for day in per_day:
        same = [s for s in slots if s.start.date() == day]
        for a, b in zip(same, same[1:]):
            assert a.end <= b.start


def test_max_slots_spreads_over_days_first():
    slots = _offer(hours=24 * 5, per_day=2, max_slots=3)  # пн..пт
    assert len(slots) == 3
    assert len({s.start.date() for s in slots}) == 3


def test_result_is_chronological():
    slots = _offer(hours=24 * 7, per_day=2, max_slots=6, prefs=_prefs(preferred=[16, 17]))
    assert len(slots) == 6
    assert [s.start for s in slots] == sorted(s.start for s in slots)


# ------------------------------------------------------------- границы окна
def test_lead_time_one_hour_rounded_up():
    now = MONDAY.replace(hour=10, minute=10)
    slots = _offer(now=now, hours=10)
    assert slots[0].start == MONDAY.replace(hour=11, minute=30)
    assert all(s.start >= now + timedelta(hours=1) for s in slots)


def test_window_bounds_respected():
    now = MONDAY.replace(hour=7)
    start = MONDAY.replace(hour=12)
    slots = _offer(now=now, start=start, hours=3, duration=60)
    assert slots
    assert all(s.start >= start and s.end <= start + timedelta(hours=3) for s in slots)
    assert slots[-1].end == MONDAY.replace(hour=15)


def test_window_too_short_gives_nothing():
    now = MONDAY.replace(hour=9)
    assert _offer(now=now, hours=1) == []  # через час окно уже кончилось


def test_window_outside_work_hours_gives_nothing():
    now = MONDAY.replace(hour=19)
    assert _offer(now=now, hours=12) == []


# ------------------------------------------------------------------ HTTP API
def _agent() -> SelfvoyAgent:
    settings = Settings()
    settings.memory_backend = "memory://"
    return SelfvoyAgent(settings=settings, llm=MockLLM(), store=memory_module.InMemoryStore(), tools=ToolManager())


@pytest.fixture()
def env(monkeypatch):
    monkeypatch.setenv("SELFVOY_MEMORY_BACKEND", "memory://")
    monkeypatch.delenv("SELFVOY_AI_TOKEN", raising=False)
    monkeypatch.delenv("SELFVOY_AI_INSECURE_DEV", raising=False)
    reset_settings()
    app_module.set_agent(_agent())
    yield monkeypatch
    app_module.set_agent(None)
    reset_settings()


@pytest.fixture()
def client(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    reset_settings()
    with TestClient(app_module.app, raise_server_exceptions=False) as test_client:
        yield test_client


def _future_monday() -> datetime:
    """Понедельник 00:00 UTC через 2+ недели — тест не зависит от сегодняшней даты."""
    base = (datetime.now(timezone.utc) + timedelta(days=14)).replace(hour=0, minute=0, second=0, microsecond=0)
    return base + timedelta(days=(7 - base.weekday()) % 7)


def _parse(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def test_slots_endpoint_moscow(client):
    monday = _future_monday()
    body = {
        "context": {
            "user_id": 1,
            "now": monday.isoformat(),
            "timezone": "Europe/Moscow",
            "preferences": {"work_hours": WORK, "preferred_hours": [10, 15]},
            "calendar": [
                {"title": "планёрка", "start": (monday + timedelta(hours=7)).isoformat(),
                 "end": (monday + timedelta(hours=8)).isoformat()},
            ],
        },
        "duration_minutes": 60,
        "window_hours": 72,
        "max_slots": 6,
    }
    response = client.post("/v1/agent/slots", json=body)
    assert response.status_code == 200, response.text
    slots = response.json()["slots"]
    assert len(slots) == 6
    assert set(slots[0]) == {"start", "end", "score", "reason", "day", "local"}
    starts = [_parse(s["start"]) for s in slots]
    assert starts == sorted(starts)
    for slot in slots:
        start, end = _parse(slot["start"]), _parse(slot["end"])
        assert start.utcoffset() == timedelta(0)
        assert end - start == timedelta(minutes=60)
        assert start.hour >= 6 and end <= start.replace(hour=16, minute=0)
        # планёрка 07:00–08:00 UTC (10:00–11:00 МСК) не задета
        assert end <= monday + timedelta(hours=7) or start >= monday + timedelta(hours=8)
    # предпочтительные 10:00 и 15:00 МСК → 07:00 (занято) и 12:00 UTC
    assert any(s.hour == 12 for s in starts)


def test_slots_endpoint_never_in_the_past(client):
    stale = datetime(2020, 1, 1, tzinfo=timezone.utc)
    body = {"context": {"now": stale.isoformat(), "timezone": "UTC", "preferences": {"work_hours": list(range(24))}}}
    response = client.post("/v1/agent/slots", json=body)
    assert response.status_code == 200
    soon = datetime.now(timezone.utc) + timedelta(minutes=59)
    assert all(_parse(s["start"]) >= soon for s in response.json()["slots"])


def test_slots_endpoint_defaults(client):
    response = client.post("/v1/agent/slots", json={"context": {"timezone": "UTC",
                                                                "preferences": {"work_hours": list(range(24))}}})
    assert response.status_code == 200
    assert 1 <= len(response.json()["slots"]) <= 6


@pytest.mark.parametrize(
    "extra",
    [
        {"duration_minutes": 14},
        {"duration_minutes": 241},
        {"window_hours": 0},
        {"window_hours": OFFER_MAX_WINDOW_HOURS + 1},
        {"max_slots": 0},
        {"max_slots": OFFER_MAX_SLOTS + 1},
        {"per_day": 0},
        {"per_day": OFFER_MAX_PER_DAY + 1},
        {"lead_minutes": -1},
        {"lead_minutes": 7 * 24 * 60 + 1},
        {"weekends": "иногда"},
        {"duration_minutes": "долго"},
    ],
)
def test_slots_endpoint_bad_input_is_422(client, extra):
    response = client.post("/v1/agent/slots", json={"context": {}, **extra})
    assert response.status_code == 422, response.text


def test_slots_endpoint_requires_context(client):
    assert client.post("/v1/agent/slots", json={"duration_minutes": 60}).status_code == 422


def test_slots_endpoint_time_overflow_is_422(client):
    body = {"context": {"now": "9999-12-31T23:00:00Z"}}
    assert client.post("/v1/agent/slots", json=body).status_code == 422


def test_slots_endpoint_auth_like_negotiate(env):
    negotiate = {"initiator": {}, "responder": {}}
    slots = {"context": {}}
    # без токена и без режима разработки — оба закрыты (503)
    with TestClient(app_module.app) as test_client:
        assert test_client.post("/v1/agent/negotiate", json=negotiate).status_code == 503
        assert test_client.post("/v1/agent/slots", json=slots).status_code == 503
    # токен задан: без него/с чужим — 401, с верным — 200
    env.setenv("SELFVOY_AI_TOKEN", "secret")
    reset_settings()
    with TestClient(app_module.app) as test_client:
        for path, body in (("/v1/agent/negotiate", negotiate), ("/v1/agent/slots", slots)):
            assert test_client.post(path, json=body).status_code == 401
            assert test_client.post(path, json=body, headers={"X-Selfvoy-Token": "wrong"}).status_code == 401
            assert test_client.post(path, json=body, headers={"X-Selfvoy-Token": "secret"}).status_code == 200


# ------------------------------------------------- страница записи: per_day, lead, weekends, day
SATURDAY = MONDAY + timedelta(days=5)


def test_lead_shifts_first_slot():
    # Без запаса — первый слот в 09:00; запас 3 часа от 08:00 → не раньше 11:00.
    now = MONDAY + timedelta(hours=8)
    first = planner.offer_slots(_window(now, 24), [], _prefs(), "UTC", max_slots=50, per_day=50, now=now,
                                lead=timedelta(0))[0]
    assert first.start == MONDAY + timedelta(hours=9)
    first = planner.offer_slots(_window(now, 24), [], _prefs(), "UTC", max_slots=50, per_day=50, now=now,
                                lead=timedelta(hours=3))[0]
    assert first.start == MONDAY + timedelta(hours=11)


def test_negative_lead_is_clamped_to_zero():
    now = MONDAY + timedelta(hours=9)
    slots = planner.offer_slots(_window(now, 24), [], _prefs(), "UTC", now=now, lead=timedelta(hours=-5))
    assert slots and slots[0].start >= now


def test_weekends_true_mixes_weekdays_and_weekends():
    # Пятница + суббота: True — слоты обоих дней, None — только пятница, False — только пятница.
    friday = MONDAY + timedelta(days=4)
    kwargs = dict(max_slots=50, per_day=1, now=friday)
    both = planner.offer_slots(_window(friday, 48), [], _prefs(), "UTC", weekends=True, **kwargs)
    assert {s.start.weekday() for s in both} == {4, 5}
    assert [s.start for s in both] == sorted(s.start for s in both)
    fallback = planner.offer_slots(_window(friday, 48), [], _prefs(), "UTC", weekends=None, **kwargs)
    assert {s.start.weekday() for s in fallback} == {4}
    strict = planner.offer_slots(_window(friday, 48), [], _prefs(), "UTC", weekends=False, **kwargs)
    assert {s.start.weekday() for s in strict} == {4}


def test_weekends_false_never_falls_back():
    # Окно только из выходных: None отдаёт выходные, False — пусто.
    assert planner.offer_slots(_window(SATURDAY, 48), [], _prefs(), "UTC", now=SATURDAY, weekends=None)
    assert planner.offer_slots(_window(SATURDAY, 48), [], _prefs(), "UTC", now=SATURDAY, weekends=False) == []


def test_weekend_reason_depends_on_mode():
    slot = planner.offer_slots(_window(SATURDAY, 24), [], _prefs(), "UTC", now=SATURDAY, weekends=True)[0]
    assert slot.reason == "выходной день"


def test_per_day_many_slots_do_not_overlap():
    slots = _offer(per_day=8, max_slots=50)
    assert len(slots) == 8
    for first, second in zip(slots, slots[1:]):
        assert first.end <= second.start


def test_day_is_owner_local_date():
    # 23:30 UTC воскресенья = 09:30 понедельника во Владивостоке (UTC+10).
    sunday_late = MONDAY - timedelta(minutes=30)
    slots = planner.offer_slots(_window(sunday_late, 24), [], _prefs(), "Asia/Vladivostok",
                                max_slots=50, per_day=50, now=sunday_late - timedelta(hours=2))
    assert slots[0].start == sunday_late
    assert slots[0].day == "2026-09-14"
    assert all(s.day == s.start.astimezone(planner._zone("Asia/Vladivostok")).date().isoformat() for s in slots)


def test_slots_endpoint_booking_params(client):
    monday = _future_monday()
    body = {
        "context": {"timezone": "UTC", "preferences": {"work_hours": WORK}, "now": monday.isoformat()},
        "duration_minutes": 30,
        "window_hours": 24 * 7,
        "max_slots": 100,
        "per_day": 6,
        "lead_minutes": 0,
        "weekends": False,
    }
    response = client.post("/v1/agent/slots", json=body)
    assert response.status_code == 200, response.text
    slots = response.json()["slots"]
    days = {}
    for slot in slots:
        start = _parse(slot["start"])
        assert start.weekday() < 5
        assert slot["day"] == start.date().isoformat()
        days[slot["day"]] = days.get(slot["day"], 0) + 1
    assert days and max(days.values()) == 6


def test_local_is_owner_wall_time():
    slot = _offer(tz="Europe/Moscow")[0]
    assert slot.local == "2026-09-14T09:00"


def test_booking_mode_fills_nearest_days_first():
    # Страница записи (per_day > 2) на длинном окне: ближайшие дни — целиком,
    # а не по паре вариантов на каждый из 60 дней.
    slots = _offer(hours=60 * 24, per_day=16, max_slots=20, duration=60)
    days = sorted({slot.start.date() for slot in slots})
    first_day = [slot for slot in slots if slot.start.date() == days[0]]
    assert len(slots) == 20
    assert len(first_day) == len(WORK)  # весь рабочий день: по слоту на час
    assert len(days) <= 3
    assert slots == sorted(slots, key=lambda slot: slot.start)
