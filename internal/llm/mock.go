package llm

import (
	"context"
	"fmt"
	"strings"
	"time"
	"unicode"

	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/planner"
)

// MockLLM provides deterministic local rules without external API calls.
type MockLLM struct{}

func NewMockLLM() *MockLLM {
	return &MockLLM{}
}

func (m *MockLLM) Name() string {
	return "mock"
}

func (m *MockLLM) CompleteJSON(ctx context.Context, agentCtx *AgentContext, understanding *planner.Understanding) (*LLMPayload, error) {
	intent := understanding.Intent
	person := understanding.Person
	topic := understanding.Topic
	if topic == "" {
		topic = "встреча"
	}
	window := understanding.Window
	when := "ближайшее время"
	if window != nil && window.Label != "" {
		when = window.Label
	}

	var actions []ToolCall
	var memoryUpdates []memory.MemoryEntry
	var a2a *A2AProposal
	var reply string
	confidence := 0.55

	switch intent {
	case "schedule_meeting":
		var winMap map[string]string
		if window != nil {
			winMap = window.AsMap()
		}
		args1 := map[string]any{}
		if winMap != nil {
			args1["window"] = winMap
		}
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "check_calendar",
			Args: args1,
		})
		actions = append(actions, ToolCall{
			ID:   "a2",
			Tool: "suggest_time",
			Args: map[string]any{"duration_minutes": understanding.DurationMinutes},
		})

		if person != "" {
			actions = append(actions, ToolCall{
				ID:   "a3",
				Tool: "send_message",
				Args: map[string]any{
					"to":   person,
					"text": fmt.Sprintf("Selfvoy предлагает встретиться: %s (%s).", topic, when),
				},
			})
			a2a = &A2AProposal{
				Target:  person,
				Intent:  "schedule_meeting",
				Topic:   topic,
				Message: fmt.Sprintf("Привет! Мой Selfvoy предлагает обсудить «%s» — %s.", topic, when),
			}
		}
		reply = fmt.Sprintf("Ищу окно в графиках на %s и подбираю место под ваши предпочтения.", when)
		confidence = 0.86

	case "reminder":
		remText := planner.ReminderText(agentCtx.Message)
		var atStr string
		if window != nil {
			fire := window.Start
			if window.Source == "phrase" && fire.Hour() == 0 && fire.Minute() == 0 {
				fire = time.Date(fire.Year(), fire.Month(), fire.Day(), 9, 0, 0, 0, fire.Location())
			}
			atStr = fire.UTC().Format(time.RFC3339)
		}
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "create_reminder",
			Args: map[string]any{"text": remText, "at": atStr},
		})
		lowerFirst := remText
		if len(remText) > 0 {
			runes := []rune(remText)
			runes[0] = unicode.ToLower(runes[0])
			lowerFirst = string(runes)
		}
		reply = fmt.Sprintf("Напомню %s: %s.", when, lowerFirst)
		confidence = 0.90

	case "note":
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "create_note",
			Args: map[string]any{"text": agentCtx.Message},
		})
		reply = "Записал в заметки."
		confidence = 0.88

	case "email":
		recipient := person
		if recipient == "" {
			recipient = agentCtx.Email
		}
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "send_email",
			Args: map[string]any{
				"to":      recipient,
				"subject": strings.Title(topic),
				"body":    agentCtx.Message,
			},
		})
		reply = "Черновик письма готов и отправлен."
		confidence = 0.80

	case "message":
		to := person
		if to == "" {
			to = "self"
		}
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "send_message",
			Args: map[string]any{"to": to, "text": agentCtx.Message},
		})
		reply = "Сообщение отправлено."
		confidence = 0.82

	case "find_place":
		actions = append(actions, ToolCall{
			ID:   "a1",
			Tool: "find_cafe",
			Args: map[string]any{
				"city": agentCtx.Preferences.City,
				"diet": agentCtx.Preferences.Diet,
			},
		})
		reply = "Подобрал несколько мест под твои предпочтения."
		confidence = 0.79

	case "memory":
		memoryUpdates = append(memoryUpdates, memory.MemoryEntry{
			Kind:      memory.KindFact,
			Text:      agentCtx.Message,
			Weight:    1.0,
			Tags:      []string{"profile"},
			UpdatedAt: time.Now().UTC(),
		})
		reply = "Запомнил."
		confidence = 0.93

	default:
		reply = "Я на связи: могу договориться о встрече, напомнить, записать заметку или написать письмо."
		confidence = 0.55
	}

	if understanding.Topic != "" && intent != "memory" {
		memoryUpdates = append(memoryUpdates, memory.MemoryEntry{
			Kind:      memory.KindFact,
			Text:      fmt.Sprintf("Интересует тема: %s", understanding.Topic),
			Weight:    1.0,
			Tags:      []string{"interest"},
			UpdatedAt: time.Now().UTC(),
		})
	}

	return &LLMPayload{
		Reply:         reply,
		Intent:        intent,
		Confidence:    confidence,
		Actions:       actions,
		MemoryUpdates: memoryUpdates,
		A2A:           a2a,
	}, nil
}
