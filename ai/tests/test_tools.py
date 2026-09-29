"""Тесты инструментов (tools.py)."""

from datetime import datetime, timedelta, timezone

import pytest

from aura_ai.models import AgentContext, CalendarEvent, Preferences
from aura_ai.tools import ToolBackends, ToolError, ToolManager, find_cafe

NOW = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)


def make_context(**kwargs) -> AgentContext:
    prefs = Preferences(
        diet=["vegan"],
        city="Керкраде",
        budget_limit=3.0,
        lat=50.861,
        lon=6.064,
        preferred_hours=[10, 18],
    )
    return AgentContext(
        user_id=1,
        email="me@example.com",
        display_name="Анна",
        message="найдём кофейню",
        now=NOW,
        preferences=prefs,
        **kwargs,
    )


def test_find_cafe_respects_diet():
    result = find_cafe(Preferences(diet=["vegan"]), city="Керкраде")
    assert result["results"]
    for cafe in result["results"]:
        assert "meat" not in cafe["tags"]


def test_find_cafe_prefers_middle_point():
    midpoint = {"lat": 50.849, "lon": 5.691}  # рядом с Маастрихтом
    result = find_cafe(Preferences(), midpoint=midpoint, limit=1)
    assert result["results"][0]["name"] == "Matcha Point"


def test_tool_manager_lists_registry():
    manager = ToolManager()
    names = {tool["name"] for tool in manager.list()}
    assert {"find_cafe", "book_table", "send_message", "create_reminder"} <= names


def test_run_book_table_returns_confirmation():
    manager = ToolManager()
    result = manager.run(
        "book_table",
        {"place": "Кофе на полпути", "at": (NOW + timedelta(hours=3)).isoformat(), "people": 2},
        make_context(),
    )
    assert result["confirmation"].startswith("AURA-")
    assert result["people"] == 2


def test_run_book_table_validates_place():
    manager = ToolManager()
    with pytest.raises(ToolError):
        manager.run("book_table", {"place": "", "at": NOW.isoformat()}, make_context())


def test_run_send_message_requires_recipient():
    manager = ToolManager()
    with pytest.raises(ToolError):
        manager.run("send_message", {"to": "", "text": "привет"}, make_context())


def test_run_unknown_tool():
    with pytest.raises(ToolError):
        ToolManager().run("hack_the_planet", {}, make_context())


def test_suggest_time_returns_slots():
    manager = ToolManager()
    result = manager.run(
        "suggest_time",
        {
            "window": {"start": NOW.isoformat(), "end": (NOW + timedelta(hours=6)).isoformat()},
            "duration_minutes": 60,
        },
        make_context(),
    )
    assert result["duration_minutes"] == 60
    assert len(result["slots"]) >= 1


def test_check_calendar_reports_busy():
    context = make_context(
        calendar=[
            CalendarEvent(
                title="созвон",
                start=NOW + timedelta(hours=1),
                end=NOW + timedelta(hours=2),
            )
        ]
    )

    manager = ToolManager()
    result = manager.run(
        "check_calendar",
        {"window": {"start": NOW.isoformat(), "end": (NOW + timedelta(hours=3)).isoformat()}},
        context,
    )
    assert len(result["busy"]) == 1
    assert result["busy"][0]["title"] == "созвон"


def test_tool_manager_logs_executions():
    manager = ToolManager()
    manager.run("create_note", {"text": "идея для стартапа"}, make_context())
    assert manager.log[-1]["tool"] == "create_note"


def test_http_backend_falls_back_when_no_url():
    backends = ToolBackends(mode="http", urls={"maps": ""})
    result = find_cafe(Preferences(), city="Керкраде", backends=backends)
    assert result["results"]  # sandbox-данные вместо сетевых
