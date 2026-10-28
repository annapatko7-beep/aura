"""Конфигурация Python AI Service.

Все значения берутся из переменных окружения, поэтому сервис одинаково
запускается и локально, и внутри docker-compose рядом с C++-сервером.
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass, field
from typing import List, Set

logger = logging.getLogger("selfvoy.ai.config")

# Переходный период (один релиз): до переименования продукта переменные
# назывались AURA_*. Если SELFVOY_X не задана, читаем старую AURA_X и один раз
# предупреждаем в логе. Новое имя всегда приоритетнее.
_NEW_PREFIX = "SELFVOY_"
_LEGACY_PREFIX = "AURA_"
_legacy_used: Set[str] = set()
_legacy_warned = False


def _env(name: str, default: str) -> str:
    value = os.environ.get(name)
    if (value is None or value == "") and name.startswith(_NEW_PREFIX):
        legacy = _LEGACY_PREFIX + name[len(_NEW_PREFIX):]
        legacy_value = os.environ.get(legacy)
        if legacy_value is not None and legacy_value != "":
            _legacy_used.add(legacy)
            value = legacy_value
    return default if value is None or value == "" else value


def legacy_env_names() -> List[str]:
    """Устаревшие AURA_* переменные, которые реально использовались (для логов и тестов)."""
    return sorted(_legacy_used)


def warn_legacy_env() -> bool:
    """Один раз за процесс предупреждает о прочитанных AURA_* переменных.

    Возвращает True, если предупреждение было выведено этим вызовом.
    """
    global _legacy_warned
    if _legacy_warned or not _legacy_used:
        return False
    _legacy_warned = True
    names = legacy_env_names()
    logger.warning(
        "Устаревшие переменные окружения: %s. Переименуйте их в %s — поддержка "
        "префикса AURA_ будет удалена в следующем релизе.",
        ", ".join(names),
        ", ".join(_NEW_PREFIX + n[len(_LEGACY_PREFIX):] for n in names),
    )
    return True


def _reset_legacy_env_state() -> None:
    """Только для тестов: сбросить учёт устаревших переменных и флаг предупреждения."""
    global _legacy_warned
    _legacy_used.clear()
    _legacy_warned = False


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

    # По умолчанию слушаем только loopback: AI-сервис — внутренний, к нему
    # ходит C++-сервер. В контейнере адрес задаёт CMD (сеть compose без проброса порта).
    host: str = field(default_factory=lambda: _env("SELFVOY_AI_HOST", "127.0.0.1"))
    port: int = field(default_factory=lambda: _env_int("SELFVOY_AI_PORT", 8000))

    # LLM
    llm_provider: str = field(default_factory=lambda: _env("SELFVOY_LLM_PROVIDER", "auto"))
    llm_model: str = field(default_factory=lambda: _env("SELFVOY_LLM_MODEL", "gpt-4o-mini"))
    llm_api_key: str = field(default_factory=lambda: _env("OPENAI_API_KEY", ""))
    llm_base_url: str = field(
        default_factory=lambda: _env("OPENAI_BASE_URL", "https://api.openai.com/v1")
    )
    llm_timeout: float = field(default_factory=lambda: _env_float("SELFVOY_LLM_TIMEOUT", "20"))
    llm_temperature: float = field(default_factory=lambda: _env_float("SELFVOY_LLM_TEMPERATURE", "0.3"))

    # Речь: распознавание (STT). Гибрид — клиент шлёт аудио на сервер, сервер
    # распознаёт через Whisper-совместимый бэкенд (или mock офлайн).
    # Провайдер: auto (whisper если задан URL/ключ, иначе mock) | mock | whisper | openai
    stt_provider: str = field(default_factory=lambda: _env("SELFVOY_STT_PROVIDER", "auto"))
    stt_model: str = field(default_factory=lambda: _env("SELFVOY_STT_MODEL", "whisper-1"))
    # Бэкенд: полный URL эндпоинта расшифровки (…/v1/audio/transcriptions) или
    # базовый URL OpenAI-совместимого сервиса (тогда добавим /audio/transcriptions).
    stt_url: str = field(default_factory=lambda: _env("SELFVOY_STT_URL", ""))
    stt_api_key: str = field(default_factory=lambda: _env("SELFVOY_STT_API_KEY", ""))
    stt_timeout: float = field(default_factory=lambda: _env_float("SELFVOY_STT_TIMEOUT", "30"))
    stt_default_language: str = field(default_factory=lambda: _env("SELFVOY_STT_LANGUAGE", "auto"))
    stt_max_bytes: int = field(default_factory=lambda: _env_int("SELFVOY_STT_MAX_BYTES", str(15 * 1024 * 1024)))

    # Долговременная память: postgres (DSN из SELFVOY_DATABASE_URL), postgresql://...,
    # file (./.selfvoy_memory.json), file://path или memory (только процесс).
    memory_backend: str = field(
        default_factory=lambda: _env("SELFVOY_MEMORY_BACKEND", "file://./.selfvoy_memory.json")
    )
    database_url: str = field(default_factory=lambda: _env("SELFVOY_DATABASE_URL", ""))

    # Гибридный RAG (этап 8): лексика (BM25) всегда + опциональная семантика.
    # rag_mode: lexical | hybrid | auto (auto = hybrid, если задан эндпоинт эмбеддингов).
    rag_mode: str = field(default_factory=lambda: _env("SELFVOY_RAG_MODE", "auto"))
    embeddings_url: str = field(default_factory=lambda: _env("SELFVOY_EMBEDDINGS_URL", ""))
    embeddings_model: str = field(
        default_factory=lambda: _env("SELFVOY_EMBEDDINGS_MODEL", "text-embedding-3-small")
    )
    embeddings_api_key: str = field(default_factory=lambda: _env("SELFVOY_EMBEDDINGS_API_KEY", ""))
    embeddings_timeout: float = field(default_factory=lambda: _env_float("SELFVOY_EMBEDDINGS_TIMEOUT", "15"))

    # Инструменты: sandbox (детерминированные заглушки) или http (реальные API)
    tools_backend: str = field(default_factory=lambda: _env("SELFVOY_TOOLS_BACKEND", "sandbox"))
    maps_api_url: str = field(default_factory=lambda: _env("SELFVOY_MAPS_API_URL", ""))
    email_api_url: str = field(default_factory=lambda: _env("SELFVOY_EMAIL_API_URL", ""))
    calendar_api_url: str = field(default_factory=lambda: _env("SELFVOY_CALENDAR_API_URL", ""))
    booking_api_url: str = field(default_factory=lambda: _env("SELFVOY_BOOKING_API_URL", ""))

    # CORS: Qt-клиент и веб-превью обращаются по относительным путям,
    # но для разработки разрешаем локальные origins.
    cors_origins: List[str] = field(
        default_factory=lambda: _env_list("SELFVOY_CORS_ORIGINS", "http://localhost:8000,http://127.0.0.1:8000")
    )
    # Общий секрет между C++-сервером и AI-сервисом (заголовок X-Selfvoy-Token).
    # Без токена сервис закрыт (503), если явно не включён режим разработки
    # SELFVOY_AI_INSECURE_DEV=1 (только localhost, никогда в проде).
    service_token: str = field(default_factory=lambda: _env("SELFVOY_AI_TOKEN", ""))
    insecure_dev: bool = field(
        default_factory=lambda: _env("SELFVOY_AI_INSECURE_DEV", "0").strip().lower() in ("1", "true", "yes")
    )

    preview_dir: str = field(default_factory=lambda: _env("SELFVOY_PREVIEW_DIR", ""))

    @property
    def memory_backend_kind(self) -> str:
        """Тип хранилища памяти без DSN и путей (для /healthz и логов)."""
        backend = self.memory_backend.strip().lower()
        if backend.startswith("postgres"):
            return "postgres"
        if backend.startswith("file"):
            return "file"
        return "memory"

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

        ``SELFVOY_STT_URL`` может быть как полным путём (…/audio/transcriptions),
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

    @property
    def rag_is_hybrid(self) -> bool:
        """True, если семантический поиск включён (иначе ранжирование лексическое).

        ``lexical`` — только BM25; ``hybrid`` — требовать семантику;
        ``auto`` (по умолчанию) — семантика включается при заданном
        ``SELFVOY_EMBEDDINGS_URL``. Без эндпоинта эмбеддингов гибрида нет,
        но ранжирование продолжает работать на лексике (graceful degrade).
        """
        if self.rag_mode == "lexical":
            return False
        if self.rag_mode == "hybrid":
            return True
        return bool(self.embeddings_url)


_settings: Settings | None = None


def get_settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()
        warn_legacy_env()
    return _settings


def reset_settings() -> None:
    """Используется тестами, чтобы перечитать окружение."""
    global _settings
    _settings = None
