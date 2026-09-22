"""Тесты планировщика: разбор времени, намерений, поиск слотов."""

from datetime import datetime, timedelta, timezone

import pytest

from aura_ai import planner
from aura_ai.models import CalendarEvent, Preferences

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
