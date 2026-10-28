package memory

import (
	"encoding/json"
	"time"
)

// Supported memory item kinds.
const (
	KindFact       = "fact"
	KindPreference = "preference"
	KindSchedule   = "schedule"
	KindContact    = "contact"
)

var ValidMemoryKinds = map[string]struct{}{
	KindFact:       {},
	KindPreference: {},
	KindSchedule:   {},
	KindContact:    {},
}

// MemoryEntry represents a single remembered fact, preference, or schedule note.
type MemoryEntry struct {
	Kind      string    `json:"kind"` // fact | preference | schedule | contact
	Text      string    `json:"text"`
	Weight    float64   `json:"weight"`
	Tags      []string  `json:"tags,omitempty"`
	UpdatedAt time.Time `json:"updated_at"`
}

// MarshalJSON ensures ISO8601 UTC timestamp format for cross-service compatibility.
func (m MemoryEntry) MarshalJSON() ([]byte, error) {
	type Alias MemoryEntry
	return json.Marshal(&struct {
		UpdatedAt string `json:"updated_at"`
		Alias
	}{
		UpdatedAt: m.UpdatedAt.UTC().Format(time.RFC3339),
		Alias:     (Alias)(m),
	})
}
