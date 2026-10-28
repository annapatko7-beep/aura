"""Серия встреч страницы записи: planner.weekly_series и POST /v1/agent/series."""

from datetime import datetime, timedelta, timezone

import pytest
from fastapi.testclient import TestClient

from selfvoy_ai import app as app_module
from selfvoy_ai import planner
from selfvoy_ai.config import reset_settings


def _utc(*args) -> datetime:
    return datetime(*args, tzinfo=timezone.utc)


def test_series_keeps_local_time_across_dst():
    # Берлин: 10:00 CEST (UTC+2) 20 октября 2026, переход на зимнее время 25 октября.
    occurrences = planner.weekly_series(_utc(2026, 10, 20, 8, 0), "Europe/Berlin", 60, 3)
    assert [o.start for o in occurrences] == [_utc(2026, 10, 20, 8, 0), _utc(2026, 10, 27, 9, 0), _utc(2026, 11, 3, 9, 0)]
    assert [o.local for o in occurrences] == ["2026-10-20T10:00", "2026-10-27T10:00", "2026-11-03T10:00"]
    assert all(o.end - o.start == timedelta(minutes=60) for o in occurrences)
    assert [o.day for o in occurrences] == ["2026-10-20", "2026-10-27", "2026-11-03"]


def test_series_without_dst_is_exact_weeks():
    occurrences = planner.weekly_series(_utc(2026, 10, 20, 7, 0), "Europe/Moscow", 30, 4)
    gaps = {(b.start - a.start) for a, b in zip(occurrences, occurrences[1:])}
    assert gaps == {timedelta(days=7)}
    assert occurrences[0].local == "2026-10-20T10:00"


def test_series_spring_gap_moves_forward():
    # Нью-Йорк: 8 марта 2026 в 02:30 местного времени не существует.
    occurrences = planner.weekly_series(_utc(2026, 3, 1, 7, 30), "America/New_York", 60, 2)
    assert occurrences[0].local == "2026-03-01T02:30"
    assert occurrences[1].local == "2026-03-08T03:30"


def test_series_unknown_zone_is_utc():
    occurrences = planner.weekly_series(_utc(2026, 10, 20, 7, 0), "Нигде/Никак", 60, 2)
    assert occurrences[1].start == _utc(2026, 10, 27, 7, 0)


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setenv("SELFVOY_MEMORY_BACKEND", "memory://")
    monkeypatch.delenv("SELFVOY_AI_TOKEN", raising=False)
    monkeypatch.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    reset_settings()
    with TestClient(app_module.app, raise_server_exceptions=False) as test_client:
        yield test_client
    reset_settings()


def test_series_endpoint(client):
    response = client.post("/v1/agent/series", json={
        "timezone": "Europe/Berlin", "start": "2026-10-20T08:00:00Z", "duration_minutes": 60, "count": 2,
    })
    assert response.status_code == 200, response.text
    body = response.json()["occurrences"]
    assert [o["local"] for o in body] == ["2026-10-20T10:00", "2026-10-27T10:00"]
    assert body[1]["start"].startswith("2026-10-27T09:00:00")


@pytest.mark.parametrize("extra", [{"count": 0}, {"count": 13}, {"duration_minutes": 10}, {"start": "завтра"}])
def test_series_endpoint_bad_input(client, extra):
    body = {"timezone": "UTC", "start": "2026-10-20T08:00:00Z", "duration_minutes": 60, "count": 2, **extra}
    assert client.post("/v1/agent/series", json=body).status_code == 422


def test_series_first_occurrence_is_exact_start_on_dst_fold():
    # Нью-Йорк, 1 ноября 2026: 01:30 бывает дважды; 06:30Z — второе (EST, fold=1).
    start = _utc(2026, 11, 1, 6, 30)
    occurrences = planner.weekly_series(start, "America/New_York", 60, 2)
    assert occurrences[0].start == start
    assert occurrences[0].end == start + timedelta(minutes=60)
    assert occurrences[0].local == "2026-11-01T01:30"
    # Следующая неделя — то же местное время 01:30 (уже EST).
    assert occurrences[1].start == _utc(2026, 11, 8, 6, 30)
    assert occurrences[1].local == "2026-11-08T01:30"


def test_series_first_fold_occurrence_endpoint(client):
    response = client.post("/v1/agent/series", json={
        "timezone": "America/New_York", "start": "2026-11-01T06:30:00Z", "duration_minutes": 60, "count": 2,
    })
    assert response.status_code == 200, response.text
    first = response.json()["occurrences"][0]
    assert datetime.fromisoformat(first["start"].replace("Z", "+00:00")) == _utc(2026, 11, 1, 6, 30)
