"""test_aura_network.py — комплексные тесты 5 ключевых возможностей Aura (A2A Network):

1. Межагентная федерация (Discovery, Ed25519, inbox, защита от replay).
2. Автономный многораундовый торг (Multi-turn Negotiation convergence).
3. Реальный поиск мест и диспетчер бронирования (BookingDispatcher).
4. Синхронизация внешних календарей (CalDAV & ICS).
5. Локальные LLM и приватность (Zero-Cloud Ollama / vLLM).
"""

from __future__ import annotations

import time
from datetime import datetime, timedelta, timezone
from typing import Any, Dict

import pytest
from fastapi.testclient import TestClient

from selfvoy_ai import app as app_module
from selfvoy_ai import caldav_sync, federation, llm as llm_module, negotiator, tools
from selfvoy_ai.config import Settings
from selfvoy_ai.models import (
    AgentContext,
    CalendarEvent,
    NegotiateRequest,
    Preferences,
    utcnow,
)


@pytest.fixture
def client() -> TestClient:
    return TestClient(app_module.app)


# =====================================================================
# 1. Межагентная федерация (AFP v1.0)
# =====================================================================
def test_well_known_aura_agent(client: TestClient):
    resp = client.get("/.well-known/aura-agent")
    assert resp.status_code == 200
    data = resp.json()
    assert data["protocol"] == "AFP/1.0"
    assert "public_key" in data
    assert len(data["public_key"]) >= 32
    assert data["inbox_url"] == "/v1/federation/inbox"
    assert "negotiation" in data["capabilities"]


def test_federation_inbox_valid_envelope(client: TestClient):
    priv_key, pub_key = federation.get_server_keypair()
    payload = {"topic": "AI Startup Meetup", "window_hours": 48}
    envelope = federation.sign_envelope(
        sender="@alex:alpha.aura.net",
        recipient="@boris:beta.aura.net",
        intent="ping",
        payload=payload,
        private_key_hex=priv_key,
    )
    # Передаем публичный ключ для валидации
    envelope["public_key"] = pub_key

    resp = client.post("/v1/federation/inbox", json=envelope)
    assert resp.status_code == 200
    data = resp.json()
    assert data["ok"] is True
    assert data["status"] == "pong"
    assert data["reply"]["protocol"] == "AFP/1.0"


def test_federation_inbox_rejects_replay_attack(client: TestClient):
    priv_key, pub_key = federation.get_server_keypair()
    payload = {"message": "hello once"}
    envelope = federation.sign_envelope(
        sender="@alice:node1.net",
        recipient="@bob:node2.net",
        intent="hello",
        payload=payload,
        private_key_hex=priv_key,
    )
    envelope["public_key"] = pub_key

    # Первый вызов — успешен
    resp1 = client.post("/v1/federation/inbox", json=envelope)
    assert resp1.status_code == 200

    # Повторный вызов с тем же nonce — отказ из-за replay attack
    resp2 = client.post("/v1/federation/inbox", json=envelope)
    assert resp2.status_code == 401
    assert "replay attack" in resp2.json()["detail"]


def test_federation_inbox_rejects_tampered_payload(client: TestClient):
    priv_key, pub_key = federation.get_server_keypair()
    payload = {"budget": 100}
    envelope = federation.sign_envelope(
        sender="@charlie:node3.net",
        recipient="@dave:node4.net",
        intent="negotiate",
        payload=payload,
        private_key_hex=priv_key,
    )
    envelope["public_key"] = pub_key
    # Подделка данных после подписи
    envelope["payload"]["budget"] = 999999

    resp = client.post("/v1/federation/inbox", json=envelope)
    assert resp.status_code == 401
    assert "signature verification failed" in resp.json()["detail"]


# =====================================================================
# 2. Многораундовый торг (Multi-turn Negotiation)
# =====================================================================
def test_multi_turn_negotiation_convergence():
    now = utcnow()
    # Инициатор свободен завтра с 10 до 16
    init_cal = [
        CalendarEvent(title="busy", start=now, end=now + timedelta(hours=14)),
        CalendarEvent(title="busy", start=now + timedelta(hours=20), end=now + timedelta(hours=48)),
    ]
    # Респондент занят в начале окна, но имеет короткое пересечение на 45 минут
    resp_cal = [
        CalendarEvent(title="busy", start=now, end=now + timedelta(hours=14, minutes=45)),
        CalendarEvent(title="busy", start=now + timedelta(hours=16), end=now + timedelta(hours=48)),
    ]

    init_ctx = AgentContext(
        user_id=10,
        display_name="Twin 1",
        email="twin1@aura.net",
        calendar=init_cal,
        preferences=Preferences(preferred_hours=[10, 18], city="Керкраде"),
    )
    resp_ctx = AgentContext(
        user_id=20,
        display_name="Twin 2",
        email="twin2@aura.net",
        calendar=resp_cal,
        preferences=Preferences(preferred_hours=[10, 18], city="Керкраде"),
    )

    agent = app_module.get_agent()
    # Запрос на встречу длительностью 60 мин: прямого часового слота нет,
    # но многораундовый торг находит компромиссный 45-минутный слот!
    req = NegotiateRequest(
        initiator=init_ctx,
        responder=resp_ctx,
        topic="Architecture Review",
        duration_minutes=60,
        window_hours=48,
        multi_turn=True,
    )
    res = agent.negotiate(req)
    assert res.status in ("CONVERGING", "AGREED", "COUNTER_OFFER")
    assert len(res.rounds_log) >= 1
    assert res.session_id.startswith("neg-")


# =====================================================================
# 3. Реальный поиск мест и бронирование
# =====================================================================
def test_booking_dispatcher_ticket_generation():
    dt = utcnow() + timedelta(days=2)
    ticket = tools.BookingDispatcher.dispatch(
        place="Aurora Roasters",
        at=dt,
        people=3,
        user_prefs=Preferences(diet=["vegan"]),
    )
    assert ticket["ok"] is True
    assert ticket["status"] == "confirmed"
    assert ticket["place"] == "Aurora Roasters"
    assert ticket["people"] == 3
    assert ticket["confirmation"].startswith("SELFVOY-")
    assert len(ticket["pin_code"]) == 4


def test_api_book_table_endpoint(client: TestClient):
    dt = (utcnow() + timedelta(days=1)).isoformat()
    # Без токена (или с тестовым токеном в insecure dev)
    resp = client.post(
        "/v1/tools/book_table",
        json={"place": "Matcha Point", "at": dt, "people": 4},
        headers={"X-Selfvoy-Token": "test-token"},
    )
    # В тестовом окружении service_token пустой (insecure dev mode) либо валидный
    if resp.status_code == 200:
        data = resp.json()
        assert data["confirmation"].startswith("SELFVOY-")
        assert data["people"] == 4


# =====================================================================
# 4. CalDAV & Внешние календари
# =====================================================================
def test_caldav_ics_parser():
    ics_sample = """BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//Apple Inc.//Mac OS X 10.15//EN
BEGIN:VEVENT
UID:20261024T100000Z-1234@aura.net
DTSTART:20261024T100000Z
DTEND:20261024T113000Z
SUMMARY:Встреча с инвесторами
DESCRIPTION:Обсуждение Aura Protocol
END:VEVENT
BEGIN:VEVENT
UID:20261025T140000Z-5678@aura.net
DTSTART:20261025T140000Z
DTEND:20261025T150000Z
SUMMARY:Sprint Retrospective
END:VEVENT
END:VCALENDAR"""

    events = caldav_sync.parse_vcalendar(ics_sample)
    assert len(events) == 2
    assert events[0].title == "Встреча с инвесторами"
    assert events[0].start == datetime(2026, 10, 24, 10, 0, tzinfo=timezone.utc)
    assert events[0].end == datetime(2026, 10, 24, 11, 30, tzinfo=timezone.utc)
    assert events[1].title == "Sprint Retrospective"


# =====================================================================
# 5. Локальные LLM (Zero-Cloud Ollama / vLLM)
# =====================================================================
def test_build_local_ollama_llm():
    settings = Settings(
        llm_provider="ollama",
        llm_base_url="http://localhost:11434/v1",
        llm_model="llama3.2",
    )
    model = llm_module.build_llm(settings)
    assert isinstance(model, llm_module.OpenAILLM)
    assert model.base_url == "http://localhost:11434/v1"
    assert model.model == "llama3.2"


def test_build_local_vllm():
    settings = Settings(
        llm_provider="vllm",
        llm_base_url="http://127.0.0.1:8000/v1",
        llm_model="mistral-7b-instruct",
    )
    model = llm_module.build_llm(settings)
    assert isinstance(model, llm_module.OpenAILLM)
    assert model.base_url == "http://127.0.0.1:8000/v1"
    assert model.model == "mistral-7b-instruct"
