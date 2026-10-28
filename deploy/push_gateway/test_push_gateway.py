"""Тесты для микросервиса Push Gateway."""

from fastapi.testclient import TestClient
from app import app

client = TestClient(app)


def test_healthz():
    resp = client.get("/healthz")
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"
    assert data["service"] == "push-gateway"
    assert "dev_mode" in data


def test_push_dev_mode_simulation():
    payload = {
        "platform": "apns",
        "token": "mock-device-token-12345",
        "notification_id": 42,
        "apns": {
            "url": "https://api.push.apple.com/3/device/mock-device-token-12345",
            "headers": {"apns-topic": "app.selfvoy"},
            "payload": {"aps": {"alert": {"title": "Встреча", "body": "Аура договорилась"}}},
        },
    }
    resp = client.post("/push", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert data["ok"] is True
    assert data["notification_id"] == 42
    assert data["provider_status"] == 200


def test_push_fcm_simulation():
    payload = {
        "platform": "fcm",
        "token": "dev-fcm-token-xyz",
        "notification_id": 99,
        "fcm": {
            "message": {
                "token": "dev-fcm-token-xyz",
                "notification": {"title": "Aura", "body": "Привет"},
            }
        },
    }
    resp = client.post("/push", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert data["ok"] is True
    assert data["notification_id"] == 99


def test_push_unknown_platform():
    payload = {
        "platform": "windows_phone_legacy",
        "token": "real-token",
        "notification_id": 1,
    }
    resp = client.post("/push", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert data["ok"] is False
    assert "unknown platform" in data["error"]
