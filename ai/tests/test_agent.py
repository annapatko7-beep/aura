"""Тесты агента: намерения, действия, память, Agent-to-Agent."""

from datetime import datetime, timedelta, timezone
from typing import Any, Dict


from selfvoy_ai.agent import SelfvoyAgent, _normalize_name
from selfvoy_ai.llm import BaseLLM, MockLLM
from selfvoy_ai.models import (
    AgentContext,
    AgentRequest,
    CalendarEvent,
    NegotiateRequest,
    Preferences,
    utcnow,
)
from selfvoy_ai.tools import ToolManager

NOW = datetime(2026, 9, 17, 14, 0, tzinfo=timezone.utc)  # четверг


def make_agent(llm: BaseLLM | None = None) -> SelfvoyAgent:
    from selfvoy_ai.config import Settings

    settings = Settings()
    settings.memory_backend = "memory://"
    return SelfvoyAgent(settings=settings, llm=llm or MockLLM(), tools=ToolManager())


def user_context(**overrides: Any) -> AgentContext:
    data: Dict[str, Any] = dict(
        user_id=1,
        email="anna@example.com",
        display_name="Анна",
        message="Хочу встретиться с Аней в эти выходные обсудить стартап",
        now=NOW,
        timezone="Europe/Moscow",
        preferences=Preferences(
            diet=["vegan"],
            city="Керкраде",
            budget_limit=3.0,
            lat=50.861,
            lon=6.064,
            preferred_hours=[10, 11, 18],
        ),
    )
    data.update(overrides)
    return AgentContext(**data)


def peer_context(**overrides: Any) -> AgentContext:
    data: Dict[str, Any] = dict(
        user_id=2,
        email="anya@example.com",
        display_name="Аня",
        preferences=Preferences(diet=["gluten_free"], city="Херлен", lat=50.888, lon=5.978),
    )
    data.update(overrides)
    return AgentContext(**data)


def test_run_detects_meeting_and_plans_actions():
    response = make_agent().run(AgentRequest(context=user_context(), execute=True))
    assert response.intent == "schedule_meeting"
    assert {call.tool for call in response.actions} >= {"check_calendar", "suggest_time", "send_message"}
    assert response.results, "действия должны выполниться"
    by_tool = {result.tool: result for result in response.results}
    assert by_tool["check_calendar"].ok and by_tool["suggest_time"].ok
    # Внешнее действие AI-сервис не исполняет — только сервер (tool_permissions).
    assert by_tool["send_message"].ok is False
    assert by_tool["send_message"].error.startswith("server_only")
    assert response.plan.steps[0].title == "Понял задачу"


def test_run_performs_agent_to_agent_negotiation():
    response = make_agent().run(
        AgentRequest(context=user_context(), peers=[peer_context()])
    )
    assert response.a2a is not None
    assert response.a2a.target in ("anya@example.com", "Аня")
    assert response.a2a.slots, "должно найтись общее окно"
    assert response.a2a.slots[0].start.weekday() in (5, 6)  # выходные
    assert response.a2a.place.get("name")
    assert "договорился" in response.reply.lower()


def test_a2a_reports_counter_slots_when_schedules_conflict():
    weekend_start = datetime(2026, 9, 19, 0, 0, tzinfo=timezone.utc)
    busy_peer = peer_context(
        calendar=[
            CalendarEvent(title="занято", start=weekend_start, end=weekend_start + timedelta(days=2))
        ]
    )
    response = make_agent().run(AgentRequest(context=user_context(), peers=[busy_peer]))
    assert response.a2a is not None
    assert "Общих окон пока нет" in response.a2a.message


def test_run_without_peer_still_returns_proposal():
    response = make_agent().run(AgentRequest(context=user_context(), peers=[]))
    assert response.a2a is not None
    assert response.a2a.slots == []


def test_dry_run_does_not_execute_or_persist():
    agent = make_agent()
    response = agent.run(AgentRequest(context=user_context(message="напомни завтра позвонить"), dry_run=True))
    assert response.results == []
    assert agent.store.load("anna@example.com") == []


def test_reminder_intent_creates_reminder():
    response = make_agent().run(
        AgentRequest(context=user_context(message="напомни завтра в 9:00 позвонить врачу"), execute=True)
    )
    assert response.intent == "reminder"
    reminder = next(r for r in response.results if r.tool == "create_reminder")
    assert reminder.ok
    # 9:00 по Москве (timezone пользователя) = 06:00 UTC; в задаче — суть без «напомни завтра в 9:00».
    assert reminder.data["fire_at"].startswith("2026-09-18T06:00")
    assert reminder.data["text"] == "Позвонить врачу"
    assert response.reply == "Напомню завтра в 9:00: позвонить врачу."


def test_reminder_hour_without_minutes_and_day_default():
    lawyer = make_agent().run(
        AgentRequest(context=user_context(message="Напомни завтра в 10 позвонить юристу"), execute=True)
    )
    reminder = next(r for r in lawyer.results if r.tool == "create_reminder")
    assert reminder.data["fire_at"].startswith("2026-09-18T07:00")
    assert reminder.data["text"] == "Позвонить юристу"

    coffee = make_agent().run(
        AgentRequest(context=user_context(message="напомни завтра купить кофе"), execute=True)
    )
    reminder = next(r for r in coffee.results if r.tool == "create_reminder")
    # День без часа — 9 утра по местному времени, не полночь.
    assert reminder.data["fire_at"].startswith("2026-09-18T06:00")
    assert reminder.data["text"] == "Купить кофе"


def test_memory_updates_are_saved():
    agent = make_agent()
    agent.run(AgentRequest(context=user_context(message="Я люблю матча-латте, запомни"), execute=True))
    stored = agent.store.load("anna@example.com")
    assert any("матча-латте" in entry.text for entry in stored)


def test_unknown_tool_from_llm_is_filtered():
    class RogueLLM(BaseLLM):
        name = "rogue"

        def complete_json(self, context, understanding, history=None):
            return {
                "reply": "делаю",
                "intent": "chat",
                "confidence": 0.9,
                "actions": [
                    {"id": "x1", "tool": "drop_database", "args": {}},
                    {"id": "x2", "tool": "create_note", "args": {"text": "ок"}},
                ],
            }

    response = make_agent(RogueLLM()).run(AgentRequest(context=user_context(message="привет"), execute=True))
    assert [call.tool for call in response.actions] == ["create_note"]
    assert len(response.results) == 1


def test_llm_failure_falls_back_to_local_rules():
    from selfvoy_ai.llm import LLMError

    class BrokenLLM(BaseLLM):
        name = "broken"

        def complete_json(self, context, understanding, history=None):
            raise LLMError("network down")

    response = make_agent(BrokenLLM()).run(
        AgentRequest(context=user_context(message="напомни завтра купить кофе"))
    )
    assert response.intent == "reminder"
    # Видно, что настоящая модель не ответила: сработали локальные правила.
    assert response.llm == "broken-fallback"


def test_negotiate_accepts_shared_slot():
    agent = make_agent()
    result = agent.negotiate(
        NegotiateRequest(
            initiator=user_context(),
            responder=peer_context(),
            duration_minutes=60,
            window_hours=72,
        )
    )
    assert result.accepted is True
    assert result.slot is not None
    assert result.slot.end - result.slot.start == timedelta(hours=1)
    assert result.place.get("name")


def test_negotiate_rejects_when_responder_busy():
    agent = make_agent()
    # «Занято» считаем от текущего момента: negotiate() строит окно от utcnow(),
    # поэтому фиксированная дата в прошлом со временем перестала бы перекрывать
    # окно переговоров и тест начал бы ложно проходить/падать в зависимости от дня.
    now = utcnow()
    busy = peer_context(
        calendar=[
            CalendarEvent(title="busy", start=now - timedelta(hours=1), end=now + timedelta(hours=72))
        ]
    )
    result = agent.negotiate(
        NegotiateRequest(initiator=user_context(), responder=busy, window_hours=48)
    )
    assert result.accepted is False
    assert result.slot is None


def test_normalize_name_handles_russian_cases():
    assert _normalize_name("Аней") == _normalize_name("Аня")
    assert _normalize_name("Анной") == _normalize_name("Анна")
    assert _normalize_name("") == ""


def test_plan_contains_window_constraint():
    response = make_agent().run(AgentRequest(context=user_context()))
    assert response.plan.constraints["person"] == "Аней"
    assert response.plan.constraints["window"]["label"] == "выходные"


def test_negotiate_without_place_when_privacy_forbids():
    """prefs.privacy.allow_place_choice=false у одной из сторон: время ищем, место — нет."""
    result = make_agent().negotiate(
        NegotiateRequest(
            initiator=user_context(),
            responder=peer_context(),
            duration_minutes=60,
            window_hours=72,
            choose_place=False,
        )
    )
    assert result.accepted is True
    assert not result.place
    assert "Место выберете сами" in result.message

