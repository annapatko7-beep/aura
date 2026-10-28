package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// Store defines the persistent or memory storage contract for user memories.
type Store interface {
	Load(ctx context.Context, userKey string) ([]MemoryEntry, error)
	Save(ctx context.Context, userKey string, entries []MemoryEntry) ([]MemoryEntry, error)
	Clear(ctx context.Context, userKey string) error
}

// InMemoryStore provides a thread-safe in-process memory store.
type InMemoryStore struct {
	mu   sync.RWMutex
	data map[string][]MemoryEntry
}

// NewInMemoryStore creates a new InMemoryStore.
func NewInMemoryStore() *InMemoryStore {
	return &InMemoryStore{
		data: make(map[string][]MemoryEntry),
	}
}

func (s *InMemoryStore) Load(ctx context.Context, userKey string) ([]MemoryEntry, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	entries := s.data[userKey]
	res := make([]MemoryEntry, len(entries))
	copy(res, entries)
	return res, nil
}

func (s *InMemoryStore) Save(ctx context.Context, userKey string, entries []MemoryEntry) ([]MemoryEntry, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	existing := s.data[userKey]
	merged := MergeEntries(existing, entries)
	s.data[userKey] = merged

	res := make([]MemoryEntry, len(merged))
	copy(res, merged)
	return res, nil
}

func (s *InMemoryStore) Clear(ctx context.Context, userKey string) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	delete(s.data, userKey)
	return nil
}

// FileMemoryStore persists memories to a JSON file on disk atomically.
type FileMemoryStore struct {
	*InMemoryStore
	path string
}

// NewFileMemoryStore initializes or reads memory from a JSON file.
func NewFileMemoryStore(path string) (*FileMemoryStore, error) {
	fs := &FileMemoryStore{
		InMemoryStore: NewInMemoryStore(),
		path:          path,
	}

	if err := fs.read(); err != nil && !os.IsNotExist(err) {
		// Log and continue with empty memory
		return fs, nil
	}

	return fs, nil
}

func (fs *FileMemoryStore) read() error {
	fs.mu.Lock()
	defer fs.mu.Unlock()

	data, err := os.ReadFile(fs.path)
	if err != nil {
		return err
	}

	var raw map[string][]MemoryEntry
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}

	fs.data = raw
	return nil
}

func (fs *FileMemoryStore) flush() error {
	dir := filepath.Dir(fs.path)
	if dir != "" {
		_ = os.MkdirAll(dir, 0755)
	}

	bytes, err := json.MarshalIndent(fs.data, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal memories: %w", err)
	}

	tmpFile := fs.path + ".tmp"
	if err := os.WriteFile(tmpFile, bytes, 0644); err != nil {
		return fmt.Errorf("write tmp file: %w", err)
	}

	if err := os.Rename(tmpFile, fs.path); err != nil {
		_ = os.Remove(tmpFile)
		return fmt.Errorf("atomic rename file: %w", err)
	}

	return nil
}

func (fs *FileMemoryStore) Save(ctx context.Context, userKey string, entries []MemoryEntry) ([]MemoryEntry, error) {
	res, err := fs.InMemoryStore.Save(ctx, userKey, entries)
	if err != nil {
		return nil, err
	}
	fs.mu.Lock()
	defer fs.mu.Unlock()
	_ = fs.flush()
	return res, nil
}

func (fs *FileMemoryStore) Clear(ctx context.Context, userKey string) error {
	if err := fs.InMemoryStore.Clear(ctx, userKey); err != nil {
		return err
	}
	fs.mu.Lock()
	defer fs.mu.Unlock()
	return fs.flush()
}

// LoadContextMemory retrieves relevant memories for the prompt via hybrid RAG:
// BM25 lexical ranking on message first, followed by remaining high-weight profile facts.
func LoadContextMemory(ctx context.Context, store Store, userKey string, message string, contextMemory []MemoryEntry, limit int) ([]MemoryEntry, error) {
	if limit <= 0 {
		limit = 20
	}

	var stored []MemoryEntry
	if store != nil && userKey != "" {
		var err error
		stored, err = store.Load(ctx, userKey)
		if err != nil {
			stored = nil
		}
	}

	merged := MergeEntries(stored, contextMemory)
	if len(merged) == 0 {
		return nil, nil
	}

	ranked := BM25Rank(message, merged, limit)
	seen := make(map[string]struct{}, len(ranked))
	for _, r := range ranked {
		seen[strings.ToLower(strings.TrimSpace(r.Text))] = struct{}{}
	}

	// Fill up to limit with top remaining facts
	for _, e := range merged {
		if len(ranked) >= limit {
			break
		}
		key := strings.ToLower(strings.TrimSpace(e.Text))
		if _, already := seen[key]; !already {
			ranked = append(ranked, e)
			seen[key] = struct{}{}
		}
	}

	return ranked, nil
}

// BuildStore selects and initializes the storage backend based on configuration.
func BuildStore(ctx context.Context, backend, dsn, filePath string) Store {
	lowered := strings.ToLower(strings.TrimSpace(backend))

	if strings.HasPrefix(lowered, "postgres://") || strings.HasPrefix(lowered, "postgresql://") {
		store, _ := NewPostgresMemoryStore(ctx, backend)
		return store
	}

	if (lowered == "postgres" || lowered == "postgresql") && dsn != "" {
		store, _ := NewPostgresMemoryStore(ctx, dsn)
		return store
	}

	if lowered == "file" || strings.HasPrefix(lowered, "file://") {
		path := filePath
		if strings.HasPrefix(lowered, "file://") {
			path = strings.TrimPrefix(backend, "file://")
		}
		if path == "" {
			path = "./.selfvoy_memory.json"
		}
		store, err := NewFileMemoryStore(path)
		if err == nil {
			return store
		}
	}

	return NewInMemoryStore()
}
