"""Контракты данных между C++-сервером и Python AI Service.

C++-сервер (AgentManager / MemoryManager) общается с сервисом по HTTP и
ожидает ровно эти JSON-структуры.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from pydantic import BaseModel, Field


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class CalendarEvent(BaseModel):
    """Занятой слот пользователя (приходит из UserMemory/календаря C++-сервера)."""

    title: str = "event"
    start: datetime
    end: datetime


class Preferences(BaseModel):
    """Срез UserPreferences."""

    diet: List[str] = Field(default_factory=list)
    transport: str = "walk"
    preferred_hours: List[int] = Field(default_factory=lambda: [10, 11, 12, 16, 17, 18, 19])
    budget_limit: float = 0.0
    city: str = ""
    work_hours: List[int] = Field(default_factory=lambda: [9, 10, 11, 12, 13, 14, 15, 16, 17, 18])
    # Домашняя точка — нужна, чтобы искать место «на полпути» между двумя Аурами
    lat: Optional[float] = None
    lon: Optional[float] = None


class MemoryEntry(BaseModel):
    kind: str = "fact"  # fact | preference | schedule | contact
    text: str
    weight: float = 1.0
    tags: List[str] = Field(default_factory=list)
    updated_at: datetime = Field(default_factory=utcnow)


class AgentContext(BaseModel):
    """Всё, что C++-сервер знает о пользователе на момент запроса."""

    user_id: int = 0
    display_name: str = ""
    email: str = ""
    now: Optional[datetime] = None
    timezone: str = "Europe/Moscow"
    message: str = ""
    history: List[Dict[str, Any]] = Field(default_factory=list)
    memory: List[MemoryEntry] = Field(default_factory=list)
    preferences: Preferences = Field(default_factory=Preferences)
    calendar: List[CalendarEvent] = Field(default_factory=list)
    contacts: List[Dict[str, Any]] = Field(default_factory=list)


class ToolCall(BaseModel):
    id: str = "a1"
    tool: str
    args: Dict[str, Any] = Field(default_factory=dict)


class ToolResult(BaseModel):
    id: str = "a1"
    tool: str
    ok: bool = True
    data: Dict[str, Any] = Field(default_factory=dict)
    error: str = ""


class Step(BaseModel):
    title: str
    detail: str = ""
    tool: Optional[str] = None
    at: Optional[datetime] = None


class Plan(BaseModel):
    goal: str = ""
    intent: str = "chat"
    steps: List[Step] = Field(default_factory=list)
    constraints: Dict[str, Any] = Field(default_factory=dict)


class Slot(BaseModel):
    start: datetime
    end: datetime
    score: float = 0.0
    reason: str = ""


class A2AProposal(BaseModel):
    """Предложение, которое Аура отправляет Ауре другого человека."""

    target: str = ""
    intent: str = "schedule_meeting"
    topic: str = ""
    slots: List[Slot] = Field(default_factory=list)
    place: Dict[str, Any] = Field(default_factory=dict)
    message: str = ""


class AgentRequest(BaseModel):
    context: AgentContext = Field(default_factory=AgentContext)
    execute: bool = True
    dry_run: bool = False
    peers: List[AgentContext] = Field(default_factory=list)


class NegotiateRequest(BaseModel):
    """Запрос Agent-to-Agent: одна Аура спрашивает другую."""

    initiator: AgentContext
    responder: AgentContext
    intent: str = "schedule_meeting"
    topic: str = ""
    duration_minutes: int = 60
    window_hours: int = 96


class NegotiateResponse(BaseModel):
    accepted: bool = False
    slot: Optional[Slot] = None
    place: Dict[str, Any] = Field(default_factory=dict)
    message: str = ""
    counter_slots: List[Slot] = Field(default_factory=list)


class AgentResponse(BaseModel):
    reply: str = ""
    intent: str = "chat"
    confidence: float = 0.5
    plan: Plan = Field(default_factory=Plan)
    actions: List[ToolCall] = Field(default_factory=list)
    results: List[ToolResult] = Field(default_factory=list)
    memory_updates: List[MemoryEntry] = Field(default_factory=list)
    a2a: Optional[A2AProposal] = None
    llm: str = "mock"
