"""agent.py — ядро Ауры.

Принимает запрос от C++-сервера (AgentManager), вызывает LLM, при необходимости
договаривается с чужой Аурой и возвращает JSON с действиями.

Порядок работы ``run()``:
1. разбор фразы (planner.understand) → намерение, окно, длительность, персона;
2. вызов LLM (с откатом на локальные правила, если модель недоступна);
3. подгрузка долговременной памяти и её ранжирование под запрос;
4. нормализация действий модели (только известные инструменты);
5. выполнение действий через ToolManager (или dry-run);
6. Agent-to-Agent: переговоры с Аурой собеседника, подбор слота и места;
7. обновление памяти фактами из диалога.
"""

from __future__ import annotations

import logging
from datetime import timedelta
from typing import Any, Dict, List, Optional

from . import memory as memory_module
from . import planner
from .config import Settings, get_settings
from .llm import BaseLLM, LLMError, MockLLM, build_llm, normalize_actions
from .models import (
    Step,
    A2AProposal,
    AgentContext,
    AgentRequest,
    AgentResponse,
    CalendarEvent,
    MemoryEntry,
    NegotiateRequest,
    NegotiateResponse,
    Plan,
    Preferences,
    Slot,
    ToolCall,
    ToolResult,
    utcnow,
)
from .tools import ToolBackends, ToolError, ToolManager

logger = logging.getLogger("aura.agent")


class AuraAgent:
    def __init__(
        self,
        settings: Optional[Settings] = None,
        llm: Optional[BaseLLM] = None,
        store: Optional[memory_module.MemoryStore] = None,
        tools: Optional[ToolManager] = None,
    ):
        self.settings = settings or get_settings()
        self.llm = llm or build_llm(self.settings)
        self.fallback_llm = MockLLM()
        self.store = store or memory_module.build_store(self.settings.memory_backend)
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
        understanding = planner.understand(context.message, now)

        payload = self._complete(context, understanding)
        intent = str(payload.get("intent") or understanding.intent)
        reply = str(payload.get("reply") or "")
        confidence = float(payload.get("confidence") or 0.5)

        relevant_memory = memory_module.load_context_memory(self.store, context)
        actions = [ToolCall(**item) for item in normalize_actions(payload.get("actions"))]
        results: List[ToolResult] = []

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
            llm=self.llm.name,
        )

    def _complete(self, context: AgentContext, understanding: planner.Understanding) -> Dict[str, Any]:
        try:
            return self.llm.complete_json(context, understanding)
        except LLMError as exc:
            logger.warning("LLM unavailable (%s), falling back to local rules", exc)
            return self.fallback_llm.complete_json(context, understanding)

    def _execute(self, actions: List[ToolCall], context: AgentContext) -> List[ToolResult]:
        results: List[ToolResult] = []
        for call in actions:
            try:
                data = self.tools.run(call.tool, call.args, context)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=True, data=data))
            except ToolError as exc:
                logger.info("tool %s failed: %s", call.tool, exc)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=False, error=str(exc)))
            except Exception as exc:  # внешний API упал — агент продолжает работать
                logger.exception("tool %s crashed", call.tool)
                results.append(ToolResult(id=call.id, tool=call.tool, ok=False, error=f"internal: {exc}"))
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
                message=str(raw.get("message") or "Аура предлагает встретиться."),
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
                f"Мы с Аурой {peer.display_name or peer.email} договорились: "
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
        return f"{reply} Отправил предложение Ауре {proposal.target}."

    # ------------------------------------------------------------- negotiate
    def negotiate(self, request: NegotiateRequest) -> NegotiateResponse:
        """Эндпоинт A2A: Аура-инициатор спрашивает Ауру-респондента."""
        start = utcnow()
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
        place = self._place(request.initiator, request.responder)

        if not shared:
            return NegotiateResponse(
                accepted=False,
                place=place,
                counter_slots=responder_slots[:3],
                message="Общих окон нет, вот мои ближайшие свободные слоты.",
            )

        best = shared[0]
        return NegotiateResponse(
            accepted=True,
            slot=best,
            place=place,
            message=(
                f"Да, {best.start.strftime('%d.%m в %H:%M')} подходит. "
                f"Предлагаю {place.get('name', 'место на полпути')}."
            ),
            counter_slots=shared[1:4],
        )


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
            return AgentContext(
                display_name=str(contact.get("name", "")),
                email=str(contact.get("email", "")),
                preferences=Preferences(
                    diet=[str(d) for d in contact.get("diet", [])],
                    city=str(contact.get("city", "")),
                    lat=contact.get("lat"),
                    lon=contact.get("lon"),
                ),
                calendar=[CalendarEvent(**event) for event in contact.get("calendar", [])],
            )
    return None


def build_agent(settings: Optional[Settings] = None) -> AuraAgent:
    return AuraAgent(settings)


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
