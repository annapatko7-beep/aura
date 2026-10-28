"""llm.py — доступ к языковой модели.

Два провайдера:
* ``MockLLM`` — детерминированный локальный планировщик (работает без ключа,
  используется тестами и офлайн-разработкой);
* ``OpenAILLM`` — любой OpenAI-совместимый HTTP-эндпоинт (chat/completions).

C++-сервер не знает, кто именно отвечает: он получает один и тот же JSON.
"""

from __future__ import annotations

import json
import math
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Sequence

from . import planner
from .models import MEMORY_KINDS, AgentContext, Preferences, utcnow

SYSTEM_PROMPT = (
    "Ты — Selfvoy, персональный ИИ-агент пользователя. Ты общаешься с агентами Selfvoy "
    "других людей от имени пользователя. Отвечай строго валидным JSON без пояснений по схеме: "
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


_WEEKDAY_ACCUSATIVE = {
    "понедельник": "понедельник", "вторник": "вторник", "среда": "среду", "четверг": "четверг",
    "пятница": "пятницу", "суббота": "субботу", "воскресенье": "воскресенье",
}


def _when_phrase(label: str, fire: datetime) -> str:
    """«завтра в 10:00», «в пятницу в 9:00», «14.10 в 18:30»."""
    clock = f"{fire.hour}:{fire.minute:02d}"
    if label in ("сегодня", "завтра", "послезавтра"):
        return f"{label} в {clock}"
    if label in _WEEKDAY_ACCUSATIVE:
        return f"в {_WEEKDAY_ACCUSATIVE[label]} в {clock}"
    return f"{fire:%d.%m} в {clock}"


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
                        "args": {"to": person, "text": f"Selfvoy предлагает встретиться: {topic} ({when})."},
                    }
                )
                a2a = {
                    "target": person,
                    "intent": "schedule_meeting",
                    "topic": topic,
                    "message": f"Привет! Мой Selfvoy предлагает обсудить «{topic}» — {when}.",
                }
            reply = f"Ищу окно в графиках на {when} и подбираю место под ваши предпочтения."
            confidence = 0.86
        elif intent == "reminder":
            text = planner.reminder_text(context.message)
            at = None
            if window:
                fire = window.start
                # «завтра» без часа — утром, а не в полночь.
                if window.source == "phrase" and (fire.hour, fire.minute) == (0, 0):
                    fire = fire.replace(hour=9)
                # Время задачи — в UTC: сервер сравнивает метки строками.
                at = fire.astimezone(timezone.utc).isoformat()
                if window.source == "phrase":
                    when = _when_phrase(window.label, fire)
            actions = [{"id": "a1", "tool": "create_reminder", "args": {"text": text, "at": at}}]
            reply = f"Напомню {when}: {text[0].lower() + text[1:] if text else text}."
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

    def chat_json(self, system: str, user: str) -> Any:
        """Произвольный запрос в JSON mode (ИИ-администратор, F7): разобранный JSON ответа.

        Проверку схемы делает вызывающий; сеть/лимиты/не-JSON — ``LLMError``.
        """
        import httpx

        payload = {
            "model": self.model,
            "temperature": self.temperature,
            "response_format": {"type": "json_object"},
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
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
        except Exception as exc:
            raise LLMError(f"LLM request failed: {exc}") from exc
        try:
            return json.loads(content)
        except (json.JSONDecodeError, TypeError) as exc:
            raise LLMError("LLM returned non-JSON payload") from exc


def build_llm(settings: Any) -> BaseLLM:
    """Фабрика провайдера по настройкам."""
    provider = str(getattr(settings, "llm_provider", "auto")).lower().strip()
    if provider in ("ollama", "local", "vllm"):
        base_url = getattr(settings, "llm_base_url", "")
        if not base_url or "api.openai.com" in base_url:
            base_url = "http://127.0.0.1:11434/v1" if provider == "ollama" else "http://127.0.0.1:8000/v1"
        return OpenAILLM(
            api_key=getattr(settings, "llm_api_key", "") or "local-no-key",
            base_url=base_url,
            model=getattr(settings, "llm_model", "") or ("llama3.2" if provider == "ollama" else "default"),
            timeout=float(getattr(settings, "llm_timeout", 30.0) or 30.0),
            temperature=float(getattr(settings, "llm_temperature", 0.3) or 0.3),
        )
    if provider == "openai" or (
        provider == "auto" and getattr(settings, "llm_api_key", "")
    ):
        return OpenAILLM(
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            model=settings.llm_model,
            timeout=settings.llm_timeout,
            temperature=settings.llm_temperature,
        )
    return MockLLM()


# Пределы ответа модели. LLM — не источник истины: всё, что она вернула,
# проверяется здесь, прежде чем попасть к серверу, в память или в инструменты.
MAX_ACTIONS = 8
MAX_MEMORY_UPDATES = 5
MAX_MEMORY_TEXT = 300
MAX_TAGS = 8
MAX_TAG_LEN = 40
MAX_REPLY_LEN = 4000
MAX_INTENT_LEN = 64
MAX_ACTION_ID_LEN = 32
MAX_A2A_FIELD_LEN = 500


def normalize_actions(raw: Any) -> List[Dict[str, Any]]:
    """Приводит действия модели к безопасному виду.

    Только известные инструменты, ``args`` — обязательно объект (иначе
    действие отбрасывается, а не исполняется с пустыми аргументами), не
    больше ``MAX_ACTIONS`` действий.
    """
    if not isinstance(raw, list):
        return []
    cleaned: List[Dict[str, Any]] = []
    for index, item in enumerate(raw):
        if len(cleaned) >= MAX_ACTIONS:
            break
        if not isinstance(item, dict):
            continue
        tool = item.get("tool")
        if not isinstance(tool, str) or tool.strip() not in TOOL_NAMES:
            continue
        args = item.get("args", {})
        if args is None:
            args = {}
        if not isinstance(args, dict):
            continue
        raw_id = item.get("id")
        action_id = str(raw_id)[:MAX_ACTION_ID_LEN] if isinstance(raw_id, (str, int)) and str(raw_id) else f"a{index + 1}"
        cleaned.append({"id": action_id, "tool": tool.strip(), "args": args})
    return cleaned


def normalize_memory_updates(raw: Any) -> List[Dict[str, Any]]:
    """Записи памяти от модели: только допустимые виды, короткий текст, не больше 5.

    Слишком длинный текст отбрасывается целиком (а не обрезается): обрезанная
    «инструкция» в памяти — та же постоянная prompt-инъекция.
    """
    if not isinstance(raw, list):
        return []
    cleaned: List[Dict[str, Any]] = []
    for item in raw:
        if len(cleaned) >= MAX_MEMORY_UPDATES:
            break
        if not isinstance(item, dict):
            continue
        kind = item.get("kind", "fact")
        if kind is None:
            kind = "fact"
        if not isinstance(kind, str) or kind.strip().lower() not in MEMORY_KINDS:
            continue
        text = item.get("text")
        if not isinstance(text, str):
            continue
        text = text.strip()
        if not text or len(text) > MAX_MEMORY_TEXT:
            continue
        tags_raw = item.get("tags")
        tags: List[str] = []
        if isinstance(tags_raw, list):
            for tag in tags_raw:
                if isinstance(tag, str) and tag.strip() and len(tag) <= MAX_TAG_LEN:
                    tags.append(tag.strip())
                if len(tags) >= MAX_TAGS:
                    break
        cleaned.append({"kind": kind.strip().lower(), "text": text, "tags": tags})
    return cleaned


def _confidence(value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0.5
    number = float(value)
    if not math.isfinite(number):
        return 0.5
    return min(1.0, max(0.0, number))


def _short_str(value: Any, limit: int) -> str:
    if not isinstance(value, (str, int, float)) or isinstance(value, bool):
        return ""
    return str(value).strip()[:limit]


def _a2a(raw: Any) -> Optional[Dict[str, str]]:
    if not isinstance(raw, dict):
        return None
    target = _short_str(raw.get("target"), 200)
    if not target:
        return None
    return {
        "target": target,
        "intent": _short_str(raw.get("intent"), MAX_INTENT_LEN) or "schedule_meeting",
        "topic": _short_str(raw.get("topic"), MAX_A2A_FIELD_LEN),
        "message": _short_str(raw.get("message"), MAX_A2A_FIELD_LEN),
    }


def sanitize_payload(raw: Any) -> Dict[str, Any]:
    """Проверяет JSON модели и приводит его к контракту AgentResponse.

    Не-объект → ``LLMError`` (агент уйдёт в локальные правила). Поля с
    неверными типами заменяются безопасными значениями, лишние — отбрасываются.
    """
    if not isinstance(raw, dict):
        raise LLMError("LLM returned non-object JSON")
    return {
        "reply": _short_str(raw.get("reply"), MAX_REPLY_LEN),
        "intent": _short_str(raw.get("intent"), MAX_INTENT_LEN),
        "confidence": _confidence(raw.get("confidence")),
        "actions": normalize_actions(raw.get("actions")),
        "memory_updates": normalize_memory_updates(raw.get("memory_updates")),
        "a2a": _a2a(raw.get("a2a")),
    }


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
