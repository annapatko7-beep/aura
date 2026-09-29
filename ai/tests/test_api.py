"""Сквозные тесты HTTP API (FastAPI + TestClient)."""

from datetime import datetime, timedelta, timezone

import pytest
from fastapi.testclient import TestClient

from aura_ai import app as app_module
from aura_ai.agent import AuraAgent
from aura_ai.config import Settings, reset_settings
from aura_ai.llm import MockLLM
from aura_ai.models import Preferences
from aura_ai.tools import ToolManager

NOW = datetime(2026, 9, 17, 14, 0, tzinfo=timezone.utc)


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setenv("AURA_MEMORY_BACKEND", "memory://")
    monkeypatch.delenv("AURA_AI_TOKEN", raising=False)
    reset_settings()
    settings = Settings()
    settings.memory_backend = "memory://"
    app_module.set_agent(AuraAgent(settings=settings, llm=MockLLM(), tools=ToolManager()))
    with TestClient(app_module.app) as test_client:
        yield test_client
    app_module.set_agent(None)
    reset_settings()


def test_healthz(client):
    response = client.get("/healthz")
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["llm"] == "mock"


def test_tools_listed(client):
    response = client.get("/v1/tools")
    assert response.status_code == 200
    names = {tool["name"] for tool in response.json()["tools"]}
    assert "book_table" in names


def test_agent_run_meeting_end_to_end(client):
    payload = {
        "context": {
            "user_id": 1,
            "email": "anna@example.com",
            "display_name": "Анна",
            "message": "Хочу встретиться с Аней в эти выходные обсудить стартап",
            "now": NOW.isoformat(),
            "preferences": {"diet": ["vegan"], "city": "Керкраде", "lat": 50.861, "lon": 6.064},
        },
        "peers": [
            {
                "user_id": 2,
                "email": "anya@example.com",
                "display_name": "Аня",
                "preferences": {"diet": ["gluten_free"], "city": "Херлен", "lat": 50.888, "lon": 5.978},
            }
        ],
        "execute": True,
    }
    response = client.post("/v1/agent/run", json=payload)
    assert response.status_code == 200
    body = response.json()
    assert body["intent"] == "schedule_meeting"
    assert body["a2a"]["slots"], "A2A должен вернуть общие слоты"
    assert body["a2a"]["place"]["name"]
    assert body["results"]


def test_agent_run_dry_run(client):
    response = client.post(
        "/v1/agent/run",
        json={
            "context": {"message": "напомни завтра позвонить", "now": NOW.isoformat()},
            "dry_run": True,
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert body["intent"] == "reminder"
    assert body["results"] == []


def test_negotiate_endpoint(client):
    response = client.post(
        "/v1/agent/negotiate",
        json={
            "initiator": {"message": "встреча", "now": NOW.isoformat()},
            "responder": {"message": "", "now": NOW.isoformat()},
            "duration_minutes": 45,
            "window_hours": 48,
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert body["accepted"] is True
    assert body["slot"]["score"] > 0


def test_memory_extract_and_read_back(client):
    extracted = client.post(
        "/v1/memory/extract",
        json={"user_key": "anna@example.com", "message": "Я люблю тихие кофейни"},
    )
    assert extracted.status_code == 200
    assert extracted.json()["entries"]

    loaded = client.get("/v1/memory/anna@example.com", params={"query": "кофейни"})
    assert loaded.status_code == 200
    texts = [entry["text"] for entry in loaded.json()["entries"]]
    assert any("кофейни" in text for text in texts)


def test_planner_slots_endpoint(client):
    response = client.post(
        "/v1/planner/slots",
        json={
            "window": {
                "start": NOW.isoformat(),
                "end": (NOW + timedelta(hours=8)).isoformat(),
                "label": "сегодня",
            },
            "preferences": Preferences().model_dump(),
            "duration_minutes": 60,
            "max_slots": 3,
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert len(body["slots"]) == 3


def test_planner_understand_endpoint(client):
    response = client.post(
        "/v1/planner/understand",
        json={"text": "встретимся в пятницу на 30 минут", "now": NOW.isoformat()},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["intent"] == "schedule_meeting"
    assert body["duration_minutes"] == 30
    assert body["window"]["label"] == "пятница"


def test_service_token_enforced(monkeypatch):
    monkeypatch.setenv("AURA_AI_TOKEN", "secret")
    reset_settings()
    app_module.set_agent(AuraAgent(settings=Settings(), llm=MockLLM(), tools=ToolManager()))
    try:
        with TestClient(app_module.app) as test_client:
            assert test_client.get("/v1/tools").status_code == 401
            assert (
                test_client.get("/v1/tools", headers={"X-Aura-Token": "secret"}).status_code == 200
            )
    finally:
        app_module.set_agent(None)
        reset_settings()


def test_invalid_payload_rejected(client):
    response = client.post("/v1/agent/run", json={"context": {"preferences": {"preferred_hours": "не список"}}})
    assert response.status_code == 422
