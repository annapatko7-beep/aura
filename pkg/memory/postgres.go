package memory

import (
	"context"
	"log"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// PostgresMemoryStore implements Store backed by PostgreSQL.
// It includes an automatic in-memory fallback for high resilience.
type PostgresMemoryStore struct {
	pool     *pgxpool.Pool
	fallback *InMemoryStore
}

// NewPostgresMemoryStore establishes a connection pool to PostgreSQL and initializes the schema.
func NewPostgresMemoryStore(ctx context.Context, dsn string) (*PostgresMemoryStore, error) {
	fallback := NewInMemoryStore()

	poolCfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		log.Printf("[WARN] Failed to parse PostgreSQL DSN: %v, using memory fallback", err)
		return &PostgresMemoryStore{fallback: fallback}, nil
	}

	poolCfg.MaxConns = 10
	poolCfg.MinConns = 2
	poolCfg.MaxConnIdleTime = 10 * time.Minute
	poolCfg.MaxConnLifetime = 1 * time.Hour
	// Supabase and other transaction poolers (port 6543) require SimpleProtocol to avoid
	// prepared statement collisions (SQLSTATE 42P05).
	poolCfg.ConnConfig.DefaultQueryExecMode = pgx.QueryExecModeSimpleProtocol

	connectCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()

	pool, err := pgxpool.NewWithConfig(connectCtx, poolCfg)
	if err != nil {
		log.Printf("[WARN] Failed to connect to PostgreSQL: %v, using memory fallback", err)
		return &PostgresMemoryStore{fallback: fallback}, nil
	}

	store := &PostgresMemoryStore{
		pool:     pool,
		fallback: fallback,
	}

	if err := store.initSchema(connectCtx); err != nil {
		log.Printf("[WARN] Failed to initialize user_memory table: %v", err)
	}

	return store, nil
}

func (p *PostgresMemoryStore) initSchema(ctx context.Context) error {
	if p.pool == nil {
		return nil
	}

	schemaSQL := `
	CREATE TABLE IF NOT EXISTS user_memory (
		id         BIGSERIAL PRIMARY KEY,
		user_key   TEXT NOT NULL,
		kind       TEXT NOT NULL DEFAULT 'fact',
		text       TEXT NOT NULL,
		weight     DOUBLE PRECISION NOT NULL DEFAULT 1.0,
		tags       TEXT[] NOT NULL DEFAULT '{}',
		updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
		UNIQUE (user_key, text)
	);
	CREATE INDEX IF NOT EXISTS idx_user_memory_key ON user_memory (user_key);
	`
	_, err := p.pool.Exec(ctx, schemaSQL)
	return err
}

func (p *PostgresMemoryStore) Load(ctx context.Context, userKey string) ([]MemoryEntry, error) {
	if p.pool == nil {
		return p.fallback.Load(ctx, userKey)
	}

	query := `
		SELECT kind, text, weight, tags, updated_at
		FROM user_memory
		WHERE user_key = $1
		ORDER BY weight DESC, updated_at DESC
	`

	rows, err := p.pool.Query(ctx, query, userKey)
	if err != nil {
		log.Printf("[WARN] Postgres load failed: %v, falling back to in-memory", err)
		return p.fallback.Load(ctx, userKey)
	}
	defer rows.Close()

	var entries []MemoryEntry
	for rows.Next() {
		var (
			kind      string
			text      string
			weight    float64
			tags      []string
			updatedAt time.Time
		)

		if err := rows.Scan(&kind, &text, &weight, &tags, &updatedAt); err != nil {
			continue
		}

		entries = append(entries, MemoryEntry{
			Kind:      kind,
			Text:      text,
			Weight:    weight,
			Tags:      tags,
			UpdatedAt: updatedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return p.fallback.Load(ctx, userKey)
	}

	return entries, nil
}

func (p *PostgresMemoryStore) Save(ctx context.Context, userKey string, entries []MemoryEntry) ([]MemoryEntry, error) {
	if p.pool == nil {
		return p.fallback.Save(ctx, userKey, entries)
	}

	upsertSQL := `
		INSERT INTO user_memory (user_key, kind, text, weight, tags, updated_at)
		VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (user_key, text) DO UPDATE
		SET weight = EXCLUDED.weight, updated_at = EXCLUDED.updated_at, tags = EXCLUDED.tags
	`

	for _, entry := range entries {
		text := strings.TrimSpace(entry.Text)
		if text == "" {
			continue
		}
		kind := entry.Kind
		if _, ok := ValidMemoryKinds[kind]; !ok {
			kind = KindFact
		}
		weight := entry.Weight
		if weight <= 0 {
			weight = 1.0
		}
		updatedAt := entry.UpdatedAt
		if updatedAt.IsZero() {
			updatedAt = time.Now().UTC()
		}
		tags := entry.Tags
		if tags == nil {
			tags = []string{}
		}

		_, err := p.pool.Exec(ctx, upsertSQL, userKey, kind, text, weight, tags, updatedAt)
		if err != nil {
			log.Printf("[WARN] Postgres upsert failed for '%s': %v, saving to fallback", text, err)
			return p.fallback.Save(ctx, userKey, entries)
		}
	}

	return p.Load(ctx, userKey)
}

func (p *PostgresMemoryStore) Clear(ctx context.Context, userKey string) error {
	_ = p.fallback.Clear(ctx, userKey)
	if p.pool == nil {
		return nil
	}

	_, err := p.pool.Exec(ctx, "DELETE FROM user_memory WHERE user_key = $1", userKey)
	return err
}

func (p *PostgresMemoryStore) Close() {
	if p.pool != nil {
		p.pool.Close()
	}
}
