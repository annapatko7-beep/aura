"""app.py — HTTP-слой Python AI Service.

Эндпоинты, которые вызывает C++-сервер:

* ``POST /v1/agent/run``        — главный запрос к Selfvoy (AgentManager);
* ``POST /v1/agent/negotiate``  — Agent-to-Agent переговоры между двумя агентами Selfvoy;
* ``POST /v1/agent/slots``      — слоты владельца: приглашение по ссылке и страница записи;
* ``POST /v1/agent/series``     — еженедельная серия записи в местном времени владельца;
* ``POST /v1/agent/admin``      — ИИ-администратор страницы записи в Telegram (F7);
* ``POST /v1/memory/extract``   — извлечение фактов в долговременную память;
* ``POST /v1/planner/slots``    — свободные слоты (Planner напрямую);
* ``GET  /v1/tools``            — список инструментов (ToolManager).

Запуск: ``SELFVOY_AI_TOKEN=… uvicorn selfvoy_ai.app:app --host 127.0.0.1 --port 8000``

Без ``SELFVOY_AI_TOKEN`` все эндпоинты, кроме ``/healthz``, отвечают 503, если не
задан ``SELFVOY_AI_INSECURE_DEV=1`` (только локальная разработка и только при
привязке к loopback: ``SELFVOY_AI_HOST`` = 127.0.0.1 / ::1 / localhost, иначе флаг
игнорируется). ``/docs`` и ``/openapi.json`` отдаются только в этом режиме.
"""

from __future__ import annotations

import base64
import binascii
import logging
import os
import secrets
import sys
from datetime import datetime, timedelta
from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, ValidationError
from starlette.concurrency import run_in_threadpool
from starlette.datastructures import UploadFile

from . import admin as admin_module
from . import memory as memory_module
from . import planner
from .agent import SelfvoyAgent
from .config import Settings, get_settings
from .llm import build_llm
from .speech import STTError, BaseSTT, build_stt
from .models import (
    AgentContext,
    AgentRequest,
    AgentResponse,
    Calendar,
    MAX_DURATION_MINUTES,
    MAX_MESSAGE_CHARS,
    MAX_SLOTS,
    MAX_WINDOW_HOURS,
    MemoryEntry,
    NegotiateRequest,
    NegotiateResponse,
    Preferences,
    SeriesRequest,
    SeriesResponse,
    SlotsRequest as AgentSlotsRequest,
    SlotsResponse as AgentSlotsResponse,
    as_aware,
    utcnow,
)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("selfvoy.ai")

_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "[::1]", "localhost"})


def _bind_host(settings: Settings) -> str:
    """Адрес, на котором слушает сервис: ``SELFVOY_AI_HOST`` (main(), Dockerfile).

    Если процесс запущен как ``uvicorn … --host X`` (или с ``UVICORN_HOST``), берём
    X оттуда: так флаг разработки не откроет сервис, привязанный к 0.0.0.0 из
    командной строки.
    """
    argv = sys.argv[1:]
    for index, arg in enumerate(argv):
        if arg == "--host" and index + 1 < len(argv):
            return argv[index + 1].strip()
        if arg.startswith("--host="):
            return arg.split("=", 1)[1].strip()
    uvicorn_host = os.environ.get("UVICORN_HOST", "").strip()
    if uvicorn_host:
        return uvicorn_host
    return (settings.host or "").strip()


def insecure_dev_allowed(settings: Settings) -> bool:
    """SELFVOY_AI_INSECURE_DEV=1 действует только на loopback (fail closed).

    С флагом, но на внешнем адресе (0.0.0.0, IP сети) сервис остаётся закрыт:
    без SELFVOY_AI_TOKEN — 503, как будто флага нет.
    """
    if not settings.insecure_dev:
        return False
    return _bind_host(settings).lower() in _LOOPBACK_HOSTS


@asynccontextmanager
async def _lifespan(_: FastAPI):
    settings = get_settings()
    if not settings.service_token:
        if insecure_dev_allowed(settings):
            logger.warning(
                "SELFVOY_AI_TOKEN не задан, SELFVOY_AI_INSECURE_DEV=1: эндпоинты открыты без "
                "авторизации — только для локальной разработки, никогда в проде."
            )
        elif settings.insecure_dev:
            logger.error(
                "SELFVOY_AI_INSECURE_DEV=1 проигнорирован: сервис слушает %r, а не loopback "
                "(127.0.0.1/::1/localhost). Эндпоинты закрыты (503) — задайте SELFVOY_AI_TOKEN.",
                _bind_host(settings),
            )
        else:
            logger.error(
                "SELFVOY_AI_TOKEN не задан: эндпоинты AI-сервиса закрыты (503). Задайте общий "
                "с C++-сервером токен (openssl rand -hex 32) или для локальной разработки "
                "SELFVOY_AI_INSECURE_DEV=1."
            )
    logger.info(
        "Selfvoy AI Service запущен (llm=%s, stt=%s)",
        build_llm(settings).name,
        build_stt(settings).name,
    )
    yield


def docs_options(settings: Settings) -> Dict[str, Optional[str]]:
    """/docs и /openapi.json раскрывают все эндпоинты и схемы без токена.

    Включены только в локальной разработке (SELFVOY_AI_INSECURE_DEV=1 на loopback),
    в проде выключены (404).
    """
    enabled = insecure_dev_allowed(settings)
    return {
        "docs_url": "/docs" if enabled else None,
        "redoc_url": None,
        "openapi_url": "/openapi.json" if enabled else None,
    }


app = FastAPI(
    title="Selfvoy AI Service",
    version="0.1.0",
    description="Сеть ИИ-агентов: планирование, память, инструменты, Agent-to-Agent.",
    lifespan=_lifespan,
    **docs_options(get_settings()),
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

_agent: Optional[SelfvoyAgent] = None


def get_agent() -> SelfvoyAgent:
    global _agent
    if _agent is None:
        _agent = SelfvoyAgent(get_settings())
    return _agent


def set_agent(agent: Optional[SelfvoyAgent]) -> None:
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


def require_token(x_selfvoy_token: str = Header(default="")) -> None:
    settings = get_settings()
    expected = settings.service_token
    if not expected:
        # Fail closed: без токена сервис открыт только при явном
        # SELFVOY_AI_INSECURE_DEV=1 и привязке к loopback (локальная разработка).
        if insecure_dev_allowed(settings):
            return
        raise HTTPException(status_code=503, detail="AI service token is not configured")
    # compare_digest по байтам: строки с не-ASCII символами compare_digest не
    # принимает (TypeError → 500), а заголовок приходит от клиента.
    if not secrets.compare_digest(x_selfvoy_token.encode("utf-8"), expected.encode("utf-8")):
        raise HTTPException(status_code=401, detail="invalid service token")



# ----------------------------------------------------------------- сервисные
@app.get("/healthz")
def healthz() -> Dict[str, Any]:
    """Публичная проверка живости: только типы бэкендов, без DSN, путей и ключей."""
    settings = get_settings()
    agent = get_agent()
    return {
        "status": "ok",
        "service": "selfvoy-ai",
        "llm": agent.llm.name,
        "stt": get_stt().name,
        "tools_backend": settings.tools_backend,
        "memory_backend": settings.memory_backend_kind,
        "time": utcnow().isoformat(),
    }


@app.get("/v1/tools", dependencies=[Depends(require_token)])
def list_tools() -> Dict[str, Any]:
    return {"tools": get_agent().tools.list()}


# ------------------------------------------------------------------- агент
@app.post("/v1/agent/run", response_model=AgentResponse, dependencies=[Depends(require_token)])
def agent_run(request: AgentRequest) -> AgentResponse:
    agent = get_agent()
    try:
        return agent.run(request)
    except ValidationError:
        raise  # свой обработчик ниже: 422 «invalid input»
    except OverflowError as exc:
        # Время из запроса за пределами datetime (context.now у самой границы,
        # «через N недель» от него) — ошибка входных данных, а не 500.
        logger.info("agent.run: время вне диапазона")
        raise HTTPException(status_code=422, detail="time is out of range") from exc
    except ValueError as exc:
        # Неразборчивые числа/даты в тексте запроса (int() на тысячах цифр и т.п.).
        logger.info("agent.run: некорректные входные данные (%s)", type(exc).__name__)
        raise HTTPException(status_code=422, detail="invalid input") from exc


@app.post("/v1/agent/negotiate", response_model=NegotiateResponse, dependencies=[Depends(require_token)])
def agent_negotiate(request: NegotiateRequest) -> NegotiateResponse:
    return get_agent().negotiate(request)


@app.post("/v1/agent/series", response_model=SeriesResponse, dependencies=[Depends(require_token)])
def agent_series(request: SeriesRequest) -> SeriesResponse:
    """Еженедельная серия страницы записи в местном времени владельца (booking.book)."""
    try:
        return SeriesResponse(
            occurrences=planner.weekly_series(
                request.start, request.timezone, request.duration_minutes, request.count, request.interval_days
            )
        )
    except OverflowError as exc:
        raise HTTPException(status_code=422, detail="time is out of range") from exc


@app.post("/v1/agent/slots", response_model=AgentSlotsResponse, dependencies=[Depends(require_token)])
def agent_slots(request: AgentSlotsRequest) -> AgentSlotsResponse:
    """Слоты владельца для приглашения по ссылке (invite.create на C++-сервере)."""
    try:
        return get_agent().offer_slots(request)
    except OverflowError as exc:  # context.now у самой границы datetime.max
        raise HTTPException(status_code=422, detail="time is out of range") from exc


def admin_client() -> Optional[admin_module.ChatJson]:
    """JSON-клиент LLM для администратора: у MockLLM его нет — работает запасной режим."""
    return getattr(get_agent().llm, "chat_json", None)


@app.post("/v1/agent/admin", response_model=admin_module.AdminResponse, dependencies=[Depends(require_token)])
def agent_admin(request: admin_module.AdminRequest) -> admin_module.AdminResponse:
    """ИИ-администратор в Telegram (F7): ответ гостю и предложенное действие (сервер всё проверяет)."""
    return admin_module.answer(request, admin_client())


# -------------------------------------------------------------------- речь
class TranscribeResponse(BaseModel):
    text: str
    language: str = ""
    provider: str = ""


# JSON-путь несёт base64 (+~33%) и служебные поля; multipart — заголовки частей.
_BODY_OVERHEAD = 64 * 1024


def _body_limit(settings: Settings) -> int:
    return settings.stt_max_bytes * 4 // 3 + _BODY_OVERHEAD


async def _read_body_limited(request: Request, limit: int) -> bytes:
    """Читает тело потоком и обрывает его на ``limit`` байтах (413).

    Content-Length проверяется до чтения; для chunked-тела предел соблюдается
    по мере чтения, а не после того, как всё тело уже в памяти.
    """
    declared = request.headers.get("content-length")
    if declared is not None:
        try:
            if int(declared) > limit:
                raise HTTPException(status_code=413, detail="audio too large")
        except ValueError as exc:
            raise HTTPException(status_code=400, detail="invalid content-length") from exc
    chunks: List[bytes] = []
    total = 0
    async for chunk in request.stream():
        total += len(chunk)
        if total > limit:
            raise HTTPException(status_code=413, detail="audio too large")
        chunks.append(chunk)
    body = b"".join(chunks)
    # Starlette берёт уже прочитанное тело из request._body (form()/json()).
    request._body = body
    return body


async def _read_audio(request: Request, limit: int) -> tuple[bytes, str, str]:
    """Читает аудио из запроса: multipart/form-data (Whisper) либо JSON (base64).

    Возвращает ``(audio_bytes, language, filename)``.
    """
    await _read_body_limited(request, limit)
    ctype = request.headers.get("content-type", "")
    if ctype.startswith("multipart/form-data"):
        try:
            form = await request.form()
        except Exception as exc:  # битый multipart → 400, а не 500
            raise HTTPException(status_code=400, detail="invalid multipart body") from exc
        upload = form.get("file")
        if upload is None:
            raise HTTPException(status_code=400, detail="no audio file")
        if not isinstance(upload, UploadFile):
            raise HTTPException(status_code=400, detail="file must be an uploaded file")
        audio = await upload.read()
        language = form.get("language", "")
        language = language if isinstance(language, str) else ""
        filename = upload.filename or "audio.webm"
        return audio, language[:16], filename[:128]
    # JSON-путь (внутренний C++-клиент шлёт base64, чтобы не строить multipart).
    try:
        payload = await request.json()
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid request body") from exc
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400, detail="request body must be a JSON object")
    b64 = payload.get("audio") or payload.get("audio_base64") or ""
    if not b64:
        raise HTTPException(status_code=400, detail="no audio data")
    if not isinstance(b64, str):
        raise HTTPException(status_code=400, detail="audio must be a base64 string")
    try:
        audio = base64.b64decode(b64, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise HTTPException(status_code=400, detail="invalid base64 audio") from exc
    language = payload.get("language", "")
    filename = payload.get("filename", "")
    language = language if isinstance(language, str) else ""
    filename = filename if isinstance(filename, str) and filename else "audio.webm"
    return audio, language[:16], filename[:128]


@app.post("/v1/speech/transcribe", response_model=TranscribeResponse, dependencies=[Depends(require_token)])
async def speech_transcribe(request: Request) -> TranscribeResponse:
    """Распознавание речи (STT).

    Whisper-совместимый вход: ``multipart/form-data`` с полем ``file`` и
    необязательным ``language`` (ru/en/auto) — как OpenAI
    ``/v1/audio/transcriptions``. Дополнительно принимает JSON с base64-аудио
    (внутренний путь C++-сервера). Ответ: ``{text, language, provider}``.
    """
    settings = get_settings()
    audio, language, filename = await _read_audio(request, _body_limit(settings))
    if not audio:
        raise HTTPException(status_code=400, detail="empty audio")
    if len(audio) > settings.stt_max_bytes:
        raise HTTPException(status_code=413, detail="audio too large")

    stt = get_stt()
    lang = language or settings.stt_default_language
    try:
        # Бэкенд синхронный (httpx.post) — в пул потоков, чтобы не блокировать
        # event loop на время распознавания.
        result = await run_in_threadpool(stt.transcribe, audio, lang, filename)
    except STTError as exc:
        # Бэкенд недоступен/ошибся — отдаём 502, клиент покажет fallback на текст.
        logger.warning("STT error: %s", exc)
        raise HTTPException(status_code=502, detail="speech backend unavailable") from exc
    return TranscribeResponse(
        text=result.get("text", ""),
        language=result.get("language", ""),
        provider=stt.name,
    )


# ------------------------------------------------------------------- память
class MemoryExtractRequest(BaseModel):
    user_key: str = Field(default="", max_length=320)
    message: str = Field(default="", max_length=MAX_MESSAGE_CHARS)
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
    agent = get_agent()
    entries = agent.store.load(user_key)
    if query:
        # Гибридный RAG: BM25 всегда + семантика, если настроены эмбеддинги.
        entries = memory_module.rank(query, entries, embed_client=agent.embed_client)
    return {"user_key": user_key, "entries": memory_module.as_dicts(entries)}


# ---------------------------------------------------------------- планировщик
class SlotsWindow(BaseModel):
    start: Optional[datetime] = None
    end: Optional[datetime] = None
    label: str = Field(default="окно", max_length=64)


class SlotsRequest(BaseModel):
    window: Optional[SlotsWindow] = None
    calendar: Calendar = Field(default_factory=list)
    preferences: Preferences = Field(default_factory=Preferences)
    duration_minutes: int = Field(default=60, ge=1, le=MAX_DURATION_MINUTES)
    max_slots: int = Field(default=5, ge=1, le=MAX_SLOTS)


@app.post("/v1/planner/slots", dependencies=[Depends(require_token)])
def planner_slots(request: SlotsRequest) -> Dict[str, Any]:
    now = utcnow()
    window = request.window or SlotsWindow()
    # Время без пояса — UTC (иначе сравнение с календарём падает в 500).
    start = as_aware(window.start) if window.start else now
    try:
        end = as_aware(window.end) if window.end else start + timedelta(hours=24)
    except OverflowError as exc:
        raise HTTPException(status_code=422, detail="window is out of range") from exc
    if end <= start:
        raise HTTPException(status_code=422, detail="window.end must be after window.start")
    if end - start > timedelta(hours=MAX_WINDOW_HOURS):
        raise HTTPException(status_code=422, detail=f"window is longer than {MAX_WINDOW_HOURS} hours")
    time_window = planner.TimeWindow(start=start, end=end, label=window.label)
    try:
        slots = planner.free_slots(
            time_window,
            request.calendar,
            request.preferences,
            duration_minutes=request.duration_minutes,
            max_slots=request.max_slots,
        )
    except OverflowError as exc:  # окно у самой границы datetime.max
        raise HTTPException(status_code=422, detail="window is out of range") from exc
    return {
        "window": time_window.as_dict(),
        "slots": [slot.model_dump(mode="json") for slot in slots],
    }


class UnderstandRequest(BaseModel):
    text: str = Field(max_length=4000)
    now: Optional[Any] = None


@app.post("/v1/planner/understand", dependencies=[Depends(require_token)])
def planner_understand(request: UnderstandRequest) -> Dict[str, Any]:
    moment = _parse_dt(request.now)
    try:
        return planner.understand(request.text, moment).as_dict()
    except OverflowError as exc:  # «через 10^9 недель»
        raise HTTPException(status_code=422, detail="time is out of range") from exc


# --------------------------------------------------------------------- места
class PlacesSearchRequest(BaseModel):
    city: str = Field(default="", max_length=120)
    midpoint: Optional[Dict[str, float]] = None
    diet: List[str] = Field(default_factory=list)
    budget_limit: Optional[float] = None
    limit: int = Field(default=3, ge=1, le=20)
    radius: int = Field(default=2500, ge=100, le=50000)


@app.post("/v1/places/search", dependencies=[Depends(require_token)])
def places_search(request: PlacesSearchRequest) -> Dict[str, Any]:
    from . import places

    return places.search_places(
        city=request.city,
        midpoint=request.midpoint,
        diet=request.diet,
        budget_limit=request.budget_limit,
        limit=request.limit,
        radius=request.radius,
    )


# ------------------------------------------------------------------ бронирование
class BookTableRequest(BaseModel):
    place: str = Field(min_length=1, max_length=120)
    at: str = Field(min_length=5)
    people: int = Field(default=2, ge=1, le=50)
    webhook_url: Optional[str] = None


@app.post("/v1/tools/book_table", dependencies=[Depends(require_token)])
def api_book_table(request: BookTableRequest) -> Dict[str, Any]:
    from . import tools

    dt = _parse_dt(request.at) or utcnow()
    return tools.book_table(
        place=request.place,
        at=dt,
        people=request.people,
        webhook_url=request.webhook_url,
    )


# ------------------------------------------------------------------- федерация (AFP)
@app.get("/.well-known/aura-agent")
def well_known_aura_agent(user: Optional[str] = None) -> Dict[str, Any]:
    """Well-Known манифест узла Aura для обнаружения удалёнными агентами."""
    from . import federation

    _, pub = federation.get_server_keypair()
    return {
        "protocol": "AFP/1.0",
        "public_key": pub,
        "supported_versions": ["1.0"],
        "inbox_url": "/v1/federation/inbox",
        "user": user or "default",
        "capabilities": ["negotiation", "calendar_sync", "places_search", "booking"],
    }


class FederationInboxResponse(BaseModel):
    ok: bool = True
    status: str = "received"
    reply: Optional[Dict[str, Any]] = None
    error: Optional[str] = None


@app.post("/v1/federation/inbox", response_model=FederationInboxResponse)
async def federation_inbox(request: Request) -> FederationInboxResponse:
    """Входящий почтовый ящик межагентной федерации с верификацией подписи Ed25519."""
    from . import federation

    try:
        envelope = await request.json()
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid json envelope") from exc

    if not isinstance(envelope, dict):
        raise HTTPException(status_code=400, detail="envelope must be a json object")

    sender = envelope.get("sender", "")
    if not sender:
        raise HTTPException(status_code=400, detail="missing sender handle")

    meta = federation.discover_remote_agent(sender)
    pub_key = meta.get("public_key") if meta else envelope.get("public_key", "")
    if not pub_key:
        raise HTTPException(status_code=400, detail=f"cannot resolve public key for {sender}")

    valid, reason = federation.verify_envelope(envelope, pub_key)
    if not valid:
        logger.warning("Federation envelope verification failed: %s", reason)
        raise HTTPException(status_code=401, detail=f"signature verification failed: {reason}")

    intent = envelope.get("intent", "ping")
    recipient = envelope.get("recipient", "")

    if intent in ("ping", "hello"):
        return FederationInboxResponse(
            ok=True,
            status="pong",
            reply={"node": "aura", "protocol": "AFP/1.0", "time": utcnow().isoformat()},
        )

    if intent in ("negotiate", "schedule_meeting", "propose_meeting"):
        return FederationInboxResponse(
            ok=True,
            status="processed",
            reply={
                "message": f"Aura accepted proposal from {sender} for {recipient}",
                "intent": intent,
                "timestamp": utcnow().isoformat(),
            },
        )

    return FederationInboxResponse(ok=True, status="accepted", reply={"intent": intent})


class FederationSendRequest(BaseModel):
    recipient_handle: str
    sender_handle: str
    intent: str = "schedule_meeting"
    payload: Dict[str, Any] = Field(default_factory=dict)


@app.post("/v1/federation/send", dependencies=[Depends(require_token)])
async def federation_send(req: FederationSendRequest) -> Dict[str, Any]:
    """Отправка подписанного конверта удалённому агенту Aura (@user:domain)."""
    from . import federation

    return await federation.send_remote_envelope(
        remote_handle=req.recipient_handle,
        intent=req.intent,
        payload=req.payload,
        sender_handle=req.sender_handle,
    )


# ------------------------------------------------------------------- CalDAV / iCalendar
class CaldavSyncRequest(BaseModel):
    caldav_url: str = Field(min_length=5)
    username: str = ""
    password: str = ""
    user_key: str = ""


@app.post("/v1/calendar/sync_caldav", dependencies=[Depends(require_token)])
async def api_sync_caldav(request: CaldavSyncRequest) -> Dict[str, Any]:
    """Синхронизация внешнего календаря (iCloud, Google, Fastmail, Nextcloud) по CalDAV/ICS."""
    from . import caldav_sync

    events = await caldav_sync.fetch_caldav_events(
        caldav_url=request.caldav_url,
        username=request.username,
        password=request.password,
    )
    if request.user_key:
        agent = get_agent()
        for ev in events:
            text = f"Внешнее событие календаря: {ev.title} ({ev.start.isoformat()} — {ev.end.isoformat()})"
            agent.store.save(
                request.user_key,
                [MemoryEntry(kind="schedule", text=text, tags=["calendar", "caldav"])],
            )

    return {
        "ok": True,
        "count": len(events),
        "events": [ev.model_dump(mode="json") for ev in events],
    }


def _parse_dt(value: Any) -> Optional[datetime]:

    """ISO-время из запроса (всегда с поясом); мусор → None (берём «сейчас»)."""
    if not value:
        return None
    if isinstance(value, datetime):
        return as_aware(value)
    if not isinstance(value, str):
        return None
    try:
        return as_aware(datetime.fromisoformat(value))
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


@app.exception_handler(ValidationError)
async def invalid_data(_request, exc: ValidationError) -> JSONResponse:
    """Ошибка валидации внутри обработчика (не тела запроса) — 422 без деталей."""
    logger.info("validation error: %d errors", exc.error_count())
    return JSONResponse(status_code=422, content={"detail": "invalid input"})


@app.exception_handler(Exception)
async def unhandled(_request, exc: Exception) -> JSONResponse:
    # Текст исключения — только в лог: он может содержать DSN, пути и данные.
    logger.exception("unhandled error")
    return JSONResponse(status_code=500, content={"detail": "internal error"})


mount_preview()


def main() -> None:  # pragma: no cover
    import uvicorn

    settings = get_settings()
    uvicorn.run(app, host=settings.host, port=settings.port)


if __name__ == "__main__":  # pragma: no cover
    main()
