package llm

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"github.com/sashabaranov/go-openai"
	"github.com/selfvoy/ai-core/pkg/planner"
)

// OpenAILLM calls an OpenAI-compatible endpoint in JSON Mode.
type OpenAILLM struct {
	client      *openai.Client
	model       string
	temperature float32
}

// NewOpenAILLM creates a new client targeting OpenAI or local compatible endpoints (Ollama/vLLM).
func NewOpenAILLM(apiKey, baseURL, model string, temperature float64) *OpenAILLM {
	if apiKey == "" {
		apiKey = "no-key"
	}
	if model == "" {
		model = "gpt-4o-mini"
	}

	cfg := openai.DefaultConfig(apiKey)
	if baseURL != "" {
		cfg.BaseURL = strings.TrimRight(baseURL, "/")
	}

	return &OpenAILLM{
		client:      openai.NewClientWithConfig(cfg),
		model:       model,
		temperature: float32(temperature),
	}
}

func (o *OpenAILLM) Name() string {
	return "openai"
}

func (o *OpenAILLM) CompleteJSON(ctx context.Context, agentCtx *AgentContext, understanding *planner.Understanding) (*LLMPayload, error) {
	userPayload := map[string]any{
		"message":       agentCtx.Message,
		"understanding": understanding,
		"preferences":   agentCtx.Preferences,
		"memory":        agentCtx.Memory,
		"recent":        agentCtx.History,
	}

	userBytes, err := json.Marshal(userPayload)
	if err != nil {
		return nil, fmt.Errorf("marshal user context: %w", err)
	}

	req := openai.ChatCompletionRequest{
		Model:       o.model,
		Temperature: o.temperature,
		ResponseFormat: &openai.ChatCompletionResponseFormat{
			Type: openai.ChatCompletionResponseFormatTypeJSONObject,
		},
		Messages: []openai.ChatCompletionMessage{
			{
				Role:    openai.ChatMessageRoleSystem,
				Content: SystemPrompt,
			},
			{
				Role:    openai.ChatMessageRoleUser,
				Content: string(userBytes),
			},
		},
	}

	resp, err := o.client.CreateChatCompletion(ctx, req)
	if err != nil {
		return nil, fmt.Errorf("openai chat completion failed: %w", err)
	}

	if len(resp.Choices) == 0 {
		return nil, fmt.Errorf("empty choices from openai")
	}

	content := resp.Choices[0].Message.Content
	var raw LLMPayload
	if err := json.Unmarshal([]byte(content), &raw); err != nil {
		return nil, fmt.Errorf("unmarshal openai json response: %w", err)
	}

	return SanitizePayload(&raw), nil
}

// SanitizePayload validates and normalizes fields returned by any LLM.
func SanitizePayload(raw *LLMPayload) *LLMPayload {
	if raw == nil {
		return &LLMPayload{
			Reply:      "Я на связи.",
			Intent:     "chat",
			Confidence: 0.5,
		}
	}

	if raw.Confidence < 0.0 {
		raw.Confidence = 0.0
	} else if raw.Confidence > 1.0 {
		raw.Confidence = 1.0
	}

	// Filter actions to valid tools
	validActions := make([]ToolCall, 0, len(raw.Actions))
	for idx, act := range raw.Actions {
		if _, ok := ToolNames[act.Tool]; ok {
			if act.ID == "" {
				act.ID = fmt.Sprintf("a%d", idx+1)
			}
			if act.Args == nil {
				act.Args = make(map[string]any)
			}
			validActions = append(validActions, act)
		}
	}
	raw.Actions = validActions

	return raw
}
