package agent

import (
	"time"

	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/planner"
)

// Step represents a discrete step in the agent execution plan.
type Step struct {
	Title  string     `json:"title"`
	Detail string     `json:"detail,omitempty"`
	Tool   string     `json:"tool,omitempty"`
	At     *time.Time `json:"at,omitempty"`
}

// Plan represents the structured plan formulated by the agent.
type Plan struct {
	Goal        string         `json:"goal"`
	Intent      string         `json:"intent"`
	Steps       []Step         `json:"steps"`
	Constraints map[string]any `json:"constraints,omitempty"`
}

// ToolResult represents the outcome of an action execution.
type ToolResult struct {
	ID    string         `json:"id"`
	Tool  string         `json:"tool"`
	Ok    bool           `json:"ok"`
	Data  map[string]any `json:"data,omitempty"`
	Error string         `json:"error,omitempty"`
}

// AgentRequest is the incoming payload from the C++ server.
type AgentRequest struct {
	Context llm.AgentContext   `json:"context"`
	Execute bool               `json:"execute"`
	DryRun  bool               `json:"dry_run"`
	Peers   []llm.AgentContext `json:"peers,omitempty"`
}

// AgentResponse is the complete response returned to the C++ server.
type AgentResponse struct {
	Reply         string               `json:"reply"`
	Intent        string               `json:"intent"`
	Confidence    float64              `json:"confidence"`
	Plan          Plan                 `json:"plan"`
	Actions       []llm.ToolCall       `json:"actions"`
	Results       []ToolResult         `json:"results"`
	MemoryUpdates []memory.MemoryEntry `json:"memory_updates"`
	A2A           *llm.A2AProposal     `json:"a2a,omitempty"`
	LLM           string               `json:"llm"`
}

// NegotiateRequest represents an A2A scheduling request between two peers.
type NegotiateRequest struct {
	Initiator       llm.AgentContext `json:"initiator"`
	Responder       llm.AgentContext `json:"responder"`
	Intent          string           `json:"intent"`
	Topic           string           `json:"topic"`
	DurationMinutes int              `json:"duration_minutes"`
	WindowHours     int              `json:"window_hours"`
	ChoosePlace     bool             `json:"choose_place"`
	MultiTurn       bool             `json:"multi_turn"`
	SessionID       string           `json:"session_id"`
	RoundNumber     int              `json:"round_number"`
}

// NegotiateResponse represents the result of the A2A negotiation.
type NegotiateResponse struct {
	Accepted     bool           `json:"accepted"`
	Status       string         `json:"status"` // AGREED | CONVERGING | COUNTER_OFFER | REJECTED
	Slot         *planner.Slot  `json:"slot,omitempty"`
	Place        map[string]any `json:"place,omitempty"`
	Message      string         `json:"message"`
	CounterSlots []planner.Slot `json:"counter_slots,omitempty"`
	RoundsLog    any            `json:"rounds_log,omitempty"`
	SessionID    string         `json:"session_id"`
}

// SlotsRequest is the request for public booking / invite slots.
type SlotsRequest struct {
	Context         llm.AgentContext `json:"context"`
	DurationMinutes int              `json:"duration_minutes"`
	WindowHours     int              `json:"window_hours"`
	MaxSlots        int              `json:"max_slots"`
	PerDay          int              `json:"per_day"`
	LeadMinutes     int              `json:"lead_minutes"`
	Weekends        *bool            `json:"weekends,omitempty"`
}

// SlotsResponse returns available slots for invite links.
type SlotsResponse struct {
	Slots []planner.Slot `json:"slots"`
}
