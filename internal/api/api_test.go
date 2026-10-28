package api

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/selfvoy/ai-core/internal/agent"
	"github.com/selfvoy/ai-core/internal/config"
	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/federation"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/planner"
)

func setupTestServer(t *testing.T) (*Server, *config.Config) {
	cfg := &config.Config{
		Host:         "127.0.0.1",
		Port:         8000,
		ServiceToken: "secret-test-token",
		InsecureDev:  false,
		CORSOrigins:  []string{"*"},
	}

	store := memory.NewInMemoryStore()
	mockLLM := llm.NewMockLLM()
	agentInstance := agent.NewSelfvoyAgent(mockLLM, store)

	srv := NewServer(cfg, agentInstance, store, nil)
	return srv, cfg
}

func TestHealthz(t *testing.T) {
	t.Parallel()

	srv, _ := setupTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()

	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", rec.Code)
	}

	var res map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &res)
	if res["status"] != "ok" {
		t.Errorf("expected status 'ok', got %v", res["status"])
	}
}

func TestWellKnownAuraAgent(t *testing.T) {
	t.Parallel()

	srv, _ := setupTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/.well-known/aura-agent?user=alice", nil)
	rec := httptest.NewRecorder()

	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", rec.Code)
	}

	var meta federation.AgentMeta
	_ = json.Unmarshal(rec.Body.Bytes(), &meta)
	if meta.Protocol != "AFP/1.0" {
		t.Errorf("expected protocol AFP/1.0, got %s", meta.Protocol)
	}
	if meta.PublicKey == "" {
		t.Errorf("expected non-empty public key")
	}
}

func TestAuthEnforcement(t *testing.T) {
	t.Parallel()

	srv, cfg := setupTestServer(t)

	// Missing token -> 401
	body := []byte(`{"text":"hello"}`)
	req := httptest.NewRequest(http.MethodPost, "/v1/planner/understand", bytes.NewReader(body))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 without token, got %d", rec.Code)
	}

	// Valid token -> 200
	reqValid := httptest.NewRequest(http.MethodPost, "/v1/planner/understand", bytes.NewReader(body))
	reqValid.Header.Set("X-Selfvoy-Token", cfg.ServiceToken)
	recValid := httptest.NewRecorder()
	srv.Handler().ServeHTTP(recValid, reqValid)
	if recValid.Code != http.StatusOK {
		t.Fatalf("expected 200 with valid token, got %d", recValid.Code)
	}
}

func TestAgentRunEndpoint(t *testing.T) {
	t.Parallel()

	srv, cfg := setupTestServer(t)

	agentReq := agent.AgentRequest{
		Context: llm.AgentContext{
			Message:     "Напомни завтра в 11:00 купить цветы",
			Preferences: planner.DefaultPreferences(),
		},
		Execute: true,
	}

	reqBytes, _ := json.Marshal(agentReq)
	req := httptest.NewRequest(http.MethodPost, "/v1/agent/run", bytes.NewReader(reqBytes))
	req.Header.Set("X-Selfvoy-Token", cfg.ServiceToken)
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()

	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp agent.AgentResponse
	_ = json.Unmarshal(rec.Body.Bytes(), &resp)

	if resp.Intent != "reminder" {
		t.Errorf("expected intent 'reminder', got '%s'", resp.Intent)
	}
	if len(resp.Actions) == 0 {
		t.Errorf("expected action created for reminder")
	}
}

func TestFederationInboxEndpoint(t *testing.T) {
	t.Parallel()

	srv, _ := setupTestServer(t)
	priv, pub, _ := federation.GenerateKeypair()

	payload := map[string]any{"action": "ping"}
	env, err := federation.SignEnvelope("@sender:remote.net", "@receiver:local.net", "ping", payload, priv)
	if err != nil {
		t.Fatalf("sign envelope error: %v", err)
	}
	env.PublicKey = pub

	envBytes, _ := json.Marshal(env)
	req := httptest.NewRequest(http.MethodPost, "/v1/federation/inbox", bytes.NewReader(envBytes))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()

	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var inboxResp FederationInboxResponse
	_ = json.Unmarshal(rec.Body.Bytes(), &inboxResp)
	if !inboxResp.Ok || inboxResp.Status != "pong" {
		t.Errorf("expected pong response, got status: %s", inboxResp.Status)
	}
}

func TestSpeechTranscribeEndpoint(t *testing.T) {
	t.Parallel()

	srv, cfg := setupTestServer(t)

	speechReq := SpeechJSONRequest{
		Audio:    "dGVzdCBhdWRpbyBkYXRh", // base64 "test audio data"
		Language: "ru",
		Filename: "voice.webm",
	}

	reqBytes, _ := json.Marshal(speechReq)
	req := httptest.NewRequest(http.MethodPost, "/v1/speech/transcribe", bytes.NewReader(reqBytes))
	req.Header.Set("X-Selfvoy-Token", cfg.ServiceToken)
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()

	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var res map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &res)
	if res["provider"] != "mock" {
		t.Errorf("expected mock provider, got %v", res["provider"])
	}
	if res["text"] == "" {
		t.Errorf("expected non-empty transcribed text")
	}
}
