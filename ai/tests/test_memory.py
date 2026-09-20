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


# ------------------------------------------------------- гибрид RAG (этап 8)
def test_bm25_ranks_relevant_first():
    entries = [
        MemoryEntry(text="Люблю встречаться в кофейне", weight=1.0),
        MemoryEntry(text="Аллергия на орехи", weight=2.0),
        MemoryEntry(text="Работаю над стартапом в сфере финтеха", weight=1.0),
    ]
    ranked = memory.bm25_rank("стартап кофейня", entries, limit=3)
    texts = [e.text for _, e in ranked]
    assert texts[0].startswith("Люблю встречаться")  # короче → выше BM25
    assert all("Аллергия" not in t for t in texts)  # нет совпадений → исключена


def test_bm25_empty_query_sorts_by_weight():
    entries = [MemoryEntry(text="а", weight=1.0), MemoryEntry(text="б", weight=3.0)]
    ranked = memory.bm25_rank("", entries, limit=2)
    assert ranked[0][1].text == "б"


def test_cosine_similarity_basics():
    assert memory.cosine_similarity([1, 0], [1, 0]) == pytest.approx(1.0)
    assert memory.cosine_similarity([1, 0], [0, 1]) == pytest.approx(0.0)
    assert memory.cosine_similarity([0, 0], [1, 1]) == 0.0  # защита от деления на 0


def test_embedding_client_disabled_without_url():
    client = memory.EmbeddingClient()
    assert client.enabled is False
    assert client.embed(["текст"]) is None


def test_embedding_client_endpoint_and_parsing(monkeypatch):
    import sys
    import types

    class _Resp:
        def raise_for_status(self):
            return None

        def json(self):
            return {"data": [{"embedding": [1.0, 0.0]}, {"embedding": [0.0, 1.0]}]}

    fake_httpx = types.ModuleType("httpx")
    fake_httpx.post = lambda *a, **k: _Resp()
    monkeypatch.setitem(sys.modules, "httpx", fake_httpx)

    client = memory.EmbeddingClient(url="http://emb.local/v1", model="m")
    assert client.enabled is True
    assert client.endpoint() == "http://emb.local/v1/embeddings"
    assert client.embed(["a", "b"]) == [[1.0, 0.0], [0.0, 1.0]]


def test_embedding_client_degrades_on_network_error(monkeypatch):
    import sys
    import types

    def _boom(*a, **k):
        raise RuntimeError("network down")

    fake_httpx = types.ModuleType("httpx")
    fake_httpx.post = _boom
    monkeypatch.setitem(sys.modules, "httpx", fake_httpx)

    client = memory.EmbeddingClient(url="http://emb.local")
    assert client.embed(["a"]) is None  # graceful degrade, не исключение


def test_vector_rank_disabled_returns_none():
    entries = [MemoryEntry(text="кофе")]
    assert memory.vector_rank("кофе", entries, None) is None
    assert memory.vector_rank("кофе", entries, memory.EmbeddingClient()) is None


def test_rank_hybrid_adds_semantic_only_match():
    entries = [
        MemoryEntry(text="кофе эспрессо"),
        MemoryEntry(text="утренний бодрящий напиток"),
        MemoryEntry(text="прогулка по парку"),
    ]

    class FakeEmbedder:
        enabled = True

        def embed(self, texts):
            table = {
                "кофе": [1.0, 0.0],
                "кофе эспрессо": [0.9, 0.1],
                "утренний бодрящий напиток": [0.95, 0.05],
                "прогулка по парку": [0.0, 1.0],
            }
            return [table.get(t, [0.0, 0.0]) for t in texts]

    # Лексика: «напиток» не совпадает с «кофе» → отсутствует.
    lexical_only = memory.rank("кофе", entries, limit=3)
    assert all("напиток" not in e.text for e in lexical_only)
    # Гибрид: семантически близкий «напиток» подмешивается через RRF.
    hybrid = memory.rank("кофе", entries, limit=3, embed_client=FakeEmbedder())
    assert any("напиток" in e.text for e in hybrid)


def test_rank_degrades_to_lexical_when_embedder_unavailable():
    entries = [MemoryEntry(text="кофе эспрессо"), MemoryEntry(text="прогулка по парку")]

    class BrokenEmbedder:
        enabled = True

        def embed(self, texts):
            return None  # эндпоинт недоступен

    # С недоступным эмбеддером ранжирование = чистая лексика (без падения).
    hybrid = memory.rank("кофе", entries, limit=3, embed_client=BrokenEmbedder())
    lexical = memory.rank("кофе", entries, limit=3)
    assert [e.text for e in hybrid] == [e.text for e in lexical]
    assert hybrid[0].text == "кофе эспрессо"


def test_reciprocal_rank_fusion_merges_lists():
    a = MemoryEntry(text="а")
    b = MemoryEntry(text="б")
    fused = memory.reciprocal_rank_fusion([
        [(0.9, a), (0.1, b)],
        [(0.5, b)],
    ])
    # «б» есть в обоих списках → выше суммарный RRF-балл.
    assert fused[0][1].text == "б"


def test_rag_mode_config_property():
    from aura_ai.config import Settings

    assert Settings(rag_mode="lexical", embeddings_url="http://x").rag_is_hybrid is False
    assert Settings(rag_mode="hybrid").rag_is_hybrid is True
    assert Settings(rag_mode="auto", embeddings_url="").rag_is_hybrid is False
    assert Settings(rag_mode="auto", embeddings_url="http://x").rag_is_hybrid is True
