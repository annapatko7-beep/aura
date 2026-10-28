package agent

import (
	"context"
	"testing"
	"time"

	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/planner"
)

func TestAgentRunMeetingFlow(t *testing.T) {
	t.Parallel()

	store := memory.NewInMemoryStore()
	mockLLM := llm.NewMockLLM()
	agent := NewSelfvoyAgent(mockLLM, store)

	ctx := context.Background()
	now := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)

	req := &AgentRequest{
		Context: llm.AgentContext{
			UserID:      1,
			DisplayName: "Alice",
			Email:       "alice@selfvoy.net",
			Now:         &now,
			Timezone:    "Europe/Moscow",
			Message:     "Давай встретимся с Бобом завтра в 15:00 обсудить релиз",
			Preferences: planner.DefaultPreferences(),
		},
		Execute: true,
		DryRun:  false,
		Peers: []llm.AgentContext{
			{
				DisplayName: "Боб",
				Email:       "bob@selfvoy.net",
				Preferences: planner.DefaultPreferences(),
			},
		},
	}

	resp, err := agent.Run(ctx, req)
	if err != nil {
		t.Fatalf("agent.Run failed: %v", err)
	}

	if resp.Intent != "schedule_meeting" {
		t.Errorf("expected intent schedule_meeting, got %s", resp.Intent)
	}

	if resp.A2A == nil {
		t.Fatalf("expected A2A proposal for Bob, got nil")
	}

	if len(resp.Plan.Steps) < 2 {
		t.Errorf("expected at least 2 plan steps, got %d", len(resp.Plan.Steps))
	}
}

func TestAgentToolPermissionCheck(t *testing.T) {
	t.Parallel()

	store := memory.NewInMemoryStore()
	agent := NewSelfvoyAgent(llm.NewMockLLM(), store)

	ctx := context.Background()
	req := &AgentRequest{
		Context: llm.AgentContext{
			Message: "Отправь письмо боссу с отчетом",
		},
		Execute: true,
		DryRun:  false,
	}

	resp, err := agent.Run(ctx, req)
	if err != nil {
		t.Fatalf("agent.Run failed: %v", err)
	}

	// send_email must have been rejected as server_only!
	for _, res := range resp.Results {
		if res.Tool == "send_email" {
			if res.Ok {
				t.Errorf("send_email should be rejected on AI service as server_only, but reported ok=true")
			}
			if res.Error == "" {
				t.Errorf("expected server_only error message")
			}
		}
	}
}

func TestAgentNegotiateAndOfferSlots(t *testing.T) {
	t.Parallel()

	agent := NewSelfvoyAgent(llm.NewMockLLM(), memory.NewInMemoryStore())
	ctx := context.Background()

	now := time.Date(2026, 10, 20, 9, 0, 0, 0, time.UTC)
	negReq := &NegotiateRequest{
		Initiator: llm.AgentContext{
			Email:       "alice@selfvoy.net",
			DisplayName: "Alice",
			Preferences: planner.DefaultPreferences(),
		},
		Responder: llm.AgentContext{
			Email:       "bob@selfvoy.net",
			DisplayName: "Bob",
			Preferences: planner.DefaultPreferences(),
		},
		Topic:           "Design Sprint",
		DurationMinutes: 60,
		WindowHours:     48,
		MultiTurn:       true,
	}

	negResp, err := agent.Negotiate(ctx, negReq)
	if err != nil {
		t.Fatalf("agent.Negotiate failed: %v", err)
	}
	if !negResp.Accepted {
		t.Errorf("expected negotiation accepted")
	}

	slotsReq := &SlotsRequest{
		Context: llm.AgentContext{
			Now:         &now,
			Timezone:    "Europe/Moscow",
			Preferences: planner.DefaultPreferences(),
		},
		DurationMinutes: 60,
		WindowHours:     48,
		MaxSlots:        4,
		PerDay:          2,
		LeadMinutes:     60,
	}

	slotsResp, err := agent.OfferSlots(ctx, slotsReq)
	if err != nil {
		t.Fatalf("agent.OfferSlots failed: %v", err)
	}
	if len(slotsResp.Slots) == 0 {
		t.Errorf("expected slots returned")
	}
}
