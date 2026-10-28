"""Общие фикстуры тестов AI-сервиса."""

import os

import pytest

from selfvoy_ai import config as config_module


@pytest.fixture(autouse=True)
def _loopback_bind(monkeypatch):
    """Тесты считают, что сервис слушает loopback (иначе SELFVOY_AI_INSECURE_DEV не действует).

    Адрес из окружения разработчика (SELFVOY_AI_HOST=0.0.0.0, UVICORN_HOST) не должен
    менять результат тестов; тесты привязки задают его сами.
    """
    monkeypatch.delenv("SELFVOY_AI_HOST", raising=False)
    monkeypatch.delenv("UVICORN_HOST", raising=False)


@pytest.fixture(autouse=True)
def _no_legacy_env(monkeypatch):
    """Устаревшие AURA_* из окружения разработчика подхватываются как запасные
    значения SELFVOY_* — в тестах их быть не должно (тесты fallback задают их сами)."""
    for name in list(os.environ):
        if name.startswith("AURA_"):
            monkeypatch.delenv(name, raising=False)
    config_module._reset_legacy_env_state()
    yield
    config_module._reset_legacy_env_state()
