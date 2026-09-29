"""speech.py — распознавание речи (STT).

Два провайдера (по образцу ``llm.py``):

* ``MockSTT`` — детерминированная заглушка для офлайн-разработки и тестов.
  Реального распознавания без модели нет, поэтому mock возвращает устойчивый
  текст и **прозрачно помечается** (логи, ``/healthz``). В проде используется
  внешний бэкенд — это честный dev-fallback, а не «подделка» расшифровки.
* ``WhisperSTT`` — любой Whisper-совместимый HTTP-эндпоинт
  (OpenAI ``/v1/audio/transcriptions``, whisper.cpp server,
  faster-whisper-server, vLLM и т.п.): multipart ``file`` + ``model`` +
  ``language`` → JSON ``{text, language}``.

C++-сервер не знает, кто отвечает: он получает один и тот же JSON.
"""

from __future__ import annotations

from typing import Any, Dict, Optional

# Языки, которые Aura поддерживает явно (остальные Whisper определяет сам).
SUPPORTED_LANGUAGES = ("ru", "en")

# Детерминированный текст заглушки — честно помечен как mock, чтобы его нельзя
# было принять за настоящую расшифровку.
_MOCK_TEXT = {
    "ru": "Тестовая расшифровка Ауры (mock STT).",
    "en": "Aura test transcription (mock STT).",
}


class STTError(RuntimeError):
    """Сбой распознавания (сеть/бэкенд) — не роняем сервис, отдаём ошибку."""


def normalize_language(language: Optional[str]) -> str:
    """Приводит язык к коду Whisper.

    ``auto``/пусто → ``""`` (Whisper определит сам). Известные коды — как есть.
    Прочее оставляем (Whisper примет ISO-код), но отсекаем мусор.
    """
    code = (language or "").strip().lower()
    if code in ("", "auto", "detect"):
        return ""
    # Основной язык без региона (ru-RU → ru).
    return code.split("-")[0].split("_")[0]


class BaseSTT:
    name = "base"

    def transcribe(self, audio: bytes, language: str = "", filename: str = "audio.webm") -> Dict[str, Any]:
        raise NotImplementedError  # pragma: no cover - интерфейс


class MockSTT(BaseSTT):
    """Офлайн-заглушка: детерминированный текст вместо реальной модели.

    Нужна, чтобы весь стек (Qt-клиент → C++-сервер → AI-сервис → ответ) работал
    и тестировался без внешнего Whisper. Помечается в логах и ``/healthz``.
    """

    name = "mock"

    def transcribe(self, audio: bytes, language: str = "", filename: str = "audio.webm") -> Dict[str, Any]:
        if not audio:
            raise STTError("empty audio")
        lang = normalize_language(language)
        text = _MOCK_TEXT.get(lang, _MOCK_TEXT["ru"])
        return {"text": text, "language": lang or "ru"}


class WhisperSTT(BaseSTT):
    """Whisper-совместимый провайдер (multipart file → JSON text)."""

    name = "whisper"

    def __init__(self, url: str, api_key: str = "", model: str = "whisper-1", timeout: float = 30.0):
        self.url = url
        self.api_key = api_key
        self.model = model
        self.timeout = timeout

    def transcribe(self, audio: bytes, language: str = "", filename: str = "audio.webm") -> Dict[str, Any]:
        import httpx  # импорт здесь, чтобы офлайн-режим не требовал библиотеку

        if not audio:
            raise STTError("empty audio")
        lang = normalize_language(language)
        data: Dict[str, str] = {"model": self.model, "response_format": "json"}
        if lang:
            data["language"] = lang
        headers: Dict[str, str] = {}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        files = {"file": (filename or "audio.webm", audio, "application/octet-stream")}
        try:
            response = httpx.post(self.url, files=files, data=data, headers=headers, timeout=self.timeout)
            response.raise_for_status()
            payload = response.json()
        except Exception as exc:  # сеть/лимиты/формат — отдаём понятную ошибку
            raise STTError(f"STT request failed: {exc}") from exc
        text = str(payload.get("text", "")).strip()
        detected = str(payload.get("language", "")).strip().lower()
        return {"text": text, "language": lang or detected}


def build_stt(settings: Any) -> BaseSTT:
    """Фабрика провайдера по настройкам (auto: внешний бэкенд либо mock)."""
    provider = getattr(settings, "stt_provider", "auto")
    if provider == "mock":
        return MockSTT()

    url = settings.stt_transcribe_url
    api_key = settings.stt_api_key
    if provider == "openai" and not url:
        url = "https://api.openai.com/v1/audio/transcriptions"
        api_key = api_key or settings.llm_api_key
    if not url:
        # auto без настроенного бэкенда → офлайн-заглушка.
        return MockSTT()
    return WhisperSTT(url=url, api_key=api_key, model=settings.stt_model, timeout=settings.stt_timeout)
