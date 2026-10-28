"""Переходный период переименования: SELFVOY_* с запасным чтением старых AURA_*."""

import logging

from fastapi.testclient import TestClient

from selfvoy_ai import app as app_module
from selfvoy_ai import config as config_module
from selfvoy_ai.config import Settings, get_settings, reset_settings


def test_new_variable_is_read(monkeypatch):
    monkeypatch.setenv("SELFVOY_LLM_MODEL", "new-model")
    assert Settings().llm_model == "new-model"
    assert config_module.legacy_env_names() == []


def test_legacy_variable_used_when_new_is_unset(monkeypatch):
    monkeypatch.delenv("SELFVOY_LLM_MODEL", raising=False)
    monkeypatch.setenv("AURA_LLM_MODEL", "old-model")
    monkeypatch.setenv("AURA_AI_PORT", "8123")
    monkeypatch.setenv("AURA_AI_INSECURE_DEV", "1")
    settings = Settings()
    assert settings.llm_model == "old-model"
    assert settings.port == 8123
    assert settings.insecure_dev is True
    assert config_module.legacy_env_names() == ["AURA_AI_INSECURE_DEV", "AURA_AI_PORT", "AURA_LLM_MODEL"]


def test_legacy_variable_empty_new_falls_back(monkeypatch):
    monkeypatch.setenv("SELFVOY_AI_TOKEN", "")
    monkeypatch.setenv("AURA_AI_TOKEN", "legacy-secret")
    assert Settings().service_token == "legacy-secret"


def test_new_variable_wins_over_legacy(monkeypatch):
    monkeypatch.setenv("SELFVOY_LLM_MODEL", "new-model")
    monkeypatch.setenv("AURA_LLM_MODEL", "old-model")
    assert Settings().llm_model == "new-model"
    assert config_module.legacy_env_names() == []


def test_openai_variables_have_no_prefix_fallback(monkeypatch):
    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
    monkeypatch.setenv("AURA_OPENAI_API_KEY", "nope")
    assert Settings().llm_api_key == ""


def test_deprecation_warning_logged_once(monkeypatch, caplog):
    monkeypatch.setenv("AURA_LLM_MODEL", "old-model")
    reset_settings()
    try:
        with caplog.at_level(logging.WARNING, logger="selfvoy.ai.config"):
            assert get_settings().llm_model == "old-model"
            reset_settings()
            get_settings()  # повторное чтение настроек не дублирует предупреждение
        warnings = [r for r in caplog.records if r.name == "selfvoy.ai.config"]
        assert len(warnings) == 1
        message = warnings[0].getMessage()
        assert "AURA_LLM_MODEL" in message and "SELFVOY_LLM_MODEL" in message
    finally:
        reset_settings()


def test_no_warning_without_legacy_variables(caplog):
    reset_settings()
    try:
        with caplog.at_level(logging.WARNING, logger="selfvoy.ai.config"):
            get_settings()
        assert not [r for r in caplog.records if r.name == "selfvoy.ai.config"]
    finally:
        reset_settings()


def test_legacy_token_still_protects_service(monkeypatch):
    monkeypatch.setenv("SELFVOY_MEMORY_BACKEND", "memory://")
    monkeypatch.setenv("AURA_AI_TOKEN", "legacy-secret")
    monkeypatch.delenv("SELFVOY_AI_TOKEN", raising=False)
    reset_settings()
    app_module.set_agent(None)
    try:
        with TestClient(app_module.app) as client:
            assert client.get("/v1/tools").status_code == 401
            assert client.get("/v1/tools", headers={"X-Selfvoy-Token": "legacy-secret"}).status_code == 200
    finally:
        app_module.set_agent(None)
        reset_settings()
