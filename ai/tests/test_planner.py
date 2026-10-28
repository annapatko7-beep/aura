"""Тесты планировщика: разбор времени, намерений, поиск слотов."""

from datetime import datetime, timedelta, timezone

import pytest

from selfvoy_ai import planner
from selfvoy_ai.models import CalendarEvent, Preferences

NOW = datetime(2026, 9, 17, 14, 0, tzinfo=timezone.utc)  # четверг


def test_detect_intent_meeting():
    assert planner.detect_intent("Хочу встретиться с другом, чтобы обсудить стартап") == "schedule_meeting"


def test_detect_intent_reminder_and_default():
    assert planner.detect_intent("напомни позвонить маме") == "reminder"
    assert planner.detect_intent("привет") == "chat"


@pytest.mark.parametrize(
    "text,label",
    [
        ("встретимся завтра", "завтра"),
        ("встретимся в эти выходные", "выходные"),
        ("созвон в пятницу", "пятница"),
        ("послезавтра обсудим", "послезавтра"),
    ],
)
def test_parse_time_window_labels(text, label):
    window = planner.parse_time_window(text, NOW)
    assert window is not None
    assert window.label == label


def test_parse_time_window_tomorrow_bounds():
    window = planner.parse_time_window("завтра в 18:30", NOW)
    assert window.start == datetime(2026, 9, 18, 18, 30, tzinfo=timezone.utc)
    assert window.end - window.start == timedelta(hours=3)


def test_parse_time_window_weekend_starts_on_saturday():
    window = planner.parse_time_window("встреча на выходных", NOW)
    assert window.start.weekday() == 5  # суббота
    assert window.end - window.start == timedelta(days=2)


def test_parse_time_window_relative():
    window = planner.parse_time_window("напомни через 2 часа", NOW)
    assert window.start == NOW + timedelta(hours=2)
    assert window.source == "relative"


def test_parse_duration():
    assert planner.parse_duration("встреча на 30 минут", 60) == 30
    assert planner.parse_duration("созвон на 2 часа", 60) == 120
    assert planner.parse_duration("просто поболтать", 60) == 60


def test_understand_extracts_person_and_topic():
    parsed = planner.understand("Хочу встретиться с Аней в пятницу обсудить стартап", NOW)
    assert parsed.intent == "schedule_meeting"
    assert parsed.person == "Аней"
    assert "стартап" in parsed.topic


def test_free_slots_skips_busy_time():
    window = planner.TimeWindow(start=NOW + timedelta(hours=1), end=NOW + timedelta(hours=6))
    calendar = [
        CalendarEvent(
            title="standup",
            start=NOW + timedelta(hours=1),
            end=NOW + timedelta(hours=3),
        )
    ]
    slots = planner.free_slots(window, calendar, Preferences(), duration_minutes=60)
    assert slots, "должен найтись хотя бы один свободный слот"
    for slot in slots:
        assert slot.start >= NOW + timedelta(hours=3)


def test_free_slots_limit_and_order():
    window = planner.TimeWindow(start=NOW, end=NOW + timedelta(hours=8))
    slots = planner.free_slots(window, [], Preferences(), duration_minutes=60, max_slots=3)
    assert len(slots) == 3
    scores = [s.score for s in slots]
    assert scores == sorted(scores, reverse=True)


def test_intersect_slots_only_overlapping():
    left = [
        planner.Slot(start=NOW + timedelta(hours=1), end=NOW + timedelta(hours=3), score=0.8),
    ]
    right = [
        planner.Slot(start=NOW + timedelta(hours=2), end=NOW + timedelta(hours=5), score=0.6),
        planner.Slot(start=NOW + timedelta(hours=9), end=NOW + timedelta(hours=10), score=0.9),
    ]
    merged = planner.intersect_slots(left, right, duration_minutes=60)
    assert len(merged) == 1
    assert merged[0].start == NOW + timedelta(hours=2)
    assert merged[0].score == pytest.approx(0.7)


def test_intersect_slots_empty_when_no_overlap():
    left = [planner.Slot(start=NOW, end=NOW + timedelta(hours=1), score=0.8)]
    right = [planner.Slot(start=NOW + timedelta(hours=5), end=NOW + timedelta(hours=6), score=0.8)]
    assert planner.intersect_slots(left, right, duration_minutes=60) == []


def test_score_slot_prefers_preferred_hours():
    prefs = Preferences(preferred_hours=[18])
    preferred = planner.score_slot(NOW.replace(hour=18, minute=0), prefs, [])
    other = planner.score_slot(NOW.replace(hour=3, minute=0), prefs, [])
    assert preferred > other


def test_hour_without_minutes():
    assert planner.parse_time_window("завтра в 10", NOW).start.hour == 10
    assert planner.parse_time_window("завтра в 7 вечера", NOW).start.hour == 19
    assert planner.parse_time_window("в пятницу в 3 дня", NOW).start.hour == 15
    assert planner.parse_time_window("завтра в 19 часов", NOW).start.hour == 19
    # «в 2 раза» — не время
    assert planner.parse_time_window("сделай в 2 раза быстрее", NOW).start == NOW


def test_reminder_text_strips_command_and_time():
    assert planner.reminder_text("Напомни завтра в 10 позвонить юристу") == "Позвонить юристу"
    assert planner.reminder_text("напомни мне в пятницу в 18:30 забрать посылку") == "Забрать посылку"
    assert planner.reminder_text("Не забудь через 2 часа выключить духовку") == "Выключить духовку"
    assert planner.reminder_text("напомни, что завтра вечером встреча с Димой") == "Встреча с Димой"
    assert planner.reminder_text("напомни") == "напомни"


# --------------------------------------------- огромные числа в тексте (аудит)
@pytest.mark.parametrize(
    "text",
    [
        "напомни через 99999999 недель позвонить",
        "через 999999999 недель",
        "через " + "9" * 5000 + " дней",
        "созвон через 3651 день",
    ],
)
def test_huge_relative_offset_is_not_parsed_as_time(text):
    window = planner.parse_time_window(text, NOW)
    assert window is not None
    assert window.source != "relative"
    assert window.start - NOW < timedelta(days=8)


def test_relative_offset_within_ten_years_still_parsed():
    window = planner.parse_time_window("созвон через 3650 дней", NOW)
    assert window.source == "relative"
    assert window.start == NOW + timedelta(days=planner.MAX_RELATIVE_DAYS)
    assert planner.parse_time_window("через 05 дней", NOW).label == "через 5 дней"


@pytest.mark.parametrize(
    "text,expected",
    [
        ("созвон на 99999999 часов", 24 * 60),
        ("созвон на " + "9" * 5000 + " минут", 60),
        ("созвон на 0 минут", 1),
        ("созвон на 2 часа", 120),
    ],
)
def test_parse_duration_clamped(text, expected):
    assert planner.parse_duration(text) == expected


def test_understand_huge_numbers_does_not_raise():
    result = planner.understand("встреча через " + "9" * 4400 + " недель на " + "9" * 3000 + " часов", NOW)
    assert result.window is not None
    assert result.duration_minutes == 60


# ------------------------------------------------ BusyIndex = прежний перебор
def test_busy_index_matches_linear_overlap_check():
    import random

    rnd = random.Random(42)
    events = []
    for _ in range(300):
        start = NOW + timedelta(minutes=15 * rnd.randint(-200, 1500))
        # в том числе события нулевой длины и вложенные/соприкасающиеся
        events.append(CalendarEvent(start=start, end=start + timedelta(minutes=15 * rnd.choice([0, 1, 2, 4, 40]))))
    linear = planner._busy_intervals(events)
    window_start, window_end = NOW, NOW + timedelta(days=10)
    index = planner.BusyIndex(events, window_start, window_end)
    assert len(index) <= len(linear)
    for _ in range(3000):
        a = window_start + timedelta(minutes=15 * rnd.randint(0, 950))
        b = a + timedelta(minutes=15 * rnd.randint(1, 16))
        if b > window_end:
            continue
        assert index.overlaps(a, b) == planner._overlaps(a, b, linear), (a, b)


def test_free_slots_large_calendar_outside_window_is_fast():
    import time

    far = NOW + timedelta(days=400)
    events = [CalendarEvent(start=far + timedelta(hours=i), end=far + timedelta(hours=i, minutes=30)) for i in range(2000)]
    events.append(CalendarEvent(start=NOW + timedelta(hours=2), end=NOW + timedelta(hours=3)))
    window = planner.TimeWindow(start=NOW, end=NOW + timedelta(days=31))
    started = time.perf_counter()
    slots = planner.free_slots(window, events, Preferences(), duration_minutes=60, max_slots=50)
    assert time.perf_counter() - started < 2.0
    assert slots
    busy = (NOW + timedelta(hours=2), NOW + timedelta(hours=3))
    assert all(not (s.start < busy[1] and busy[0] < s.end) for s in slots)
