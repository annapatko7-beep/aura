package llm

import (
	"strings"

	"github.com/selfvoy/ai-core/internal/config"
)

// BuildLLM creates an LLM Client based on application configuration.
func BuildLLM(cfg *config.Config) Client {
	provider := strings.ToLower(strings.TrimSpace(cfg.LLMProvider))

	if provider == "free" || provider == "pollinations" {
		baseURL := cfg.LLMBaseURL
		if baseURL == "" {
			baseURL = "https://text.pollinations.ai/openai"
		}
		model := cfg.LLMModel
		if model == "" {
			model = "openai"
		}
		apiKey := cfg.LLMAPIKey
		if apiKey == "" {
			apiKey = "anonymous"
		}
		return NewOpenAILLM(apiKey, baseURL, model, cfg.LLMTemperature)
	}

	if provider == "gemini" {
		baseURL := cfg.LLMBaseURL
		if baseURL == "" {
			baseURL = "https://generativelanguage.googleapis.com/v1beta/openai"
		}
		model := cfg.LLMModel
		if model == "" {
			model = "gemini-1.5-flash"
		}
		return NewOpenAILLM(cfg.LLMAPIKey, baseURL, model, cfg.LLMTemperature)
	}

	if provider == "groq" {
		baseURL := cfg.LLMBaseURL
		if baseURL == "" {
			baseURL = "https://api.groq.com/openai/v1"
		}
		model := cfg.LLMModel
		if model == "" {
			model = "llama-3.3-70b-versatile"
		}
		return NewOpenAILLM(cfg.LLMAPIKey, baseURL, model, cfg.LLMTemperature)
	}

	if provider == "openrouter" {
		baseURL := cfg.LLMBaseURL
		if baseURL == "" {
			baseURL = "https://openrouter.ai/api/v1"
		}
		model := cfg.LLMModel
		if model == "" {
			model = "google/gemini-2.0-flash-exp:free"
		}
		return NewOpenAILLM(cfg.LLMAPIKey, baseURL, model, cfg.LLMTemperature)
	}

	if provider == "ollama" || provider == "local" || provider == "vllm" {
		baseURL := cfg.LLMBaseURL
		if baseURL == "" || strings.Contains(baseURL, "api.openai.com") {
			if provider == "ollama" {
				baseURL = "http://127.0.0.1:11434/v1"
			} else {
				baseURL = "http://127.0.0.1:8000/v1"
			}
		}
		model := cfg.LLMModel
		if model == "" {
			if provider == "ollama" {
				model = "llama3.2"
			} else {
				model = "default"
			}
		}
		apiKey := cfg.LLMAPIKey
		if apiKey == "" {
			apiKey = "local-no-key"
		}
		return NewOpenAILLM(apiKey, baseURL, model, cfg.LLMTemperature)
	}

	if provider == "openai" || (provider == "auto" && cfg.LLMAPIKey != "") {
		model := cfg.LLMModel
		if model == "" {
			model = "gpt-4o-mini"
		}
		return NewOpenAILLM(cfg.LLMAPIKey, cfg.LLMBaseURL, model, cfg.LLMTemperature)
	}

	if provider == "auto" && cfg.LLMAPIKey == "" {
		// Out-of-the-box live LLM with zero registration/key needed!
		baseURL := cfg.LLMBaseURL
		if baseURL == "" {
			baseURL = "https://text.pollinations.ai/openai"
		}
		model := cfg.LLMModel
		if model == "" {
			model = "openai"
		}
		return NewOpenAILLM("anonymous", baseURL, model, cfg.LLMTemperature)
	}

	return NewMockLLM()
}
