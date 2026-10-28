package memory

import (
	"context"
	"testing"
	"time"
)

func TestPostgresMemoryStoreFallback(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	// Connect to non-existent DSN -> must gracefully fallback to in-memory without crashing
	store, err := NewPostgresMemoryStore(ctx, "postgresql://user:pass@127.0.0.1:59999/nonexistent?connect_timeout=1")
	if err != nil {
		t.Fatalf("unexpected error, should fallback: %v", err)
	}

	entries := []MemoryEntry{
		{Text: "Факт для проверки отказоустойчивости", Weight: 1.2, UpdatedAt: time.Now().UTC()},
	}

	saved, err := store.Save(ctx, "user_fallback", entries)
	if err != nil {
		t.Fatalf("save error: %v", err)
	}
	if len(saved) != 1 {
		t.Fatalf("expected 1 saved entry via fallback, got %d", len(saved))
	}

	loaded, err := store.Load(ctx, "user_fallback")
	if err != nil || len(loaded) != 1 {
		t.Fatalf("expected 1 loaded entry via fallback, got %d, err: %v", len(loaded), err)
	}
	if loaded[0].Text != "Факт для проверки отказоустойчивости" {
		t.Errorf("unexpected text: %s", loaded[0].Text)
	}

	_ = store.Clear(ctx, "user_fallback")
	afterClear, _ := store.Load(ctx, "user_fallback")
	if len(afterClear) != 0 {
		t.Errorf("expected 0 entries after clear")
	}

	store.Close()
}
