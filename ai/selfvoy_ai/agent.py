"""agent.py — ядро Selfvoy.

Принимает запрос от C++-сервера (AgentManager), вызывает LLM, при необходимости
договаривается с чужим Selfvoy и возвращает JSON с действиями.

Порядок работы ``run()``:
1. разбор фразы (planner.understand) → намерение, окно, длительность, персона;
2. вызов LLM (с откатом на локальные правила, если модель недоступна);
3. подгрузка долговременной памяти и её ранжирование под запрос;
4. нормализация действий модели (только известные инструменты);
5. выполнение действий через ToolManager (или dry-run);
6. Agent-to-Agent: переговоры с Selfvoy собеседника, подбор слота и места;
7. обновление памяти фактами из диалога.
"""

from __future__ import annotations

import logging
import secrets
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError
from typing import Any, Dict, List, Optional, Tuple


from . import memory as memory_module
from . import planner
from .config import Settings, get_settings
from pydantic import ValidationError

from .llm import BaseLLM, LLMError, MockLLM, build_llm, sanitize_payload
from .models import (
    Step,
    A2AProposal,
    AgentContext,
    AgentRequest,
    AgentResponse,
    MemoryEntry,
    NegotiateRequest,
    NegotiateResponse,
    Plan,
    Preferences,
    Slot,
    SlotsRequest,
    SlotsResponse,
    ToolCall,
    ToolResult,
    utcnow,
)
from .tools import SERVER_ONLY_TOOLS, ToolBackends, ToolError, ToolManager

logger = logging.getLogger("selfvoy.agent")


class SelfvoyAgent:
    def __init__(
        self,
        settings: Optional[Settings] = None,
        llm: Optional[BaseLLM] = None,
        store: Optional[memory_module.MemoryStore] = None,
        tools: Optional[ToolManager] = None,
        embed_client: Optional[memory_module.EmbeddingClient] = None,
    ):
        self.settings = settings or get_settings()
        self.llm = llm or build_llm(self.settings)
        self.fallback_llm = MockLLM()
        self.store = store or memory_module.build_store(
            self.settings.memory_backend, self.settings.database_url
        )
        # Гибридный RAG: семантический клиент включается только в hybrid-режиме
        # с заданным эндпоинтом эмбеддингов; иначе ранжирование лексическое.
        if embed_client is not None:
            self.embed_client = embed_client
        elif self.settings.rag_is_hybrid and self.settings.embeddings_url:
            self.embed_client = memory_module.EmbeddingClient(
                url=self.settings.embeddings_url,
                model=self.settings.embeddings_model,
                api_key=self.settings.embeddings_api_key,
                timeout=self.settings.embeddings_timeout,
            )
        else:
            self.embed_client = None
        self.tools = tools or ToolManager(
            ToolBackends(
                mode=self.settings.tools_backend,
                urls={
                    "maps": self.settings.maps_api_url,
                    "email": self.settings.email_api_url,
                    "calendar": self.settings.calendar_api_url,
                    "booking": self.settings.booking_api_url,
                },
            )
        )

    # ------------------------------------------------------------------ run
    def run(self, request: AgentRequest) -> AgentResponse:
        context = request.context
        now = context.now or utcnow()
        # «завтра в 10» — по часам пользователя, а не по UTC.
        understanding = planner.understand(context.message, _local(now, context.timezone))

        # payload уже проверен (sanitize_payload): типы, пределы, виды памяти.
        payload, used_llm = self._complete(context, understanding)
        intent = payload["intent"] or understanding.intent
        reply = payload["reply"]
        confidence = payload["confidence"]

        relevant_memory = memory_module.load_context_memory(
            self.store, context, embed_client=self.embed_client
        )
        actions = [ToolCall(**item) for item in payload["actions"]]
        results: List[ToolResult] = []

        # Исполнять действия AI-сервис может только по явной просьбе
        # (execute=true без dry_run); внешние действия — никогда (см. _execute).
        if actions and request.execute and not request.dry_run:
            results = self._execute(actions, context)

        plan = self._plan(intent, understanding, actions, reply)

        proposal: Optional[A2AProposal] = None
        raw_a2a = payload.get("a2a")
        if isinstance(raw_a2a, dict) and raw_a2a.get("target"):
            proposal = self._negotiate_for(context, raw_a2a, understanding, request)
            if proposal is not None:
                reply = self._compose_reply(reply, proposal)

        updates = memory_module.extract_from_context(context, payload)
        if updates and request.execute and not request.dry_run:
            self.store.save(memory_module.user_key(context), updates)

        return AgentResponse(
            reply=reply,
            intent=intent,
            confidence=round(min(1.0, confidence + (0.05 if relevant_memory else 0.0)), 3),
            plan=plan,
            actions=actions,
            results=results,
            memory_updates=updates,
            a2a=proposal,
            llm=used_llm,
        )

    def _complete(
        self, context: AgentContext, understanding: planner.Understanding
    ) -> Tuple[Dict[str, Any], str]:
        """Ответ модели, прошедший проверку; при сбое/мусоре — локальные правила.

        Второй элемент — кто на самом деле ответил: имя модели или
        «<модель>-fallback», если сработали правила (в ответе поле ``llm``:
        по нему видно, что настоящая LLM не ответила — ключ, сеть, лимиты).
        """
        try:
            return sanitize_payload(self.llm.complete_json(context, understanding)), self.llm.name
        except LLMError as exc:
            logger.warning("LLM unavailable or invalid (%s), falling back to local rules", exc)
            fallback = sanitize_payload(self.fallback_llm.complete_json(context, understanding))
            return fallback, f"{self.llm.name}-fallback" if self.llm.name != self.fallback_llm.name else self.llm.name

    def _execute(self, actions: List[ToolCall], context: AgentContext) -> List[ToolResult]:
        results: List[ToolResult] = []
        for call in actions:
            if call.tool in SERVER_ONLY_TOOLS:
                # Внешние действия (письма, сообщения, брони) исполняет только
                # C++-сервер: там tool_permissions и подтверждение пользователя.
                results.append(
                    ToolResult(
                        id=call.id,
                        tool=call.tool,
                        ok=False,
                        error="server_only: действие исполняет сервер после проверки разрешений",
                    )
                )
                continue
            try:
                data = self.tools.run(call.tool, call.args, context)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=True, data=data))
            except ToolError as exc:
                logger.info("tool %s failed: %s", call.tool, exc)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=False, error=str(exc)))
            except Exception:  # внешний API упал — агент продолжает работать
                logger.exception("tool %s crashed", call.tool)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=False, error="internal error"))
        return results

    def _plan(
        self,
        intent: str,
        understanding: planner.Understanding,
        actions: List[ToolCall],
        reply: str,
    ) -> Plan:
        steps = [
            Step(
                title="Понял задачу",
                detail=understanding.text,
            )
        ]
        if understanding.window:
            steps.append(
                Step(
                    title=f"Окно: {understanding.window.label}",
                    detail=f"{understanding.window.start.isoformat()} → {understanding.window.end.isoformat()}",
                    at=understanding.window.start,
                )
            )
        for call in actions:
            steps.append(
                Step(
                    title=f"Действие: {call.tool}",
                    detail=", ".join(f"{k}={v}" for k, v in call.args.items() if not isinstance(v, dict)),
                    tool=call.tool,
                    at=understanding.window.start if understanding.window else None,
                )
            )
        steps.append(Step(title="Ответ пользователю", detail=reply))
        return Plan(
            goal=understanding.topic or intent,
            intent=intent,
            steps=steps,
            constraints={
                "duration_minutes": understanding.duration_minutes,
                "person": understanding.person,
                "window": understanding.window.as_dict() if understanding.window else None,
            },
        )

    # ----------------------------------------------------------- Agent2Agent
    def _negotiate_for(
        self,
        context: AgentContext,
        raw: Dict[str, Any],
        understanding: planner.Understanding,
        request: AgentRequest,
    ) -> Optional[A2AProposal]:
        peer = _find_peer(raw.get("target"), request.peers, context)
        if peer is None:
            # Собеседник не в контексте: отправляем намерение, сервер доставит его
            return A2AProposal(
                target=str(raw.get("target")),
                intent=str(raw.get("intent") or "schedule_meeting"),
                topic=str(raw.get("topic") or understanding.topic or "встреча"),
                slots=[],
                message=str(raw.get("message") or "Selfvoy предлагает встретиться."),
            )

        window_hours = 96
        start = understanding.window.start if understanding.window else utcnow()
        end = understanding.window.end if understanding.window else start + timedelta(hours=window_hours)
        duration = understanding.duration_minutes

        mine = planner.free_slots(
            planner.TimeWindow(start=start, end=end, label="окно запроса"),
            context.calendar,
            context.preferences,
            duration_minutes=duration,
        )
        theirs = planner.free_slots(
            planner.TimeWindow(start=start, end=end, label="окно запроса"),
            peer.calendar,
            peer.preferences,
            duration_minutes=duration,
        )
        shared = planner.intersect_slots(mine, theirs, duration_minutes=duration)
        place = self._place(context, peer)

        if not shared:
            return A2AProposal(
                target=peer.email or peer.display_name,
                intent="schedule_meeting",
                topic=str(raw.get("topic") or understanding.topic or "встреча"),
                slots=mine[:3],
                place=place,
                message="Общих окон пока нет — вот мои свободные слоты, выбери удобный.",
            )

        best = shared[0]
        return A2AProposal(
            target=peer.email or peer.display_name,
            intent="schedule_meeting",
            topic=str(raw.get("topic") or understanding.topic or "встреча"),
            slots=shared[:3],
            place=place,
            message=(
                f"Мы с Selfvoy {peer.display_name or peer.email} договорились: "
                f"{best.start.isoformat()}, {place.get('name', 'место не выбрано')}."
            ),
        )

    def _place(self, mine: AgentContext, peer: Optional[AgentContext]) -> Dict[str, Any]:
        midpoint = None
        if peer and None not in (mine.preferences.lat, mine.preferences.lon, peer.preferences.lat, peer.preferences.lon):
            midpoint = {
                "lat": (mine.preferences.lat + peer.preferences.lat) / 2.0,
                "lon": (mine.preferences.lon + peer.preferences.lon) / 2.0,
            }
        diet = sorted(set(mine.preferences.diet) | set(peer.preferences.diet if peer else set()))
        result = self.tools.run(
            "find_cafe",
            {"diet": diet, "midpoint": midpoint, "city": mine.preferences.city},
            mine,
        )
        results = result.get("results") or []
        return results[0] if results else {}

    def _compose_reply(self, reply: str, proposal: A2AProposal) -> str:
        if proposal.slots:
            slot = proposal.slots[0]
            return f"{reply} Договорился на {slot.start.strftime('%d.%m %H:%M')} — {proposal.place.get('name', 'место уточняется')}."
        return f"{reply} Отправил предложение Selfvoy {proposal.target}."

    # ------------------------------------------------------------- negotiate
    def negotiate(self, request: NegotiateRequest) -> NegotiateResponse:
        """Эндпоинт A2A: Selfvoy-инициатор спрашивает Selfvoy-респондента."""
        start = utcnow()
        # Пределы window_hours/duration_minutes проверены моделью (422 вместо 500).
        end = start + timedelta(hours=max(1, request.window_hours))
        duration = max(15, request.duration_minutes)

        initiator_slots = planner.free_slots(
            planner.TimeWindow(start=start, end=end, label="окно переговоров"),
            request.initiator.calendar,
            request.initiator.preferences,
            duration_minutes=duration,
            max_slots=8,
        )
        responder_slots = planner.free_slots(
            planner.TimeWindow(start=start, end=end, label="окно переговоров"),
            request.responder.calendar,
            request.responder.preferences,
            duration_minutes=duration,
            max_slots=8,
        )
        shared = planner.intersect_slots(initiator_slots, responder_slots, duration_minutes=duration)
        place = self._place(request.initiator, request.responder) if request.choose_place else {}

        if not shared:
            # Если разрешен многораундовый торг, запускаем сессию сближения предпочтений
            if request.multi_turn:
                from . import negotiator

                session_id = request.session_id or f"neg-{secrets.token_hex(6)}"
                session = negotiator.NegotiationSession(
                    id=session_id,
                    topic=request.topic,
                    initiator_email=request.initiator.email or request.initiator.display_name,
                    responder_email=request.responder.email or request.responder.display_name,
                    duration_minutes=request.duration_minutes,
                    window_hours=request.window_hours,
                    round_number=request.round_number,
                    selected_place=place,
                )
                session = negotiator.run_negotiation_round(
                    session=session,
                    initiator=request.initiator,
                    responder=request.responder,
                    candidate_place=place,
                )
                return NegotiateResponse(
                    accepted=(session.status == "AGREED"),
                    status=session.status,
                    slot=session.selected_slot,
                    place=place,
                    message=session.message,
                    counter_slots=session.counter_slots,
                    rounds_log=session.history,
                    session_id=session.id,
                )

            return NegotiateResponse(
                accepted=False,
                status="REJECTED",
                place=place,
                counter_slots=responder_slots[:3],
                message="Общих окон нет, вот мои ближайшие свободные слоты.",
            )

        best = shared[0]
        return NegotiateResponse(
            accepted=True,
            status="AGREED",
            slot=best,
            place=place,
            message=(
                f"Да, {best.start.strftime('%d.%m в %H:%M')} подходит. "
                + (f"Предлагаю {place.get('name', 'место на полпути')}." if request.choose_place
                   else "Место выберете сами.")
            ),
            counter_slots=shared[1:4],
            rounds_log=[{
                "round": 1,
                "action": "agree",
                "slot": best.model_dump(mode="json"),
                "note": "прямое пересечение графиков",
            }],
            session_id=request.session_id or f"neg-{secrets.token_hex(6)}",
        )



    # ---------------------------------------------------------------- slots
    def offer_slots(self, request: SlotsRequest) -> SlotsResponse:
        """Слоты владельца для приглашения по ссылке и страницы записи (гость без Selfvoy).

        «Сейчас» — реальное время, но не раньше context.now: так слоты никогда
        не окажутся в прошлом, даже если сервер прислал устаревший now.
        """
        context = request.context
        now = utcnow()
        if context.now is not None and context.now > now:
            now = context.now
        # Пределы window_hours/duration_minutes/max_slots проверены моделью (422).
        window = planner.TimeWindow(
            start=now, end=now + timedelta(hours=request.window_hours), label="окно приглашения"
        )
        slots = planner.offer_slots(
            window,
            context.calendar,
            context.preferences,
            context.timezone,
            duration_minutes=request.duration_minutes,
            max_slots=request.max_slots,
            per_day=request.per_day,
            now=now,
            lead=timedelta(minutes=request.lead_minutes),
            weekends=request.weekends,
        )
        return SlotsResponse(slots=slots)


def _local(now: datetime, tz_name: str) -> datetime:
    """Момент now в часовом поясе пользователя; неизвестный пояс — без изменений."""
    try:
        zone = ZoneInfo(tz_name or "UTC")
    except (ZoneInfoNotFoundError, ValueError):
        return now
    return planner._as_aware(now).astimezone(zone)


def _normalize_name(name: Any) -> str:
    """Грубая нормализация русского имени: 'Аней' и 'Аня' дают один ключ.

    C++-сервер передаёт контакты уже разобранными, но фраза пользователя может
    содержать имя в косвенном падеже — поэтому сначала пробуем точное
    совпадение, потом ключ без типового окончания.
    """
    raw = str(name or "").strip().lower()
    if not raw:
        return ""
    for suffix in ("ией", "ей", "ой", "ий", "ем", "ом", "ам", "ям", "у", "ю", "а", "я", "ы", "и"):
        if len(raw) > len(suffix) + 1 and raw.endswith(suffix):
            return raw[: -len(suffix)]
    return raw


def _find_peer(
    target: Any, peers: List[AgentContext], context: AgentContext
) -> Optional[AgentContext]:
    """Ищем контекст собеседника: по email, по имени или по списку контактов."""
    if not target:
        return None
    needle = str(target).strip().lower()
    key = _normalize_name(needle)
    for peer in peers:
        candidates = {(peer.email or "").lower(), (peer.display_name or "").lower()}
        if needle in candidates:
            return peer
    for peer in peers:
        if key and _normalize_name(peer.display_name) == key:
            return peer
        if needle and needle in (peer.email or "").lower():
            return peer
    for contact in context.contacts:
        contact_name = str(contact.get("name", "")).lower()
        contact_email = str(contact.get("email", "")).lower()
        if needle in (contact_name, contact_email) or (key and _normalize_name(contact_name) == key):
            return _contact_context(contact)
    return None


def _contact_context(contact: Dict[str, Any]) -> Optional[AgentContext]:
    """Контекст собеседника из записи контакта; мусор в полях не роняет запрос."""
    diet = contact.get("diet")
    calendar = contact.get("calendar")
    try:
        return AgentContext(
            display_name=str(contact.get("name", "")),
            email=str(contact.get("email", "")),
            preferences=Preferences(
                diet=[str(d) for d in diet] if isinstance(diet, list) else [],
                city=str(contact.get("city", "")),
                lat=contact.get("lat"),
                lon=contact.get("lon"),
            ),
            # Calendar сам пропускает записи без корректных start/end.
            calendar=calendar if isinstance(calendar, list) else [],
        )
    except ValidationError as exc:
        logger.info("контакт пропущен: некорректные данные (%s)", exc.error_count())
        return None


def build_agent(settings: Optional[Settings] = None) -> SelfvoyAgent:
    return SelfvoyAgent(settings)


def memory_entries(payload: Any) -> List[MemoryEntry]:
    """Служебный хелпер: привести сырые memory_updates к моделям."""
    if not isinstance(payload, list):
        return []
    entries: List[MemoryEntry] = []
    for item in payload:
        if isinstance(item, MemoryEntry):
            entries.append(item)
        elif isinstance(item, dict):
            entries.append(MemoryEntry(**item))
    return entries


def slot_from_dict(payload: Dict[str, Any]) -> Slot:
    return Slot(**payload)
