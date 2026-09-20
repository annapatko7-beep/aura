"""llm.py — доступ к языковой модели.

Два провайдера:
* ``MockLLM`` — детерминированный локальный планировщик (работает без ключа,
  используется тестами и офлайн-разработкой);
* ``OpenAILLM`` — любой OpenAI-совместимый HTTP-эндпоинт (chat/completions).

C++-сервер не знает, кто именно отвечает: он получает один и тот же JSON.
"""

from __future__ import annotations

import json
from typing import Any, Dict, List, Optional, Sequence

from . import planner
from .models import AgentContext, Preferences, utcnow

SYSTEM_PROMPT = (
    "Ты — Аура, персональный ИИ-агент пользователя. Ты общаешься с другими Аурами "
    "от имени пользователя. Отвечай строго валидным JSON без пояснений по схеме: "
    '{"reply": str, "intent": str, "confidence": float, '
    '"actions": [{"id": str, "tool": str, "args": {}}], '
    '"memory_updates": [{"kind": str, "text": str, "tags": [str]}], '
    '"a2a": {"target": str, "intent": str, "topic": str, "message": str} | null}'
)

TOOL_NAMES = (
    "find_cafe",
    "book_table",
    "send_message",
    "create_reminder",
    "create_note",
    "send_email",
    "check_calendar",
    "suggest_time",
)


class LLMError(RuntimeError):
    pass


class BaseLLM:
    name = "base"

    def complete_json(
        self,
        context: AgentContext,
        understanding: planner.Understanding,
        history: Optional[Sequence[Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:  # pragma: no cover - интерфейс
        raise NotImplementedError


class MockLLM(BaseLLM):
    """Локальная «модель»: правила вместо нейросети.

    Нужна не для красоты, а чтобы весь стек (C++-сервер → AI-сервис → действия)
    работал и тестировался без внешнего API.
    """

    name = "mock"

    def complete_json(
        self,
        context: AgentContext,
        understanding: planner.Understanding,
        history: Optional[Sequence[Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:
        intent = understanding.intent
        person = understanding.person
        topic = understanding.topic or "встреча"
        window = understanding.window
        when = window.label if window else "ближайшее время"
        actions: List[Dict[str, Any]] = []
        memory_updates: List[Dict[str, Any]] = []
        a2a: Optional[Dict[str, Any]] = None

        if intent == "schedule_meeting":
            actions = [
                {"id": "a1", "tool": "check_calendar", "args": {"window": window.as_dict() if window else None}},
                {"id": "a2", "tool": "suggest_time", "args": {"duration_minutes": understanding.duration_minutes}},
            ]
            if person:
                actions.append(
                    {
                        "id": "a3",
                        "tool": "send_message",
                        "args": {"to": person, "text": f"Аура предлагает встретиться: {topic} ({when})."},
                    }
                )
                a2a = {
                    "target": person,
                    "intent": "schedule_meeting",
                    "topic": topic,
                    "message": f"Привет! Моя Аура предлагает обсудить «{topic}» — {when}.",
                }
            reply = f"Ищу окно в графиках на {when} и подбираю место под ваши предпочтения."
            confidence = 0.86
        elif intent == "reminder":
            actions = [
                {
                    "id": "a1",
                    "tool": "create_reminder",
                    "args": {"text": context.message, "at": window.start.isoformat() if window else None},
                }
            ]
            reply = f"Напомню {when}."
            confidence = 0.9
        elif intent == "note":
            actions = [{"id": "a1", "tool": "create_note", "args": {"text": context.message}}]
            reply = "Записал в заметки."
            confidence = 0.88
        elif intent == "email":
            actions = [
                {
                    "id": "a1",
                    "tool": "send_email",
                    "args": {"to": person or context.email, "subject": topic.title(), "body": context.message},
                }
            ]
            reply = "Черновик письма готов и отправлен."
            confidence = 0.8
        elif intent == "message":
            actions = [
                {"id": "a1", "tool": "send_message", "args": {"to": person or "self", "text": context.message}}
            ]
            reply = "Сообщение отправлено."
            confidence = 0.82
        elif intent == "find_place":
            actions = [
                {
                    "id": "a1",
                    "tool": "find_cafe",
                    "args": {"city": context.preferences.city, "diet": context.preferences.diet},
                }
            ]
            reply = "Подобрал несколько мест под твои предпочтения."
            confidence = 0.79
        elif intent == "memory":
            memory_updates = [{"kind": "fact", "text": context.message, "tags": ["profile"]}]
            reply = "Запомнил."
            confidence = 0.93
        else:
            reply = "Я на связи: могу договориться о встрече, напомнить, записать заметку или написать письмо."
            confidence = 0.55

        if understanding.topic:
            memory_updates.append(
                {"kind": "fact", "text": f"Интересует тема: {understanding.topic}", "tags": ["interest"]}
            )

        return {
            "reply": reply,
            "intent": intent,
            "confidence": confidence,
            "actions": actions,
            "memory_updates": memory_updates,
            "a2a": a2a,
        }


class OpenAILLM(BaseLLM):
    """OpenAI-совместимый провайдер (chat/completions + JSON mode)."""

    name = "openai"

    def __init__(self, api_key: str, base_url: str, model: str, timeout: float, temperature: float):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout
        self.temperature = temperature

    def _user_prompt(self, context: AgentContext, understanding: planner.Understanding) -> str:
        return json.dumps(
            {
                "message": context.message,
                "understanding": understanding.as_dict(),
                "preferences": context.preferences.model_dump(),
                "memory": [m.model_dump(mode="json") for m in context.memory[:20]],
                "recent": list(context.history[-10:]),
                "tools": list(TOOL_NAMES),
            },
            ensure_ascii=False,
            default=str,
        )

    def complete_json(
        self,
        context: AgentContext,
        understanding: planner.Understanding,
        history: Optional[Sequence[Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:
        import httpx  # импорт здесь, чтобы офлайн-режим не требовал библиотеку

        payload = {
            "model": self.model,
            "temperature": self.temperature,
            "response_format": {"type": "json_object"},
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": self._user_prompt(context, understanding)},
            ],
        }
        try:
            response = httpx.post(
                f"{self.base_url}/chat/completions",
                headers={"Authorization": f"Bearer {self.api_key}"},
                json=payload,
                timeout=self.timeout,
            )
            response.raise_for_status()
            content = response.json()["choices"][0]["message"]["content"]
        except Exception as exc:  # сеть/лимиты — не роняем сервер, уходим в правила
            raise LLMError(f"LLM request failed: {exc}") from exc

        try:
            return json.loads(content)
        except json.JSONDecodeError as exc:
            raise LLMError("LLM returned non-JSON payload") from exc


def build_llm(settings: Any) -> BaseLLM:
    """Фабрика провайдера по настройкам."""
    if settings.llm_provider == "openai" or (
        settings.llm_provider == "auto" and settings.llm_api_key
    ):
        return OpenAILLM(
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            model=settings.llm_model,
            timeout=settings.llm_timeout,
            temperature=settings.llm_temperature,
        )
    return MockLLM()


def normalize_actions(raw: Any) -> List[Dict[str, Any]]:
    """Приводит действия модели к безопасному виду (только известные инструменты)."""
    if not isinstance(raw, list):
        return []
    cleaned: List[Dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            continue
        tool = str(item.get("tool", "")).strip()
        if tool not in TOOL_NAMES:
            continue
        args = item.get("args")
        cleaned.append(
            {
                "id": str(item.get("id") or f"a{index + 1}"),
                "tool": tool,
                "args": args if isinstance(args, dict) else {},
            }
        )
    return cleaned


def now_iso() -> str:
    return utcnow().isoformat()


def build_prompt(context: AgentContext, prefs: Optional[Preferences] = None) -> str:
    """Публичная сборка промпта — используется и MockLLM, и отладочным роутом."""
    return json.dumps(
        {
            "system": SYSTEM_PROMPT,
            "message": context.message,
            "preferences": (prefs or context.preferences).model_dump(),
        },
        ensure_ascii=False,
    )
