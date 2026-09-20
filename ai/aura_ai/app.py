"""app.py — HTTP-слой Python AI Service.

Эндпоинты, которые вызывает C++-сервер:

* ``POST /v1/agent/run``        — главный запрос Ауры (AgentManager);
* ``POST /v1/agent/negotiate``  — Agent-to-Agent переговоры между двумя Аурами;
* ``POST /v1/memory/extract``   — извлечение фактов в долговременную память;
* ``POST /v1/planner/slots``    — свободные слоты (Planner напрямую);
* ``GET  /v1/tools``            — список инструментов (ToolManager).

Запуск: ``uvicorn aura_ai.app:app --host 0.0.0.0 --port 8000``
"""

from __future__ import annotations

import base64
import binascii
import logging
import os
import secrets
from datetime import timedelta
from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import memory as memory_module
from . import planner
from .agent import AuraAgent
from .config import Settings, get_settings
from .llm import build_llm
from .speech import STTError, BaseSTT, build_stt
from .models import (
    AgentContext,
    AgentRequest,
    AgentResponse,
    CalendarEvent,
    MemoryEntry,
    NegotiateRequest,
    NegotiateResponse,
    Preferences,
    utcnow,
)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("aura.ai")


@asynccontextmanager
async def _lifespan(_: FastAPI):
    settings = get_settings()
    if not settings.service_token:
        logger.warning(
            "AURA_AI_TOKEN не задан: эндпоинты AI-сервиса открыты без авторизации. "
            "Для работы вне локальной машины задайте токен."
        )
    logger.info(
        "Aura AI Service запущен (llm=%s, stt=%s)",
        build_llm(settings).name,
        build_stt(settings).name,
    )
    yield

app = FastAPI(
    title="Aura AI Service",
    version="0.1.0",
    description="Сеть ИИ-агентов: планирование, память, инструменты, Agent-to-Agent.",
    lifespan=_lifespan,
)

# CORS должен быть добавлен до старта приложения (иначе Starlette ругается
# на изменение стека middleware), поэтому делаем это на уровне модуля.
app.add_middleware(
    CORSMiddleware,
    allow_origins=get_settings().cors_origins or ["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)

_agent: Optional[AuraAgent] = None


def get_agent() -> AuraAgent:
    global _agent
    if _agent is None:
        _agent = AuraAgent(get_settings())
    return _agent


def set_agent(agent: Optional[AuraAgent]) -> None:
    """Тесты подменяют агента детерминированной реализацией."""
    global _agent
    _agent = agent


_stt: Optional[BaseSTT] = None


def get_stt() -> BaseSTT:
    global _stt
    if _stt is None:
        _stt = build_stt(get_settings())
    return _stt


def set_stt(stt: Optional[BaseSTT]) -> None:
    """Тесты подменяют STT детерминированной реализацией."""
    global _stt
    _stt = stt


def _settings() -> Settings:
    return get_settings()


def require_token(x_aura_token: str = Header(default="")) -> None:
    expected = get_settings().service_token
    if not expected:
        # Токен не задан — сервис открыт. Это удобно в разработке, но недопустимо
        # в сети: предупреждение пишется при старте (см. _lifespan).
        return
    # compare_digest, а не «!=»: обычное сравнение строк измеряемо по времени.
    if not secrets.compare_digest(x_aura_token, expected):
        raise HTTPException(status_code=401, detail="invalid service token")





# ----------------------------------------------------------------- сервисные
@app.get("/healthz")
def healthz() -> Dict[str, Any]:
    settings = get_settings()
    agent = get_agent()
    return {
        "status": "ok",
        "service": "aura-ai",
        "llm": agent.llm.name,
        "stt": get_stt().name,
        "tools_backend": settings.tools_backend,
        "memory_backend": settings.memory_backend,
        "time": utcnow().isoformat(),
    }


@app.get("/v1/tools", dependencies=[Depends(require_token)])
def list_tools() -> Dict[str, Any]:
    return {"tools": get_agent().tools.list()}


# ------------------------------------------------------------------- агент
@app.post("/v1/agent/run", response_model=AgentResponse, dependencies=[Depends(require_token)])
def agent_run(request: AgentRequest) -> AgentResponse:
    agent = get_agent()
    return agent.run(request)


@app.post("/v1/agent/negotiate", response_model=NegotiateResponse, dependencies=[Depends(require_token)])
def agent_negotiate(request: NegotiateRequest) -> NegotiateResponse:
    return get_agent().negotiate(request)


# -------------------------------------------------------------------- речь
class TranscribeResponse(BaseModel):
    text: str
    language: str = ""
    provider: str = ""


async def _read_audio(request: Request) -> tuple[bytes, str, str]:
    """Читает аудио из запроса: multipart/form-data (Whisper) либо JSON (base64).

    Возвращает ``(audio_bytes, language, filename)``.
    """
    ctype = request.headers.get("content-type", "")
    if ctype.startswith("multipart/form-data"):
        form = await request.form()
        upload = form.get("file")
        if upload is None:
            raise HTTPException(status_code=400, detail="no audio file")
        audio = await upload.read()
        language = str(form.get("language", "") or "")
        filename = getattr(upload, "filename", "") or "audio.webm"
        return audio, language, filename
    # JSON-путь (внутренний C++-клиент шлёт base64, чтобы не строить multipart).
    try:
        payload = await request.json()
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid request body") from exc
    b64 = payload.get("audio") or payload.get("audio_base64") or ""
    if not b64:
        raise HTTPException(status_code=400, detail="no audio data")
    try:
        audio = base64.b64decode(b64, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail="invalid base64 audio") from exc
    language = str(payload.get("language", "") or "")
    filename = str(payload.get("filename", "") or "audio.webm")
    return audio, language, filename


@app.post("/v1/speech/transcribe", response_model=TranscribeResponse, dependencies=[Depends(require_token)])
async def speech_transcribe(request: Request) -> TranscribeResponse:
    """Распознавание речи (STT).

    Whisper-совместимый вход: ``multipart/form-data`` с полем ``file`` и
    необязательным ``language`` (ru/en/auto) — как OpenAI
    ``/v1/audio/transcriptions``. Дополнительно принимает JSON с base64-аудио
    (внутренний путь C++-сервера). Ответ: ``{text, language, provider}``.
    """
    settings = get_settings()
    audio, language, filename = await _read_audio(request)
    if not audio:
        raise HTTPException(status_code=400, detail="empty audio")
    if len(audio) > settings.stt_max_bytes:
        raise HTTPException(status_code=413, detail="audio too large")

    stt = get_stt()
    lang = language or settings.stt_default_language
    try:
        result = stt.transcribe(audio, lang, filename)
    except STTError as exc:
        # Бэкенд недоступен/ошибся — отдаём 502, клиент покажет fallback на текст.
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    return TranscribeResponse(
        text=result.get("text", ""),
        language=result.get("language", ""),
        provider=stt.name,
    )


# ------------------------------------------------------------------- память
class MemoryExtractRequest(BaseModel):
    user_key: str = ""
    message: str = ""
    context: AgentContext = Field(default_factory=AgentContext)
    commit: bool = True


class MemoryExtractResponse(BaseModel):
    entries: List[MemoryEntry]
    stored: List[MemoryEntry]


@app.post("/v1/memory/extract", response_model=MemoryExtractResponse, dependencies=[Depends(require_token)])
def memory_extract(request: MemoryExtractRequest) -> MemoryExtractResponse:
    agent = get_agent()
    context = request.context.model_copy(update={"message": request.message or request.context.message})
    entries = memory_module.extract_from_context(context)
    stored: List[MemoryEntry] = []
    if entries and request.commit:
        key = request.user_key or memory_module.user_key(context)
        stored = agent.store.save(key, entries)
    return MemoryExtractResponse(entries=entries, stored=stored)


@app.get("/v1/memory/{user_key}", dependencies=[Depends(require_token)])
def memory_load(user_key: str, query: str = "") -> Dict[str, Any]:
    entries = get_agent().store.load(user_key)
    if query:
        entries = memory_module.rank(query, entries)
    return {"user_key": user_key, "entries": memory_module.as_dicts(entries)}


# ---------------------------------------------------------------- планировщик
class SlotsRequest(BaseModel):
    window: Optional[Dict[str, Any]] = None
    calendar: List[CalendarEvent] = Field(default_factory=list)
    preferences: Preferences = Field(default_factory=Preferences)
    duration_minutes: int = 60
    max_slots: int = 5


@app.post("/v1/planner/slots", dependencies=[Depends(require_token)])
def planner_slots(request: SlotsRequest) -> Dict[str, Any]:
    now = utcnow()
    window = planner.TimeWindow(
        start=_parse(request.window, "start") or now,
        end=_parse(request.window, "end") or (now + timedelta(hours=24)),
        label=str((request.window or {}).get("label", "окно")),
    )
    slots = planner.free_slots(
        window,
        request.calendar,
        request.preferences,
        duration_minutes=request.duration_minutes,
        max_slots=request.max_slots,
    )
    return {
        "window": window.as_dict(),
        "slots": [slot.model_dump(mode="json") for slot in slots],
    }


class UnderstandRequest(BaseModel):
    text: str
    now: Optional[Any] = None


@app.post("/v1/planner/understand", dependencies=[Depends(require_token)])
def planner_understand(request: UnderstandRequest) -> Dict[str, Any]:
    moment = _parse_dt(request.now)
    return planner.understand(request.text, moment).as_dict()


def _parse(payload: Optional[Dict[str, Any]], key: str):
    return _parse_dt((payload or {}).get(key))


def _parse_dt(value: Any):
    from datetime import datetime

    if not value:
        return None
    if isinstance(value, datetime):
        return value
    try:
        return datetime.fromisoformat(str(value))
    except ValueError:
        return None


# ------------------------------------------------------------------- превью
def _preview_dir() -> str:
    configured = get_settings().preview_dir
    if configured:
        return configured
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "preview"))


def mount_preview() -> None:
    """Отдаёт HTML-превью дизайна Qt-клиента (та же палитра и «стеклянные» кнопки)."""
    directory = _preview_dir()
    if not os.path.isdir(directory):
        logger.warning("preview dir not found: %s", directory)
        return

    app.mount("/static", StaticFiles(directory=directory), name="preview-static")

    @app.get("/", include_in_schema=False)
    def preview_index() -> FileResponse:
        return FileResponse(os.path.join(directory, "index.html"))


@app.exception_handler(Exception)
async def unhandled(_request, exc: Exception) -> JSONResponse:  # pragma: no cover
    logger.exception("unhandled error")
    return JSONResponse(status_code=500, content={"detail": f"internal error: {exc}"})


mount_preview()


def main() -> None:  # pragma: no cover
    import uvicorn

    settings = get_settings()
    uvicorn.run(app, host=settings.host, port=settings.port)


if __name__ == "__main__":  # pragma: no cover
    main()
