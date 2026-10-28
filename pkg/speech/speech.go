package speech

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"strings"
	"time"
)

// SupportedLanguages for speech recognition.
var SupportedLanguages = map[string]string{
	"ru": "ru",
	"en": "en",
}

// TranscribeResult represents the recognized text.
type TranscribeResult struct {
	Text     string `json:"text"`
	Language string `json:"language"`
	Provider string `json:"provider"`
}

// Provider defines the speech-to-text contract.
type Provider interface {
	Name() string
	Transcribe(ctx context.Context, audio []byte, language string, filename string) (*TranscribeResult, error)
}

// MockSTT provides an offline deterministic speech recognition stub for tests and local dev.
type MockSTT struct{}

func NewMockSTT() *MockSTT {
	return &MockSTT{}
}

func (m *MockSTT) Name() string {
	return "mock"
}

func (m *MockSTT) Transcribe(ctx context.Context, audio []byte, language string, filename string) (*TranscribeResult, error) {
	if len(audio) == 0 {
		return nil, fmt.Errorf("empty audio data")
	}

	lang := strings.ToLower(strings.TrimSpace(language))
	if lang == "" || lang == "auto" {
		lang = "ru"
	}

	text := "Тестовая расшифровка Selfvoy (mock STT)."
	if lang == "en" {
		text = "Selfvoy test transcription (mock STT)."
	}

	return &TranscribeResult{
		Text:     text,
		Language: lang,
		Provider: "mock",
	}, nil
}

// WhisperSTT calls any OpenAI-compatible Whisper endpoint (OpenAI, Groq, faster-whisper).
type WhisperSTT struct {
	endpoint string
	apiKey   string
	model    string
	client   *http.Client
}

// NewWhisperSTT creates a Whisper provider.
func NewWhisperSTT(endpoint, apiKey, model string) *WhisperSTT {
	if endpoint == "" {
		endpoint = "https://api.openai.com/v1/audio/transcriptions"
	}
	if model == "" {
		model = "whisper-1"
	}
	return &WhisperSTT{
		endpoint: endpoint,
		apiKey:   apiKey,
		model:    model,
		client: &http.Client{
			Timeout: 45 * time.Second,
		},
	}
}

func (w *WhisperSTT) Name() string {
	return "whisper"
}

func (w *WhisperSTT) Transcribe(ctx context.Context, audio []byte, language string, filename string) (*TranscribeResult, error) {
	if len(audio) == 0 {
		return nil, fmt.Errorf("empty audio data")
	}
	if filename == "" {
		filename = "audio.webm"
	}

	body := &bytes.Buffer{}
	writer := multipart.NewWriter(body)

	part, err := writer.CreateFormFile("file", filename)
	if err != nil {
		return nil, fmt.Errorf("create form file: %w", err)
	}
	if _, err := part.Write(audio); err != nil {
		return nil, fmt.Errorf("write audio bytes: %w", err)
	}

	_ = writer.WriteField("model", w.model)
	_ = writer.WriteField("response_format", "json")
	if language != "" && language != "auto" {
		_ = writer.WriteField("language", language)
	}

	if err := writer.Close(); err != nil {
		return nil, fmt.Errorf("close multipart writer: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, w.endpoint, body)
	if err != nil {
		return nil, fmt.Errorf("create whisper request: %w", err)
	}
	req.Header.Set("Content-Type", writer.FormDataContentType())
	if w.apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+w.apiKey)
	}

	resp, err := w.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("whisper request failed: %w", err)
	}
	defer resp.Body.Close()

	respBytes, err := io.ReadAll(io.LimitReader(resp.Body, 1024*1024))
	if err != nil {
		return nil, fmt.Errorf("read response: %w", err)
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("whisper returned status %d: %s", resp.StatusCode, string(respBytes))
	}

	var parsed struct {
		Text     string `json:"text"`
		Language string `json:"language"`
	}
	if err := json.Unmarshal(respBytes, &parsed); err != nil {
		return nil, fmt.Errorf("unmarshal whisper response: %w", err)
	}

	return &TranscribeResult{
		Text:     parsed.Text,
		Language: parsed.Language,
		Provider: "whisper",
	}, nil
}

// BuildSTT initializes an STT provider based on configuration.
func BuildSTT(provider, apiKey, endpoint, model string) Provider {
	prov := strings.ToLower(strings.TrimSpace(provider))
	if prov == "groq" {
		ep := endpoint
		if ep == "" {
			ep = "https://api.groq.com/openai/v1/audio/transcriptions"
		}
		m := model
		if m == "" {
			m = "whisper-large-v3-turbo"
		}
		return NewWhisperSTT(ep, apiKey, m)
	}

	if (prov == "whisper" || prov == "openai") && apiKey != "" {
		ep := endpoint
		if ep == "" {
			ep = "https://api.openai.com/v1/audio/transcriptions"
		}
		m := model
		if m == "" {
			m = "whisper-1"
		}
		return NewWhisperSTT(ep, apiKey, m)
	}

	return NewMockSTT()
}
