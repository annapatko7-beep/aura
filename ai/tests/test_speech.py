"""Тесты распознавания речи: /v1/speech/transcribe (mock + Whisper-провайдер)."""

import base64

import pytest
from fastapi.testclient import TestClient

from aura_ai import app as app_module
from aura_ai import speech
from aura_ai.agent import AuraAgent
from aura_ai.config import Settings, reset_settings
from aura_ai.llm import MockLLM
from aura_ai.speech import MockSTT, WhisperSTT, build_stt, normalize_language
from aura_ai.tools import ToolManager

# Небольшие «аудио»-байты (содержимое не важно — mock не распознаёт по-настоящему).
AUDIO = b"\x1aE\xdf\xa3fake-webm-bytes" * 8


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setenv("AURA_MEMORY_BACKEND", "memory://")
    monkeypatch.delenv("AURA_AI_TOKEN", raising=False)
    monkeypatch.delenv("AURA_STT_URL", raising=False)
    monkeypatch.delenv("AURA_STT_API_KEY", raising=False)
    monkeypatch.setenv("AURA_STT_PROVIDER", "mock")
    reset_settings()
    settings = Settings()
    settings.memory_backend = "memory://"
    app_module.set_agent(AuraAgent(settings=settings, llm=MockLLM(), tools=ToolManager()))
    app_module.set_stt(MockSTT())
    with TestClient(app_module.app) as test_client:
        yield test_client
    app_module.set_agent(None)
    app_module.set_stt(None)
    reset_settings()


# ------------------------------------------------------------------- normalize
def test_normalize_language():
    assert normalize_language("auto") == ""
    assert normalize_language("") == ""
    assert normalize_language("detect") == ""
    assert normalize_language("RU") == "ru"
    assert normalize_language("ru-RU") == "ru"
    assert normalize_language("en_US") == "en"


# ------------------------------------------------------------------- фабрика
def test_build_stt_mock_without_backend():
    settings = Settings()
    settings.stt_provider = "auto"
    settings.stt_url = ""
    settings.stt_api_key = ""
    assert isinstance(build_stt(settings), MockSTT)


def test_build_stt_whisper_with_url():
    settings = Settings()
    settings.stt_provider = "auto"
    settings.stt_url = "http://whisper.local:9000"
    stt = build_stt(settings)
    assert isinstance(stt, WhisperSTT)
    # Базовый URL → стандартный путь Whisper.
    assert stt.url == "http://whisper.local:9000/v1/audio/transcriptions"


def test_build_stt_openai_default_url():
    settings = Settings()
    settings.stt_provider = "openai"
    settings.stt_url = ""
    stt = build_stt(settings)
    assert isinstance(stt, WhisperSTT)
    assert stt.url == "https://api.openai.com/v1/audio/transcriptions"


def test_transcribe_url_full_path_kept():
    settings = Settings()
    settings.stt_url = "http://x/v1/audio/transcriptions"
    assert settings.stt_transcribe_url == "http://x/v1/audio/transcriptions"


# ------------------------------------------------------------------- эндпоинт
def test_healthz_shows_stt(client):
    body = client.get("/healthz").json()
    assert body["stt"] == "mock"


def test_transcribe_json_base64(client):
    payload = {"audio": base64.b64encode(AUDIO).decode(), "language": "ru"}
    response = client.post("/v1/speech/transcribe", json=payload)
    assert response.status_code == 200
    body = response.json()
    assert body["provider"] == "mock"
    assert body["language"] == "ru"
    assert body["text"]  # непустая (детерминированная) расшифровка


def test_transcribe_multipart_whisper_compatible(client):
    files = {"file": ("audio.webm", AUDIO, "application/octet-stream")}
    response = client.post("/v1/speech/transcribe", files=files, data={"language": "en"})
    assert response.status_code == 200
    body = response.json()
    assert body["language"] == "en"
    assert body["text"]


def test_transcribe_empty_audio_rejected(client):
    response = client.post("/v1/speech/transcribe", json={"audio": ""})
    assert response.status_code == 400


def test_transcribe_invalid_base64_rejected(client):
    response = client.post("/v1/speech/transcribe", json={"audio": "!!не base64!!"})
    assert response.status_code == 400


def test_transcribe_too_large_rejected(client, monkeypatch):
    monkeypatch.setenv("AURA_STT_MAX_BYTES", "16")
    reset_settings()
    app_module.set_stt(MockSTT())
    big = base64.b64encode(b"x" * 64).decode()
    try:
        response = client.post("/v1/speech/transcribe", json={"audio": big})
        assert response.status_code == 413
    finally:
        reset_settings()
        app_module.set_stt(MockSTT())


def test_transcribe_token_enforced(monkeypatch):
    monkeypatch.setenv("AURA_AI_TOKEN", "secret")
    monkeypatch.setenv("AURA_STT_PROVIDER", "mock")
    reset_settings()
    app_module.set_agent(AuraAgent(settings=Settings(), llm=MockLLM(), tools=ToolManager()))
    app_module.set_stt(MockSTT())
    try:
        with TestClient(app_module.app) as test_client:
            payload = {"audio": base64.b64encode(AUDIO).decode()}
            assert test_client.post("/v1/speech/transcribe", json=payload).status_code == 401
            ok = test_client.post(
                "/v1/speech/transcribe", json=payload, headers={"X-Aura-Token": "secret"}
            )
            assert ok.status_code == 200
    finally:
        app_module.set_agent(None)
        app_module.set_stt(None)
        reset_settings()


# ------------------------------------------------------- WhisperSTT (форвард)
def test_whisper_stt_forwards_and_parses(monkeypatch):
    """WhisperSTT шлёт multipart на бэкенд и разбирает JSON-ответ."""
    captured = {}

    class FakeResponse:
        def raise_for_status(self):
            return None

        def json(self):
            return {"text": "  привет мир  ", "language": "ru"}

    def fake_post(url, files=None, data=None, headers=None, timeout=None):
        captured["url"] = url
        captured["files"] = files
        captured["data"] = data
        captured["headers"] = headers
        return FakeResponse()

    monkeypatch.setattr(speech.httpx if hasattr(speech, "httpx") else _import_httpx(), "post", fake_post)
    stt = WhisperSTT(url="http://whisper.local/v1/audio/transcriptions", api_key="k", model="whisper-1")
    result = stt.transcribe(AUDIO, "ru", "a.webm")
    assert result["text"] == "привет мир"  # пробелы обрезаны
    assert result["language"] == "ru"
    assert captured["url"] == "http://whisper.local/v1/audio/transcriptions"
    assert captured["data"]["language"] == "ru"
    assert captured["data"]["model"] == "whisper-1"
    assert captured["headers"]["Authorization"] == "Bearer k"
    assert "file" in captured["files"]


def test_whisper_stt_error_raises(monkeypatch):
    def boom(*args, **kwargs):
        raise RuntimeError("connection refused")

    monkeypatch.setattr(_import_httpx(), "post", boom)
    stt = WhisperSTT(url="http://whisper.local/v1/audio/transcriptions")
    with pytest.raises(speech.STTError):
        stt.transcribe(AUDIO, "auto")


def _import_httpx():
    import httpx

    return httpx
