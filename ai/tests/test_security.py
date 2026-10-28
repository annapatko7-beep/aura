"""Регрессии аудита безопасности AI-сервиса (A3, A5, A7–A10, A14–A16, A18)."""

import asyncio
import base64
import json
import math
import os
import threading
from datetime import datetime, timedelta, timezone

import pytest
from fastapi.testclient import TestClient

from selfvoy_ai import app as app_module
from selfvoy_ai import memory as memory_module
from selfvoy_ai.agent import SelfvoyAgent
from selfvoy_ai.config import Settings, reset_settings
from selfvoy_ai.llm import BaseLLM, MockLLM, normalize_actions, sanitize_payload
from selfvoy_ai.models import AgentContext, AgentRequest, CalendarEvent, MemoryEntry
from selfvoy_ai.speech import BaseSTT
from selfvoy_ai.tools import ToolBackends, ToolError, ToolManager

NOW = datetime(2026, 9, 17, 14, 0, tzinfo=timezone.utc)


def _agent(llm=None, store=None) -> SelfvoyAgent:
    settings = Settings()
    settings.memory_backend = "memory://"
    return SelfvoyAgent(
        settings=settings,
        llm=llm or MockLLM(),
        store=store or memory_module.InMemoryStore(),
        tools=ToolManager(),
    )


@pytest.fixture()
def env(monkeypatch):
    """Чистое окружение: без токена и без режима разработки."""
    monkeypatch.setenv("SELFVOY_MEMORY_BACKEND", "memory://")
    monkeypatch.delenv("SELFVOY_AI_TOKEN", raising=False)
    monkeypatch.delenv("SELFVOY_AI_INSECURE_DEV", raising=False)
    monkeypatch.setenv("SELFVOY_STT_PROVIDER", "mock")
    reset_settings()
    yield monkeypatch
    app_module.set_agent(None)
    app_module.set_stt(None)
    reset_settings()


def _client(agent=None, raise_server_exceptions=True) -> TestClient:
    reset_settings()
    app_module.set_agent(agent or _agent())
    return TestClient(app_module.app, raise_server_exceptions=raise_server_exceptions)


@pytest.fixture()
def dev_client(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client() as client:
        yield client


class StaticLLM(BaseLLM):
    name = "static"

    def __init__(self, payload):
        self.payload = payload

    def complete_json(self, context, understanding, history=None):
        return self.payload


# ------------------------------------------------------------- A3: токен
def test_without_token_service_fails_closed(env):
    with _client() as client:
        assert client.get("/v1/tools").status_code == 503
        assert client.post("/v1/agent/run", json={"context": {"message": "привет"}}).status_code == 503
        assert client.get("/v1/memory/1").status_code == 503
        # healthz остаётся открытым: его пингует C++-сервер и healthcheck compose.
        assert client.get("/healthz").status_code == 200


def test_insecure_dev_flag_opens_service_without_token(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client() as client:
        assert client.get("/v1/tools").status_code == 200


def test_token_required_even_with_insecure_flag_when_token_set(env):
    env.setenv("SELFVOY_AI_TOKEN", "s3cret")
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client() as client:
        assert client.get("/v1/tools").status_code == 401
        assert client.get("/v1/tools", headers={"X-Selfvoy-Token": "s3cret"}).status_code == 200


def test_non_ascii_token_is_401_not_500(env):
    env.setenv("SELFVOY_AI_TOKEN", "s3cret")
    with _client(raise_server_exceptions=False) as client:
        response = client.get("/v1/tools", headers={"X-Selfvoy-Token": "\xe9".encode("latin-1")})
        assert response.status_code == 401


def test_default_bind_host_is_loopback(env):
    env.delenv("SELFVOY_AI_HOST", raising=False)
    assert Settings().host == "127.0.0.1"


# ------------------------------------------------------- A5: /healthz
def test_healthz_does_not_leak_dsn(env):
    env.setenv("SELFVOY_MEMORY_BACKEND", "postgresql://selfvoy:hunter2@db/selfvoy")
    with _client() as client:
        body = client.get("/healthz").json()
    assert body["memory_backend"] == "postgres"
    assert "hunter2" not in json.dumps(body)


# ----------------------------------------------- A15: текст исключений
def test_internal_error_does_not_leak_exception_text(env):
    class ExplodingLLM(BaseLLM):
        name = "boom"

        def complete_json(self, context, understanding, history=None):
            raise RuntimeError("postgresql://selfvoy:hunter2@db/selfvoy")

    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client(_agent(ExplodingLLM()), raise_server_exceptions=False) as client:
        response = client.post("/v1/agent/run", json={"context": {"message": "привет"}})
    assert response.status_code == 500
    assert response.json() == {"detail": "internal error"}


# ------------------------------------------- A8: исполнение инструментов
def test_agent_does_not_execute_tools_by_default():
    agent = _agent()
    response = agent.run(AgentRequest(context=AgentContext(message="напиши письмо Ане про проект", email="a@x.io")))
    assert response.actions, "план действий возвращается серверу"
    assert response.results == []
    assert AgentRequest().execute is False


def test_server_only_tools_never_executed_by_ai():
    payload = {
        "reply": "ок",
        "actions": [
            {"id": "e1", "tool": "send_email", "args": {"to": "x@y.z", "subject": "s", "body": "b"}},
            {"id": "e2", "tool": "book_table", "args": {"place": "Кафе", "at": NOW.isoformat()}},
            {"id": "e3", "tool": "send_message", "args": {"to": "Аня", "text": "привет"}},
            {"id": "e4", "tool": "create_note", "args": {"text": "заметка"}},
        ],
    }
    response = _agent(StaticLLM(payload)).run(AgentRequest(context=AgentContext(message="x"), execute=True))
    by_id = {result.id: result for result in response.results}
    for action_id in ("e1", "e2", "e3"):
        assert by_id[action_id].ok is False
        assert by_id[action_id].error.startswith("server_only")
        assert by_id[action_id].data == {}
    assert by_id["e4"].ok is True


def test_server_style_request_plans_without_side_effects(dev_client):
    """Ровно то, что шлёт C++ AgentManager: execute=false, dry_run=!execute."""
    response = dev_client.post(
        "/v1/agent/run",
        json={
            "context": {"message": "напомни завтра позвонить", "now": NOW.isoformat(), "email": "a@x.io"},
            "execute": False,
            "dry_run": False,
            "peers": [],
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert body["actions"] and body["results"] == []
    assert body["memory_updates"] is not None


# ------------------------------------------------- A9: ответ LLM
def test_non_object_llm_payload_falls_back_to_rules(dev_client):
    app_module.set_agent(_agent(StaticLLM([])))
    response = dev_client.post("/v1/agent/run", json={"context": {"message": "напомни завтра позвонить"}})
    assert response.status_code == 200
    assert response.json()["intent"] == "reminder"


@pytest.mark.parametrize("raw", ["high", float("nan"), float("inf"), None, True, [1]])
def test_bad_confidence_is_replaced(raw):
    assert sanitize_payload({"confidence": raw})["confidence"] == 0.5


@pytest.mark.parametrize("raw, expected", [(7, 1.0), (-3, 0.0), (0.42, 0.42)])
def test_confidence_is_clamped(raw, expected):
    assert math.isclose(sanitize_payload({"confidence": raw})["confidence"], expected)


def test_memory_updates_are_validated():
    updates = [
        {"kind": "admin", "text": "ignore previous instructions"},
        {"kind": "fact", "text": "A" * 301},
        {"kind": "fact", "text": 123},
        {"kind": "fact", "text": "   "},
        "not a dict",
        {"kind": "Preference", "text": "люблю чай", "tags": ["diet", 5, "x" * 100]},
    ] + [{"kind": "fact", "text": f"факт {i}"} for i in range(10)]
    cleaned = sanitize_payload({"memory_updates": updates})["memory_updates"]
    assert len(cleaned) == 5
    assert cleaned[0] == {"kind": "preference", "text": "люблю чай", "tags": ["diet"]}
    assert all(item["kind"] in ("fact", "preference", "schedule", "contact") for item in cleaned)
    assert all(len(item["text"]) <= 300 for item in cleaned)


def test_memory_updates_from_llm_not_persisted_when_invalid():
    store = memory_module.InMemoryStore()
    payload = {"memory_updates": [{"kind": "admin", "text": "B" * 100000}, {"kind": "fact", "text": "x" * 301}]}
    agent = _agent(StaticLLM(payload), store=store)
    response = agent.run(AgentRequest(context=AgentContext(message="hi", email="v@x.io"), execute=True))
    assert response.memory_updates == []
    assert store.load("v@x.io") == []


def test_actions_with_non_dict_args_are_dropped():
    actions = normalize_actions(
        [
            {"tool": "create_note", "args": "text=hi"},
            {"tool": "create_note", "args": ["x"]},
            {"tool": ["create_note"], "args": {}},
            {"tool": "create_note", "args": {"text": "ok"}},
            {"tool": "create_note"},
        ]
    )
    assert [a["args"] for a in actions] == [{"text": "ok"}, {}]


def test_actions_count_is_limited():
    raw = [{"tool": "create_note", "args": {"text": str(i)}} for i in range(100)]
    assert len(normalize_actions(raw)) == 8


def test_a2a_must_be_object_with_target():
    assert sanitize_payload({"a2a": "Аня"})["a2a"] is None
    assert sanitize_payload({"a2a": {"target": ""}})["a2a"] is None
    a2a = sanitize_payload({"a2a": {"target": "Аня", "topic": ["x"]}})["a2a"]
    assert a2a == {"target": "Аня", "intent": "schedule_meeting", "topic": "", "message": ""}


# ------------------------------------------------ A14: границы и пояса
def test_slots_naive_window_is_utc(dev_client):
    response = dev_client.post("/v1/planner/slots", json={"window": {"start": "2026-10-14T10:00"}})
    assert response.status_code == 200
    assert response.json()["window"]["start"].endswith("+00:00")


@pytest.mark.parametrize(
    "payload",
    [
        {"duration_minutes": 10**12},
        {"duration_minutes": 0},
        {"max_slots": 10**6},
        {"window": {"start": "2026-10-14T10:00:00Z", "end": "2026-10-13T10:00:00Z"}},
        {"window": {"start": "2026-10-14T10:00:00Z", "end": "2030-10-14T10:00:00Z"}},
        {"window": {"start": "garbage"}},
        {"window": {"start": "9999-12-31T23:00:00"}},
        {"window": "сегодня"},
    ],
)
def test_slots_bad_input_is_4xx(env, payload):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client(raise_server_exceptions=False) as client:
        response = client.post("/v1/planner/slots", json=payload)
    assert response.status_code == 422, response.text


@pytest.mark.parametrize("payload", [{"window_hours": 10**12}, {"window_hours": 0}, {"duration_minutes": 10**9}])
def test_negotiate_bounds(env, payload):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client(raise_server_exceptions=False) as client:
        body = {"initiator": {}, "responder": {}, **payload}
        assert client.post("/v1/agent/negotiate", json=body).status_code == 422


def test_naive_times_in_context_are_normalized():
    event = CalendarEvent(start=datetime(2026, 10, 14, 10), end=datetime(2026, 10, 14, 11))
    assert event.start.tzinfo is not None
    context = AgentContext(now="2026-10-14T09:00", timezone="../../etc/passwd")
    assert context.now.tzinfo is not None
    assert context.timezone == "UTC"


def test_calendar_event_with_end_before_start_is_skipped():
    context = AgentContext(
        calendar=[
            {"start": "2026-10-14T11:00:00Z", "end": "2026-10-14T10:00:00Z"},
            {"start": "2026-10-14T12:00:00Z", "end": "2026-10-14T13:00:00Z"},
        ]
    )
    assert len(context.calendar) == 1


def test_naive_calendar_does_not_break_tools(dev_client):
    response = dev_client.post(
        "/v1/agent/run",
        json={
            "context": {
                "message": "давай встретимся завтра",
                "now": NOW.isoformat(),
                "calendar": [{"start": "2026-09-18T10:00", "end": "2026-09-18T11:00"}],
            },
            "execute": True,
        },
    )
    assert response.status_code == 200
    results = {r["tool"]: r for r in response.json()["results"]}
    assert results["check_calendar"]["ok"] is True


@pytest.mark.parametrize(
    "contact",
    [
        {"name": "Аня", "calendar": [{"start": "x"}]},
        {"name": "Аня", "lat": "abc", "lon": {"x": 1}},
        {"name": "Аня", "diet": "vegan", "calendar": "busy"},
    ],
)
def test_garbage_contacts_do_not_500(dev_client, contact):
    response = dev_client.post(
        "/v1/agent/run",
        json={"context": {"message": "давай встретимся с Аней завтра", "contacts": [contact]}},
    )
    assert response.status_code == 200


def test_internal_validation_error_is_422_without_details(env):
    class BadSlotLLM(BaseLLM):
        name = "bad"

        def complete_json(self, context, understanding, history=None):
            from selfvoy_ai.models import Slot

            Slot(start="not a date", end="nope")  # ValidationError внутри обработчика
            return {}

    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client(_agent(BadSlotLLM()), raise_server_exceptions=False) as client:
        response = client.post("/v1/agent/run", json={"context": {"message": "hi"}})
    assert response.status_code == 422
    assert response.json() == {"detail": "invalid input"}


# ------------------------------------------------------ A10: речь
def test_speech_rejects_non_object_json(dev_client):
    assert dev_client.post("/v1/speech/transcribe", json=[1]).status_code == 400
    assert dev_client.post("/v1/speech/transcribe", json={"audio": 123}).status_code == 400


def test_speech_rejects_non_file_field(dev_client):
    response = dev_client.post("/v1/speech/transcribe", files={"file": (None, "abc")})
    assert response.status_code == 400


def test_speech_declared_length_checked_before_reading(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setenv("SELFVOY_STT_MAX_BYTES", "16")
    with _client() as client:
        response = client.post(
            "/v1/speech/transcribe",
            content=b'{"audio": "' + b"A" * 200 + b'"}',
            headers={"Content-Type": "application/json"},
        )
    assert response.status_code == 413


def test_speech_streamed_body_is_cut_at_limit(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setenv("SELFVOY_STT_MAX_BYTES", "16")

    def chunks():
        for _ in range(100):
            yield b"A" * 1024

    with _client() as client:
        response = client.post(
            "/v1/speech/transcribe", content=chunks(), headers={"Content-Type": "application/json"}
        )
    assert response.status_code == 413


def test_speech_backend_runs_off_the_event_loop(dev_client):
    seen = {}

    class ProbeSTT(BaseSTT):
        name = "probe"

        def transcribe(self, audio, language="", filename="audio.webm"):
            try:
                asyncio.get_running_loop()
                seen["in_loop"] = True
            except RuntimeError:
                seen["in_loop"] = False
            seen["thread"] = threading.current_thread().name
            return {"text": "ok", "language": "ru"}

    app_module.set_stt(ProbeSTT())
    response = dev_client.post(
        "/v1/speech/transcribe", json={"audio": base64.b64encode(b"audio-bytes").decode()}
    )
    assert response.status_code == 200
    assert seen["in_loop"] is False, "синхронный STT не должен блокировать event loop"


# ----------------------------------------------- A7/A16: хранилища
def test_build_store_variants(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    assert isinstance(memory_module.build_store("memory://"), memory_module.InMemoryStore)
    file_store = memory_module.build_store("file")
    assert isinstance(file_store, memory_module.FileMemoryStore)
    assert file_store.path == memory_module.DEFAULT_FILE_PATH
    custom = memory_module.build_store(f"file://{tmp_path}/m.json")
    assert custom.path == f"{tmp_path}/m.json"
    pg = memory_module.build_store("postgres", "postgresql://u:p@db/selfvoy")
    assert isinstance(pg, memory_module.PostgresMemoryStore) and pg.dsn == "postgresql://u:p@db/selfvoy"
    # «postgres» без DSN — не пытаемся подключиться к строке «postgres».
    assert not isinstance(memory_module.build_store("postgres", ""), memory_module.PostgresMemoryStore)
    direct = memory_module.build_store("postgresql://u:p@db/selfvoy")
    assert direct.dsn == "postgresql://u:p@db/selfvoy"


def test_agent_uses_database_url_for_postgres_backend():
    settings = Settings()
    settings.memory_backend = "postgres"
    settings.database_url = "postgresql://u:p@db/selfvoy"
    agent = SelfvoyAgent(settings=settings, llm=MockLLM(), tools=ToolManager())
    assert isinstance(agent.store, memory_module.PostgresMemoryStore)
    assert agent.store.dsn == "postgresql://u:p@db/selfvoy"


class _FakeCursor:
    def __init__(self, log, rows):
        self.log = log
        self.rows = rows

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def execute(self, sql, params):
        self.log.append((" ".join(sql.split()), params))

    def fetchall(self):
        return self.rows


class _FakeConn:
    def __init__(self, log, rows):
        self.log = log
        self.rows = rows

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def cursor(self):
        return _FakeCursor(self.log, self.rows)

    def commit(self):
        self.log.append(("COMMIT", None))


def test_postgres_store_uses_numeric_id_and_text_array(monkeypatch):
    log = []
    store = memory_module.PostgresMemoryStore("postgresql://u:p@db/selfvoy")
    monkeypatch.setattr(store, "_connect", lambda: _FakeConn(log, []))
    store.save(
        "42",
        [
            MemoryEntry(kind="fact", text="люблю чай", tags=["diet", "drink"]),
            MemoryEntry(kind="admin", text="не должно попасть в БД"),
        ],
    )
    inserts = [entry for entry in log if entry[0].startswith("INSERT")]
    assert len(inserts) == 1, "запись с недопустимым kind не уходит в БД (CHECK)"
    sql, params = inserts[0]
    assert "WHERE id = %s" in sql
    assert params[-1] == 42
    assert params[3] == ["diet", "drink"], "TEXT[] — список, а не JSON-строка"
    select_sql, select_params = [entry for entry in log if entry[0].startswith("SELECT")][0]
    assert "WHERE id = %s" in select_sql and select_params == (42,)


def test_postgres_store_email_key(monkeypatch):
    log = []
    store = memory_module.PostgresMemoryStore("postgresql://u:p@db/selfvoy")
    rows = [("fact", "люблю чай", 1.0, ["diet"], NOW)]
    monkeypatch.setattr(store, "_connect", lambda: _FakeConn(log, rows))
    entries = store.load("Anna@Example.com")
    assert entries[0].tags == ["diet"]
    sql, params = log[0]
    assert "lower(email) = lower(%s)" in sql and params == ("Anna@Example.com",)


def test_file_store_writes_atomically_and_concurrently(tmp_path):
    path = tmp_path / "memory.json"
    store = memory_module.FileMemoryStore(str(path))

    def worker(n):
        for i in range(20):
            store.save(f"user{n}", [MemoryEntry(text=f"факт {n}-{i}")])

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    data = json.loads(path.read_text(encoding="utf-8"))
    assert {key: len(value) for key, value in data.items()} == {f"user{n}": 20 for n in range(4)}
    leftovers = [name for name in os.listdir(tmp_path) if name.endswith(".tmp")]
    assert leftovers == []
    reloaded = memory_module.FileMemoryStore(str(path))
    assert len(reloaded.load("user0")) == 20


def test_file_store_survives_corrupted_entries(tmp_path):
    path = tmp_path / "memory.json"
    path.write_text(json.dumps({"u": [{"no_text": True}]}), encoding="utf-8")
    store = memory_module.FileMemoryStore(str(path))
    assert store.load("u") == []


# -------------------------------------------------- A18: send_email
def test_send_email_posts_plain_payload(monkeypatch):
    import httpx

    captured = {}

    class FakeResponse:
        def raise_for_status(self):
            return None

        def json(self):
            return {"ok": True}

    def fake_post(url, json=None, timeout=None):
        captured["json"] = json
        return FakeResponse()

    monkeypatch.setattr(httpx, "post", fake_post)
    manager = ToolManager(ToolBackends(mode="http", urls={"email": "http://mail.local/send"}))
    manager.run("send_email", {"to": "a@b.c", "subject": "s", "body": "b"}, AgentContext())
    assert "**payload" not in captured["json"]
    assert set(captured["json"]) == {"to", "subject", "body", "sent_at"}


def test_tool_with_wrong_arg_types_is_tool_error():
    manager = ToolManager()
    with pytest.raises(ToolError):
        manager.run("create_note", {"text": 123}, AgentContext())
    with pytest.raises(ToolError):
        manager.run("suggest_time", {"duration_minutes": "много"}, AgentContext())
    with pytest.raises(ToolError):
        manager.run("check_calendar", {"window": "завтра"}, AgentContext())
    with pytest.raises(ToolError):
        manager.run("create_note", "text", AgentContext())


def test_suggest_time_clamps_huge_values():
    result = ToolManager().run(
        "suggest_time",
        {"duration_minutes": 10**9, "limit": 10**9, "window": {"start": NOW.isoformat(), "end": (NOW + timedelta(days=3650)).isoformat()}},
        AgentContext(),
    )
    assert result["duration_minutes"] == 24 * 60
    assert len(result["slots"]) <= 20


# ------------------------------------------- аудит: режим разработки и /docs
@pytest.mark.parametrize("host", ["0.0.0.0", "192.168.1.10", "::"])
def test_insecure_dev_refused_on_non_loopback_host(env, host):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setenv("SELFVOY_AI_HOST", host)
    with _client() as client:
        assert client.get("/v1/tools").status_code == 503
        assert client.post("/v1/agent/run", json={"context": {"message": "привет"}}).status_code == 503


@pytest.mark.parametrize("host", ["127.0.0.1", "::1", "localhost"])
def test_insecure_dev_allowed_on_loopback_host(env, host):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setenv("SELFVOY_AI_HOST", host)
    with _client() as client:
        assert client.get("/v1/tools").status_code == 200


def test_insecure_dev_refused_when_uvicorn_binds_all_interfaces(env):
    # uvicorn selfvoy_ai.app:app --host 0.0.0.0 — SELFVOY_AI_HOST по умолчанию 127.0.0.1.
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setattr("sys.argv", ["uvicorn", "selfvoy_ai.app:app", "--host", "0.0.0.0", "--port", "8000"])
    with _client() as client:
        assert client.get("/v1/tools").status_code == 503
    env.setattr("sys.argv", ["uvicorn", "selfvoy_ai.app:app", "--host=0.0.0.0"])
    assert not app_module.insecure_dev_allowed(Settings())
    env.setattr("sys.argv", ["uvicorn", "selfvoy_ai.app:app"])
    env.setenv("UVICORN_HOST", "0.0.0.0")
    assert not app_module.insecure_dev_allowed(Settings())


def test_api_docs_disabled_outside_dev(env):
    assert app_module.docs_options(Settings()) == {"docs_url": None, "redoc_url": None, "openapi_url": None}
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    env.setenv("SELFVOY_AI_HOST", "0.0.0.0")
    assert app_module.docs_options(Settings())["openapi_url"] is None
    env.setenv("SELFVOY_AI_HOST", "127.0.0.1")
    assert app_module.docs_options(Settings())["openapi_url"] == "/openapi.json"


def test_api_docs_not_served_without_dev_flag():
    if app_module.app.openapi_url is not None:
        pytest.skip("тесты запущены с SELFVOY_AI_INSECURE_DEV=1 — /docs включены намеренно")
    with TestClient(app_module.app) as client:
        assert client.get("/docs").status_code == 404
        assert client.get("/redoc").status_code == 404
        assert client.get("/openapi.json").status_code == 404


# ------------------------------------------- аудит: огромные числа и время
def test_agent_run_huge_relative_time_is_not_500(dev_client):
    for message in (
        "напомни через 99999999 недель позвонить",
        "встреча через " + "9" * 4400 + " дней на " + "9" * 3000 + " минут",
    ):
        response = dev_client.post(
            "/v1/agent/run", json={"context": {"message": message, "now": NOW.isoformat()}}
        )
        assert response.status_code == 200, response.text


def test_agent_run_time_out_of_range_is_422(env):
    env.setenv("SELFVOY_AI_INSECURE_DEV", "1")
    with _client(raise_server_exceptions=False) as client:
        response = client.post(
            "/v1/agent/run",
            json={"context": {"message": "встреча завтра в 10", "now": "9999-12-31T22:00:00Z"}},
        )
    assert response.status_code == 422
    assert response.json() == {"detail": "time is out of range"}


# ------------------------------------------- аудит: пределы размера запроса
def _events(count: int):
    return [
        {"start": (NOW + timedelta(hours=i)).isoformat(), "end": (NOW + timedelta(hours=i, minutes=30)).isoformat()}
        for i in range(count)
    ]


def test_calendar_limit_enforced(dev_client):
    from selfvoy_ai.models import MAX_CALENDAR_EVENTS

    ok = dev_client.post("/v1/agent/slots", json={"context": {"calendar": _events(MAX_CALENDAR_EVENTS)}})
    assert ok.status_code == 200, ok.text
    for path, body in (
        ("/v1/agent/slots", {"context": {"calendar": _events(MAX_CALENDAR_EVENTS + 1)}}),
        ("/v1/agent/run", {"context": {"message": "привет", "calendar": _events(MAX_CALENDAR_EVENTS + 1)}}),
        ("/v1/planner/slots", {"calendar": _events(MAX_CALENDAR_EVENTS + 1)}),
    ):
        assert dev_client.post(path, json=body).status_code == 422, path


def test_context_list_and_message_limits_enforced(dev_client):
    from selfvoy_ai.models import MAX_HISTORY_ITEMS, MAX_MEMORY_ENTRIES, MAX_MESSAGE_CHARS, MAX_PEERS

    def run(context, **extra):
        return dev_client.post("/v1/agent/run", json={"context": context, **extra}).status_code

    assert run({"message": "x" * MAX_MESSAGE_CHARS}) == 200
    assert run({"message": "x" * (MAX_MESSAGE_CHARS + 1)}) == 422
    assert run({"message": "привет", "history": [{"body": "a"}] * MAX_HISTORY_ITEMS}) == 200
    assert run({"message": "привет", "history": [{"body": "a"}] * (MAX_HISTORY_ITEMS + 1)}) == 422
    assert run({"message": "привет", "memory": [{"text": "факт"}] * (MAX_MEMORY_ENTRIES + 1)}) == 422
    assert run({"message": "привет"}, peers=[{}] * (MAX_PEERS + 1)) == 422
    response = dev_client.post("/v1/memory/extract", json={"message": "x" * (MAX_MESSAGE_CHARS + 1)})
    assert response.status_code == 422
