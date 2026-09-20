"""Тесты долговременной памяти."""

import pytest

from aura_ai import memory
from aura_ai.models import AgentContext, MemoryEntry


def test_merge_entries_dedupes_and_raises_weight():
    existing = [MemoryEntry(text="Люблю кофе", weight=1.0, tags=["a"])]
    additions = [MemoryEntry(text="люблю кофе", tags=["b"])]
    merged = memory.merge_entries(existing, additions)
    assert len(merged) == 1
    assert merged[0].weight == pytest.approx(1.25)
    assert merged[0].tags == ["a", "b"]


def test_extract_facts_from_russian_phrase():
    facts = memory.extract_facts("Я люблю матча-латте. У меня аллергия на орехи.")
    texts = " | ".join(f.text for f in facts)
    assert "матча-латте" in texts
    assert "орехи" in texts


def test_extract_from_context_prefers_llm_payload():
    context = AgentContext(message="Я не ем мясо")
    payload = {"memory_updates": [{"kind": "preference", "text": "вегетарианец", "tags": ["diet"]}]}
    entries = memory.extract_from_context(context, payload)
    assert any(e.text == "вегетарианец" and e.kind == "preference" for e in entries)
    assert any("не ем мясо" in e.text for e in entries)


def test_rank_prefers_relevant_entries():
    entries = [
        MemoryEntry(text="Люблю встречаться в кофейне", weight=1.0),
        MemoryEntry(text="Аллергия на орехи", weight=2.0),
        MemoryEntry(text="Работаю над стартапом в сфере финтеха", weight=1.0),
    ]
    ranked = memory.rank("стартап кофейня", entries, limit=2)
    assert ranked[0].text.startswith("Люблю встречаться")
    assert all("Аллергия" not in e.text for e in ranked)


def test_in_memory_store_roundtrip():
    store = memory.InMemoryStore()
    assert store.load("user@example.com") == []
    stored = store.save("user@example.com", [MemoryEntry(text="факт один")])
    assert len(stored) == 1
    store.save("user@example.com", [MemoryEntry(text="факт один")])
    assert len(store.load("user@example.com")) == 1


def test_file_memory_store_persists(tmp_path):
    path = str(tmp_path / "memory.json")
    first = memory.FileMemoryStore(path)
    first.save("u1", [MemoryEntry(text="живу в Керкраде")])

    second = memory.FileMemoryStore(path)
    loaded = second.load("u1")
    assert len(loaded) == 1
    assert loaded[0].text == "живу в Керкраде"


def test_load_context_memory_merges_server_side_entries():
    store = memory.InMemoryStore()
    store.save("me@example.com", [MemoryEntry(text="Люблю тихие кофейни")])
    context = AgentContext(
        email="me@example.com",
        message="найдём кофейню",
        memory=[MemoryEntry(text="Не люблю шумные места")],
    )
    merged = memory.load_context_memory(store, context)
    texts = {e.text for e in merged}
    assert "Люблю тихие кофейни" in texts
    assert "Не люблю шумные места" in texts


def test_summarize_limits_entries():
    entries = [MemoryEntry(text=f"факт {i}", weight=float(i)) for i in range(10)]
    summary = memory.summarize(entries, limit=3)
    assert len(summary.split(";")) == 3
    assert "факт 9" in summary
