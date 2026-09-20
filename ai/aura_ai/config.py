"""Конфигурация Python AI Service.

Все значения берутся из переменных окружения, поэтому сервис одинаково
запускается и локально, и внутри docker-compose рядом с C++-сервером.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import List


def _env(name: str, default: str) -> str:
    value = os.environ.get(name)
    return default if value is None or value == "" else value


def _env_int(name: str, default: int) -> int:
    try:
        return int(_env(name, str(default)))
    except ValueError:
        return default


def _env_float(name: str, default: float) -> float:
    try:
        return float(_env(name, str(default)))
    except ValueError:
        return default


def _env_list(name: str, default: str) -> List[str]:
    return [item.strip() for item in _env(name, default).split(",") if item.strip()]


@dataclass
class Settings:
    """Настройки сервиса (snapshot переменных окружения)."""

    host: str = field(default_factory=lambda: _env("AURA_AI_HOST", "0.0.0.0"))
    port: int = field(default_factory=lambda: _env_int("AURA_AI_PORT", 8000))

    # LLM
    llm_provider: str = field(default_factory=lambda: _env("AURA_LLM_PROVIDER", "auto"))
    llm_model: str = field(default_factory=lambda: _env("AURA_LLM_MODEL", "gpt-4o-mini"))
    llm_api_key: str = field(default_factory=lambda: _env("OPENAI_API_KEY", ""))
    llm_base_url: str = field(
        default_factory=lambda: _env("OPENAI_BASE_URL", "https://api.openai.com/v1")
    )
    llm_timeout: float = field(default_factory=lambda: _env_float("AURA_LLM_TIMEOUT", "20"))
    llm_temperature: float = field(default_factory=lambda: _env_float("AURA_LLM_TEMPERATURE", "0.3"))

    # Речь: распознавание (STT). Гибрид — клиент шлёт аудио на сервер, сервер
    # распознаёт через Whisper-совместимый бэкенд (или mock офлайн).
    # Провайдер: auto (whisper если задан URL/ключ, иначе mock) | mock | whisper | openai
    stt_provider: str = field(default_factory=lambda: _env("AURA_STT_PROVIDER", "auto"))
    stt_model: str = field(default_factory=lambda: _env("AURA_STT_MODEL", "whisper-1"))
    # Бэкенд: полный URL эндпоинта расшифровки (…/v1/audio/transcriptions) или
    # базовый URL OpenAI-совместимого сервиса (тогда добавим /audio/transcriptions).
    stt_url: str = field(default_factory=lambda: _env("AURA_STT_URL", ""))
    stt_api_key: str = field(default_factory=lambda: _env("AURA_STT_API_KEY", ""))
    stt_timeout: float = field(default_factory=lambda: _env_float("AURA_STT_TIMEOUT", "30"))
    stt_default_language: str = field(default_factory=lambda: _env("AURA_STT_LANGUAGE", "auto"))
    stt_max_bytes: int = field(default_factory=lambda: _env_int("AURA_STT_MAX_BYTES", str(15 * 1024 * 1024)))

    # Долговременная память: postgres://... или file://path (по умолчанию файл)
    memory_backend: str = field(
        default_factory=lambda: _env("AURA_MEMORY_BACKEND", "file://./.aura_memory.json")
    )

    # Инструменты: sandbox (детерминированные заглушки) или http (реальные API)
    tools_backend: str = field(default_factory=lambda: _env("AURA_TOOLS_BACKEND", "sandbox"))
    maps_api_url: str = field(default_factory=lambda: _env("AURA_MAPS_API_URL", ""))
    email_api_url: str = field(default_factory=lambda: _env("AURA_EMAIL_API_URL", ""))
    calendar_api_url: str = field(default_factory=lambda: _env("AURA_CALENDAR_API_URL", ""))
    booking_api_url: str = field(default_factory=lambda: _env("AURA_BOOKING_API_URL", ""))

    # CORS: Qt-клиент и веб-превью обращаются по относительным путям,
    # но для разработки разрешаем локальные origins.
    cors_origins: List[str] = field(
        default_factory=lambda: _env_list("AURA_CORS_ORIGINS", "http://localhost:8000,http://127.0.0.1:8000")
    )
    # Общий секрет между C++-сервером и AI-сервисом (заголовок X-Aura-Token)
    service_token: str = field(default_factory=lambda: _env("AURA_AI_TOKEN", ""))

    preview_dir: str = field(default_factory=lambda: _env("AURA_PREVIEW_DIR", ""))

    @property
    def llm_is_remote(self) -> bool:
        if self.llm_provider == "mock":
            return False
        if self.llm_provider in ("openai", "http"):
            return True
        return bool(self.llm_api_key)

    @property
    def stt_is_remote(self) -> bool:
        """True, если распознавание идёт через внешний Whisper-совместимый сервис."""
        if self.stt_provider == "mock":
            return False
        if self.stt_provider in ("whisper", "openai"):
            return True
        # auto: внешний, если задан URL бэкенда (или API-ключ OpenAI).
        return bool(self.stt_url or self.stt_api_key)

    @property
    def stt_transcribe_url(self) -> str:
        """Полный URL эндпоинта расшифровки.

        ``AURA_STT_URL`` может быть как полным путём (…/audio/transcriptions),
        так и базовым URL сервиса — тогда добавляем стандартный путь Whisper.
        """
        url = self.stt_url.rstrip("/")
        if not url:
            return ""
        if url.endswith("/transcriptions"):
            return url
        if url.endswith("/v1"):
            return url + "/audio/transcriptions"
        return url + "/v1/audio/transcriptions"


_settings: Settings | None = None


def get_settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()
    return _settings


def reset_settings() -> None:
    """Используется тестами, чтобы перечитать окружение."""
    global _settings
    _settings = None
