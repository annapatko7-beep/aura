package negotiator

import (
	"testing"
	"time"

	"github.com/selfvoy/ai-core/pkg/planner"
)

func TestNegotiationImmediateAgreement(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 10, 20, 9, 0, 0, 0, time.UTC)
	session := &NegotiationSession{
		ID:              "neg-1",
		Topic:           "Coffee & Strategy",
		InitiatorEmail:  "alice@selfvoy.net",
		ResponderEmail:  "bob@selfvoy.net",
		DurationMinutes: 60,
		WindowHours:     48,
		RoundNumber:     1,
		Status:          StateProposed,
	}

	initContext := ParticipantContext{
		Email:       "alice@selfvoy.net",
		DisplayName: "Alice",
		Preferences: planner.DefaultPreferences(),
		Calendar:    []planner.CalendarEvent{}, // Alice is free
	}

	respContext := ParticipantContext{
		Email:       "bob@selfvoy.net",
		DisplayName: "Bob",
		Preferences: planner.DefaultPreferences(),
		Calendar:    []planner.CalendarEvent{}, // Bob is free
	}

	place := map[string]any{"name": "Aurora Roasters"}
	updated := RunNegotiationRound(session, initContext, respContext, place, now)

	if updated.Status != StateAgreed {
		t.Fatalf("expected AGREED, got %s", updated.Status)
	}
	if !updated.Accepted {
		t.Errorf("expected Accepted = true")
	}
	if updated.SelectedSlot == nil {
		t.Fatalf("expected SelectedSlot to be populated")
	}
	if len(updated.History) != 1 || updated.History[0].Action != "agree" {
		t.Errorf("expected 1 history item with action 'agree'")
	}
}

func TestNegotiationDurationCompromise(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)
	session := &NegotiationSession{
		ID:              "neg-2",
		Topic:           "Quick sync",
		InitiatorEmail:  "alice@selfvoy.net",
		ResponderEmail:  "bob@selfvoy.net",
		DurationMinutes: 60,
		WindowHours:     24,
		RoundNumber:     1,
		Status:          StateProposed,
	}

	// Make Bob busy everywhere EXCEPT a 45-minute gap between 11:00 and 11:45
	// So 60 min cannot fit, but 45 min fits!
	bobCalendar := []planner.CalendarEvent{
		{Title: "E1", Start: now, End: now.Add(1 * time.Hour)}, // 10:00 - 11:00 busy
		{Title: "E2", Start: now.Add(1*time.Hour + 45*time.Minute), End: now.Add(24 * time.Hour)}, // 11:45 onwards busy
	}

	initContext := ParticipantContext{
		Email:       "alice@selfvoy.net",
		DisplayName: "Alice",
		Preferences: planner.DefaultPreferences(),
		Calendar:    nil,
	}
	respContext := ParticipantContext{
		Email:       "bob@selfvoy.net",
		DisplayName: "Bob",
		Preferences: planner.DefaultPreferences(),
		Calendar:    bobCalendar,
	}

	updated := RunNegotiationRound(session, initContext, respContext, nil, now)

	if updated.Status != StateConverging {
		t.Fatalf("expected CONVERGING, got %s", updated.Status)
	}
	if updated.SelectedSlot == nil {
		t.Fatalf("expected SelectedSlot to be populated")
	}
	if updated.RoundNumber != 2 {
		t.Errorf("expected RoundNumber to increment to 2, got %d", updated.RoundNumber)
	}
}

func TestNegotiationEscalateToHuman(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)
	session := &NegotiationSession{
		ID:              "neg-3",
		Topic:           "Urgent meeting",
		InitiatorEmail:  "alice@selfvoy.net",
		ResponderEmail:  "bob@selfvoy.net",
		DurationMinutes: 60,
		WindowHours:     24,
		RoundNumber:     MaxNegotiationRounds, // Round 5!
		Status:          StateCounterOffer,
	}

	// Bob is completely booked all 24 hours
	bobCalendar := []planner.CalendarEvent{
		{Title: "All Day Busy", Start: now, End: now.Add(24 * time.Hour)},
	}

	initContext := ParticipantContext{
		Email:       "alice@selfvoy.net",
		DisplayName: "Alice",
		Preferences: planner.DefaultPreferences(),
		Calendar:    nil,
	}
	respContext := ParticipantContext{
		Email:       "bob@selfvoy.net",
		DisplayName: "Bob",
		Preferences: planner.DefaultPreferences(),
		Calendar:    bobCalendar,
	}

	updated := RunNegotiationRound(session, initContext, respContext, nil, now)

	if updated.Status != StateRejected {
		t.Fatalf("expected REJECTED, got %s", updated.Status)
	}
	if updated.Accepted {
		t.Errorf("expected Accepted = false")
	}
	if len(updated.History) == 0 || updated.History[len(updated.History)-1].Action != "escalate_to_human" {
		t.Errorf("expected escalate_to_human in history")
	}
}

func BenchmarkRunNegotiationRound(b *testing.B) {
	now := time.Date(2026, 10, 20, 9, 0, 0, 0, time.UTC)
	initContext := ParticipantContext{
		Email:       "alice@selfvoy.net",
		DisplayName: "Alice",
		Preferences: planner.DefaultPreferences(),
		Calendar:    []planner.CalendarEvent{},
	}
	respContext := ParticipantContext{
		Email:       "bob@selfvoy.net",
		DisplayName: "Bob",
		Preferences: planner.DefaultPreferences(),
		Calendar:    []planner.CalendarEvent{},
	}
	place := map[string]any{"name": "Aurora Roasters"}

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		session := &NegotiationSession{
			ID:              "bench-neg",
			DurationMinutes: 60,
			WindowHours:     48,
			RoundNumber:     1,
			Status:          StateProposed,
		}
		_ = RunNegotiationRound(session, initContext, respContext, place, now)
	}
}
