"""memory.py — долговременная память пользователя.

C++-сервер (MemoryManager) является источником истины в PostgreSQL. Этот модуль:
* извлекает факты из диалога (``extract_facts``);
* ранжирует воспоминания под конкретный запрос (``rank``);
* хранит локальную копию для офлайн-разработки (``FileMemoryStore``) и умеет
  ходить в PostgreSQL, если задан DSN (``PostgresMemoryStore``).
"""

from __future__ import annotations

import json
import logging
import math
import os
import re
import threading
from collections import Counter
from datetime import datetime

logger = logging.getLogger("aura.ai.memory")

# psycopg импортируется лениво (без драйвера работаем через файловое хранилище),
# поэтому типы его ошибок собираем здесь, чтобы ниже не ловить «всё подряд».
try:  # pragma: no cover - зависит от окружения
    import psycopg as _psycopg

    _DB_ERRORS: tuple = (_psycopg.Error,)
except ImportError:  # pragma: no cover
    _DB_ERRORS = ()

_MEMORY_ERRORS: tuple = (ImportError, OSError, ValueError) + _DB_ERRORS
from typing import Any, Dict, Iterable, List, Optional, Sequence

from .models import AgentContext, MemoryEntry, utcnow

FACT_PATTERNS = (
    re.compile(r"я\s+(люблю|не люблю|предпочитаю|ем|не ем|живу|работаю)\s+([^.;!?]{2,60})", re.I),
    re.compile(r"у меня\s+(аллергия на|есть|нет)\s+([^.;!?]{2,60})", re.I),
    re.compile(r"мо(?:й|я|ё)\s+(друг|подруга|коллега|начальник)\s+([А-ЯЁA-Z][а-яёa-zA-Z]{1,30})"),
)

STOPWORDS = {
    "что", "как", "это", "для", "мне", "тебе", "хочу", "надо", "быть", "чтобы",
    "сегодня", "завтра", "встрет", "можно", "просто", "очень", "котор", "когда",
}


class MemoryStore:
    """Интерфейс хранилища памяти (user_id → записи)."""

    def load(self, user_key: str) -> List[MemoryEntry]:  # pragma: no cover - интерфейс
        raise NotImplementedError

    def save(self, user_key: str, entries: Sequence[MemoryEntry]) -> List[MemoryEntry]:  # pragma: no cover
        raise NotImplementedError

    def clear(self, user_key: str) -> None:  # pragma: no cover
        raise NotImplementedError


class InMemoryStore(MemoryStore):
    """Хранилище в процессе — для тестов."""

    def __init__(self) -> None:
        self._data: Dict[str, List[MemoryEntry]] = {}
        self._lock = threading.Lock()

    def load(self, user_key: str) -> List[MemoryEntry]:
        with self._lock:
            return list(self._data.get(user_key, []))

    def save(self, user_key: str, entries: Sequence[MemoryEntry]) -> List[MemoryEntry]:
        with self._lock:
            merged = merge_entries(self._data.get(user_key, []), entries)
            self._data[user_key] = merged
            return list(merged)

    def clear(self, user_key: str) -> None:
        with self._lock:
            self._data.pop(user_key, None)


class FileMemoryStore(InMemoryStore):
    """Тот же словарь, но с сохранением в JSON-файл (локальная разработка)."""

    def __init__(self, path: str):
        super().__init__()
        self.path = path
        self._read()

    def _read(self) -> None:
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                raw = json.load(handle)
        except (OSError, json.JSONDecodeError):
            return
        with self._lock:
            self._data = {
                key: [MemoryEntry(**item) for item in value] for key, value in raw.items()
            }

    def _flush(self) -> None:
        directory = os.path.dirname(os.path.abspath(self.path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        with self._lock:
            payload = {
                key: [entry.model_dump(mode="json") for entry in value]
                for key, value in self._data.items()
            }
        with open(self.path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)

    def save(self, user_key: str, entries: Sequence[MemoryEntry]) -> List[MemoryEntry]:
        merged = super().save(user_key, entries)
        self._flush()
        return merged

    def clear(self, user_key: str) -> None:
        super().clear(user_key)
        self._flush()


class PostgresMemoryStore(MemoryStore):
    """Чтение/запись таблицы ``user_memory`` через psycopg (если он установлен).

    В проде источником истины остаётся C++-сервер: он передаёт память в контексте
    запроса. Это хранилище нужно для автономного режима AI-сервиса.
    """

    def __init__(self, dsn: str):
        self.dsn = dsn
        self._fallback = InMemoryStore()

    def _connect(self):
        import psycopg  # ленивый импорт: без драйвера работаем через fallback

        return psycopg.connect(self.dsn)

    def load(self, user_key: str) -> List[MemoryEntry]:
        try:
            with self._connect() as conn, conn.cursor() as cur:
                cur.execute(
                    "SELECT kind, text, weight, tags, updated_at FROM user_memory "
                    "WHERE user_id = (SELECT id FROM users WHERE email = %s) "
                    "ORDER BY weight DESC, updated_at DESC",
                    (user_key,),
                )
                rows = cur.fetchall()
        except _MEMORY_ERRORS as exc:
            # Падаем в файловое хранилище, но не молча: иначе проблемы с БД
            # выглядят как «память просто пустая».
            logger.warning("не удалось прочитать память из БД (%s), использую файл", exc)
            return self._fallback.load(user_key)
        return [
            MemoryEntry(kind=r[0], text=r[1], weight=float(r[2]), tags=list(r[3] or []), updated_at=r[4])
            for r in rows
        ]

    def save(self, user_key: str, entries: Sequence[MemoryEntry]) -> List[MemoryEntry]:
        try:
            with self._connect() as conn, conn.cursor() as cur:
                for entry in entries:
                    cur.execute(
                        """
                        INSERT INTO user_memory (user_id, kind, text, weight, tags, updated_at)
                        SELECT id, %s, %s, %s, %s, %s FROM users WHERE email = %s
                        ON CONFLICT (user_id, text) DO UPDATE
                          SET weight = EXCLUDED.weight, updated_at = EXCLUDED.updated_at
                        """,
                        (
                            entry.kind,
                            entry.text,
                            entry.weight,
                            json.dumps(entry.tags, ensure_ascii=False),
                            entry.updated_at,
                            user_key,
                        ),
                    )
                conn.commit()
        except _MEMORY_ERRORS as exc:
            logger.warning("не удалось сохранить память в БД (%s), использую файл", exc)
            return self._fallback.save(user_key, entries)
        return self.load(user_key)

    def clear(self, user_key: str) -> None:
        self._fallback.clear(user_key)


def build_store(backend: str) -> MemoryStore:
    if backend.startswith("postgres"):
        return PostgresMemoryStore(backend)
    if backend.startswith("file://"):
        return FileMemoryStore(backend[len("file://") :])
    return InMemoryStore()


def merge_entries(
    existing: Sequence[MemoryEntry], additions: Sequence[MemoryEntry]
) -> List[MemoryEntry]:
    """Объединяет записи, убирая дубликаты по тексту и поднимая вес известных."""
    by_text: Dict[str, MemoryEntry] = {}
    for entry in list(existing) + list(additions):
        key = entry.text.strip().lower()
        if not key:
            continue
        if key in by_text:
            previous = by_text[key]
            by_text[key] = previous.model_copy(
                update={
                    "weight": min(5.0, round(previous.weight + 0.25, 3)),
                    "tags": sorted(set(previous.tags) | set(entry.tags)),
                    "updated_at": max(previous.updated_at, entry.updated_at),
                }
            )
        else:
            by_text[key] = entry
    merged = list(by_text.values())
    merged.sort(key=lambda e: (-e.weight, e.updated_at))
    return merged


def extract_facts(text: str) -> List[MemoryEntry]:
    """Достаёт устойчивые факты из свободной фразы (без LLM)."""
    facts: List[MemoryEntry] = []
    for pattern in FACT_PATTERNS:
        for match in pattern.finditer(text or ""):
            phrase = " ".join(part for part in match.groups() if part).strip()
            if phrase:
                facts.append(
                    MemoryEntry(
                        kind="preference" if "люблю" in phrase.lower() or "предпоч" in phrase.lower() else "fact",
                        text=phrase,
                        tags=["extracted"],
                    )
                )
    return facts


def extract_from_context(context: AgentContext, llm_payload: Optional[Dict[str, Any]] = None) -> List[MemoryEntry]:
    """Факты из контекста: сначала то, что предложила модель, потом правила."""
    entries: List[MemoryEntry] = []
    if llm_payload:
        for item in llm_payload.get("memory_updates") or []:
            if not isinstance(item, dict):
                continue
            text = str(item.get("text", "")).strip()
            if not text:
                continue
            tags = item.get("tags")
            entries.append(
                MemoryEntry(
                    kind=str(item.get("kind") or "fact"),
                    text=text,
                    tags=[str(t) for t in tags] if isinstance(tags, list) else [],
                )
            )
    entries.extend(extract_facts(context.message))
    return entries


STEM_LEN = 4


def stem(word: str) -> str:
    """Грубый «стемминг» по первым символам: «стартап» и «стартапом» совпадут.

    Полноценный морфологический анализ здесь избыточен — память ищется по
    совпадению основ, а точность добирается весом записи.
    """
    return word[:STEM_LEN] if len(word) >= STEM_LEN else word


def tokenize(text: str) -> List[str]:
    return [w for w in re.findall(r"[а-яёa-z0-9]{3,}", (text or "").lower()) if w not in STOPWORDS]


def rank(query: str, entries: Iterable[MemoryEntry], limit: int = 8) -> List[MemoryEntry]:
    """Простое релевантное ранжирование: TF-совпадения + вес записи."""
    query_tokens = tokenize(query)
    if not query_tokens:
        return sorted(entries, key=lambda e: -e.weight)[:limit]
    counts = Counter(stem(token) for token in query_tokens)
    scored: List[tuple[float, MemoryEntry]] = []
    for entry in entries:
        tokens = tokenize(entry.text)
        if not tokens:
            continue
        overlap = sum(counts[stem(t)] for t in tokens if stem(t) in counts)
        if overlap == 0:
            continue
        tf = overlap / (1 + math.log(len(tokens)))
        scored.append((round(tf * (0.5 + entry.weight), 4), entry))
    scored.sort(key=lambda item: (-item[0], item[1].text))
    return [entry for _, entry in scored[:limit]]


def summarize(entries: Sequence[MemoryEntry], limit: int = 5) -> str:
    """Короткая сводка памяти для промпта модели."""
    top = sorted(entries, key=lambda e: -e.weight)[:limit]
    return "; ".join(entry.text for entry in top)


def as_dicts(entries: Sequence[MemoryEntry]) -> List[Dict[str, Any]]:
    return [entry.model_dump(mode="json") for entry in entries]


def now() -> datetime:
    return utcnow()


def user_key(context: AgentContext) -> str:
    return context.email or str(context.user_id)


def load_context_memory(store: MemoryStore, context: AgentContext, limit: int = 20) -> List[MemoryEntry]:
    """Память из хранилища + память, присланная C++-сервером, без дублей.

    Сначала идут записи, релевантные текущей фразе, затем — самые весомые из
    остальных: агенту важно помнить и про «не люблю шумные места», даже если
    в запросе об этом не сказано.
    """
    merged = merge_entries(store.load(user_key(context)), context.memory)
    relevant = rank(context.message, merged, limit=limit)
    seen = {entry.text.lower() for entry in relevant}
    for entry in merged:
        if len(relevant) >= limit:
            break
        if entry.text.lower() not in seen:
            relevant.append(entry)
            seen.add(entry.text.lower())
    return relevant
