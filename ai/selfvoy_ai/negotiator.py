"""negotiator.py — многораундовый движок переговоров Аур (Multi-Turn A2A Negotiation Engine).

Реализует асинхронный конечный автомат (State Machine) согласования времени,
места и условий встречи между двумя ИИ-двойниками.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Any, Dict, List, Optional, Sequence

from . import planner
from .models import AgentContext, Slot, utcnow

logger = logging.getLogger("selfvoy.negotiator")

MAX_NEGOTIATION_ROUNDS = 5


@dataclass
class NegotiationSession:
    id: str
    topic: str
    initiator_email: str
    responder_email: str
    duration_minutes: int = 60
    window_hours: int = 96
    round_number: int = 1
    status: str = "PROPOSED"  # PROPOSED | COUNTER_OFFER | CONVERGING | AGREED | REJECTED
    selected_slot: Optional[Slot] = None
    selected_place: Optional[Dict[str, Any]] = None
    counter_slots: List[Slot] = field(default_factory=list)
    history: List[Dict[str, Any]] = field(default_factory=list)
    message: str = ""

    def as_dict(self) -> Dict[str, Any]:
        return {
            "id": self.id,
            "topic": self.topic,
            "initiator_email": self.initiator_email,
            "responder_email": self.responder_email,
            "duration_minutes": self.duration_minutes,
            "window_hours": self.window_hours,
            "round_number": self.round_number,
            "status": self.status,
            "accepted": self.status == "AGREED",
            "selected_slot": self.selected_slot.model_dump(mode="json") if self.selected_slot else None,
            "selected_place": self.selected_place or {},
            "counter_slots": [s.model_dump(mode="json") for s in self.counter_slots],
            "history": self.history,
            "message": self.message,
        }


def run_negotiation_round(
    session: NegotiationSession,
    initiator: AgentContext,
    responder: AgentContext,
    candidate_place: Optional[Dict[str, Any]] = None,
) -> NegotiationSession:
    """Выполняет раунд переговоров между двумя Агент-двойниками."""
    now = utcnow()
    start = now
    end = start + timedelta(hours=max(1, session.window_hours))
    duration = max(15, session.duration_minutes)

    window = planner.TimeWindow(start=start, end=end, label=f"раунд {session.round_number}")

    init_slots = planner.free_slots(
        window,
        initiator.calendar,
        initiator.preferences,
        duration_minutes=duration,
        max_slots=8,
    )
    resp_slots = planner.free_slots(
        window,
        responder.calendar,
        responder.preferences,
        duration_minutes=duration,
        max_slots=8,
    )

    shared = planner.intersect_slots(init_slots, resp_slots, duration_minutes=duration)
    place = candidate_place or {}

    # Раунд 1: Прямое пересечение
    if shared:
        best_slot = shared[0]
        session.status = "AGREED"
        session.selected_slot = best_slot
        session.selected_place = place
        session.counter_slots = shared[1:4]
        date_str = best_slot.start.strftime("%d.%m в %H:%M")
        place_name = place.get("name", "место согласовано")
        session.message = f"Договорились: {date_str}, {place_name}."
        session.history.append({
            "round": session.round_number,
            "action": "agree",
            "slot": best_slot.model_dump(mode="json"),
            "note": "найдено прямое пересечение графиков",
        })
        return session

    # Раунд 2-3: Поиск гибкого компромисса (сдвиг длительности или мягкие окна)
    if session.round_number < MAX_NEGOTIATION_ROUNDS:
        # Пробуем проверить встречные слоты респондента против инициатора
        shorter_duration = max(30, duration - 15)
        if shorter_duration < duration:
            flexible_init = planner.free_slots(window, initiator.calendar, initiator.preferences, duration_minutes=shorter_duration)
            flexible_resp = planner.free_slots(window, responder.calendar, responder.preferences, duration_minutes=shorter_duration)
            flex_shared = planner.intersect_slots(flexible_init, flexible_resp, duration_minutes=shorter_duration)
            if flex_shared:
                best_slot = flex_shared[0]
                session.status = "CONVERGING"
                session.selected_slot = best_slot
                session.selected_place = place
                session.counter_slots = flex_shared
                session.round_number += 1
                session.message = f"Нашли компромиссный слот на {shorter_duration} мин: {best_slot.start.strftime('%d.%m в %H:%M')}."
                session.history.append({
                    "round": session.round_number,
                    "action": "compromise_duration",
                    "slot": best_slot.model_dump(mode="json"),
                    "duration": shorter_duration,
                })
                return session

        # Если сдвиг не помог — предлагаем лучшие встречные слоты респондента
        session.status = "COUNTER_OFFER"
        session.counter_slots = resp_slots[:3]
        session.round_number += 1
        session.history.append({
            "round": session.round_number - 1,
            "action": "counter_offer",
            "slots": [s.model_dump(mode="json") for s in resp_slots[:3]],
        })
        session.message = "Прямых пересечений нет. Аура собеседника предложила встречные свободные слоты."
        return session

    # Превышен лимит раундов: эскалация человеку
    session.status = "REJECTED"
    session.message = "Не удалось автоматически найти общее окно за 5 раундов. Требуется выбор пользователя."
    session.history.append({"round": session.round_number, "action": "escalate_to_human"})
    return session
