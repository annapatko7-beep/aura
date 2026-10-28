package memory

import (
	"context"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func TestExtractFacts(t *testing.T) {
	t.Parallel()

	input := "Привет! Я люблю фильтр-кофе, но у меня аллергия на арахис. Мой коллега Иван посоветовал встретиться."
	facts := ExtractFacts(input)

	if len(facts) < 2 {
		t.Fatalf("expected at least 2 extracted facts, got %d", len(facts))
	}

	foundCoffee := false
	foundAllergy := false
	for _, f := range facts {
		if f.Kind == KindPreference {
			foundCoffee = true
		}
		if f.Kind == KindFact {
			foundAllergy = true
		}
	}

	if !foundCoffee {
		t.Errorf("expected coffee preference extracted")
	}
	if !foundAllergy {
		t.Errorf("expected allergy fact extracted")
	}
}

func TestMergeEntries(t *testing.T) {
	t.Parallel()

	now := time.Now().UTC()
	existing := []MemoryEntry{
		{Kind: KindFact, Text: "Люблю кофе", Weight: 1.0, Tags: []string{"coffee"}, UpdatedAt: now},
		{Kind: KindFact, Text: "Не ем мясо", Weight: 1.5, Tags: []string{"diet"}, UpdatedAt: now},
	}

	additions := []MemoryEntry{
		{Kind: KindFact, Text: "люблю кофе", Weight: 1.0, Tags: []string{"drinks"}, UpdatedAt: now.Add(time.Minute)},
		{Kind: KindFact, Text: "Работаю удаленно", Weight: 1.0, Tags: []string{"work"}, UpdatedAt: now},
	}

	merged := MergeEntries(existing, additions)
	if len(merged) != 3 {
		t.Fatalf("expected 3 merged entries, got %d", len(merged))
	}

	// "Люблю кофе" should have boosted weight 1.25 and both tags "coffee" and "drinks"
	for _, m := range merged {
		if m.Text == "Люблю кофе" || m.Text == "люблю кофе" {
			if m.Weight != 1.25 {
				t.Errorf("expected boosted weight 1.25, got %f", m.Weight)
			}
			if len(m.Tags) != 2 {
				t.Errorf("expected 2 merged tags, got %d", len(m.Tags))
			}
		}
	}
}

func TestBM25Rank(t *testing.T) {
	t.Parallel()

	entries := []MemoryEntry{
		{Text: "Я люблю тихие кофейни в центре", Weight: 1.0},
		{Text: "У меня аллергия на арахис", Weight: 2.0},
		{Text: "Предпочитаю созвоны до 15 минут", Weight: 1.2},
		{Text: "Работаю в офисе по четвергам", Weight: 1.0},
	}

	query := "где выпить кофе тихо"
	ranked := BM25Rank(query, entries, 2)

	if len(ranked) == 0 {
		t.Fatalf("expected ranked results, got 0")
	}

	if ranked[0].Text != "Я люблю тихие кофейни в центре" {
		t.Errorf("expected top result to be coffee fact, got: %s", ranked[0].Text)
	}
}

func TestStoreConcurrency(t *testing.T) {
	t.Parallel()

	store := NewInMemoryStore()
	ctx := context.Background()

	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			_, _ = store.Save(ctx, "user1", []MemoryEntry{
				{Text: "Факт из горутины", Weight: 1.0},
			})
			_, _ = store.Load(ctx, "user1")
		}(i)
	}
	wg.Wait()

	loaded, err := store.Load(ctx, "user1")
	if err != nil {
		t.Fatalf("load error: %v", err)
	}
	if len(loaded) == 0 {
		t.Fatalf("expected saved memory")
	}
}

func TestFileMemoryStore(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "selfvoy_mem_test*")
	if err != nil {
		t.Fatalf("temp dir error: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	filePath := filepath.Join(tmpDir, "memory.json")
	store, err := NewFileMemoryStore(filePath)
	if err != nil {
		t.Fatalf("new file store error: %v", err)
	}

	ctx := context.Background()
	_, err = store.Save(ctx, "test_user", []MemoryEntry{
		{Text: "Персистированный факт", Weight: 1.5},
	})
	if err != nil {
		t.Fatalf("save error: %v", err)
	}

	// Read again from new instance
	store2, err := NewFileMemoryStore(filePath)
	if err != nil {
		t.Fatalf("load from disk error: %v", err)
	}

	loaded, err := store2.Load(ctx, "test_user")
	if err != nil || len(loaded) != 1 {
		t.Fatalf("expected 1 loaded item from disk, got %d, err: %v", len(loaded), err)
	}
	if loaded[0].Text != "Персистированный факт" {
		t.Errorf("unexpected text: %s", loaded[0].Text)
	}
}

func BenchmarkBM25Rank(b *testing.B) {
	entries := make([]MemoryEntry, 50)
	for i := 0; i < 50; i++ {
		entries[i] = MemoryEntry{
			Text:   "Пользователь предпочитает короткие встречи в тихих местах",
			Weight: 1.0,
		}
	}
	query := "тихие места встречи"

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		_ = BM25Rank(query, entries, 5)
	}
}
