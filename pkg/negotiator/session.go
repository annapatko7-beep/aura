package negotiator

import (
	"time"

	"github.com/selfvoy/ai-core/pkg/planner"
)

const (
	MaxNegotiationRounds = 5

	StateProposed     = "PROPOSED"
	StateCounterOffer = "COUNTER_OFFER"
	StateConverging   = "CONVERGING"
	StateAgreed       = "AGREED"
	StateRejected     = "REJECTED"
)

// RoundLog records an audit log entry for each negotiation step.
type RoundLog struct {
	Round    int           `json:"round"`
	Action   string        `json:"action"` // agree | compromise_duration | counter_offer | escalate_to_human
	Slot     *planner.Slot `json:"slot,omitempty"`
	Slots    []planner.Slot `json:"slots,omitempty"`
	Duration int           `json:"duration,omitempty"`
	Note     string        `json:"note,omitempty"`
}

// NegotiationSession tracks the state machine of an A2A scheduling dialogue.
type NegotiationSession struct {
	ID              string           `json:"id"`
	Topic           string           `json:"topic"`
	InitiatorEmail  string           `json:"initiator_email"`
	ResponderEmail  string           `json:"responder_email"`
	DurationMinutes int              `json:"duration_minutes"`
	WindowHours     int              `json:"window_hours"`
	RoundNumber     int              `json:"round_number"`
	Status          string           `json:"status"` // PROPOSED | COUNTER_OFFER | CONVERGING | AGREED | REJECTED
	Accepted        bool             `json:"accepted"`
	SelectedSlot    *planner.Slot    `json:"selected_slot,omitempty"`
	SelectedPlace   map[string]any   `json:"selected_place,omitempty"`
	CounterSlots    []planner.Slot   `json:"counter_slots,omitempty"`
	History         []RoundLog       `json:"history"`
	Message         string           `json:"message"`
	CreatedAt       time.Time        `json:"created_at"`
	UpdatedAt       time.Time        `json:"updated_at"`
}

// ParticipantContext contains the schedule and preferences of one peer in negotiation.
type ParticipantContext struct {
	Email       string                  `json:"email"`
	DisplayName string                  `json:"display_name"`
	Preferences planner.Preferences     `json:"preferences"`
	Calendar    []planner.CalendarEvent `json:"calendar"`
}
