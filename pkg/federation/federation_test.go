package federation

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestGenerateAndGetServerKeypair(t *testing.T) {
	t.Parallel()

	priv, pub, err := GenerateKeypair()
	if err != nil {
		t.Fatalf("GenerateKeypair error: %v", err)
	}
	if len(priv) != 128 { // 64 bytes hex
		t.Errorf("expected 128 hex chars for private key, got %d", len(priv))
	}
	if len(pub) != 64 { // 32 bytes hex
		t.Errorf("expected 64 hex chars for public key, got %d", len(pub))
	}
}

func TestSignAndVerifyEnvelope(t *testing.T) {
	t.Parallel()

	priv, pub, err := GenerateKeypair()
	if err != nil {
		t.Fatalf("keypair error: %v", err)
	}

	payload := map[string]any{
		"action": "propose_meeting",
		"topic":  "Architecture Review",
		"slots_count": 3,
	}

	env, err := SignEnvelope("@alice:domainA.org", "@bob:domainB.org", "negotiate", payload, priv)
	if err != nil {
		t.Fatalf("SignEnvelope error: %v", err)
	}

	valid, reason := VerifyEnvelope(env, pub)
	if !valid {
		t.Fatalf("expected valid envelope, got false: %s", reason)
	}

	// 1. Test replay attack: second verify with same nonce must fail!
	validReplay, reasonReplay := VerifyEnvelope(env, pub)
	if validReplay {
		t.Fatalf("expected replay attack detection, but verification succeeded")
	}
	if reasonReplay != "replay attack detected: nonce already used" {
		t.Errorf("unexpected replay reason: %s", reasonReplay)
	}

	// 2. Tampered payload
	env2, _ := SignEnvelope("@alice:domainA.org", "@bob:domainB.org", "negotiate", payload, priv)
	env2.Payload["topic"] = "Tampered Topic"
	validTampered, _ := VerifyEnvelope(env2, pub)
	if validTampered {
		t.Errorf("expected tampered payload to fail verification")
	}

	// 3. Expired timestamp
	env3, _ := SignEnvelope("@alice:domainA.org", "@bob:domainB.org", "negotiate", payload, priv)
	env3.Timestamp = time.Now().Unix() - 400 // > 300s ago
	validExpired, reasonExpired := VerifyEnvelope(env3, pub)
	if validExpired {
		t.Errorf("expected expired envelope to fail verification")
	}
	if reasonExpired != "envelope expired or timestamp skew too large" {
		t.Errorf("unexpected expired reason: %s", reasonExpired)
	}

	// 4. Wrong public key
	_, wrongPub, _ := GenerateKeypair()
	env4, _ := SignEnvelope("@alice:domainA.org", "@bob:domainB.org", "negotiate", payload, priv)
	validWrongKey, _ := VerifyEnvelope(env4, wrongPub)
	if validWrongKey {
		t.Errorf("expected verification with wrong key to fail")
	}
}

func TestParseAgentHandle(t *testing.T) {
	t.Parallel()

	tests := []struct {
		input      string
		wantUser   string
		wantDomain string
	}{
		{"@alice:aura.net", "alice", "aura.net"},
		{"bob:example.com", "bob", "example.com"},
		{"@charlie", "charlie", "localhost"},
		{"  @dan:nodes.internal  ", "dan", "nodes.internal"},
	}

	for _, tc := range tests {
		u, d := ParseAgentHandle(tc.input)
		if u != tc.wantUser || d != tc.wantDomain {
			t.Errorf("for %s: got user=%s, domain=%s; want %s, %s", tc.input, u, d, tc.wantUser, tc.wantDomain)
		}
	}
}

func TestDiscoveryAndTransport(t *testing.T) {
	priv, pub, _ := GenerateKeypair()

	var receivedEnvelope Envelope
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/.well-known/aura-agent" {
			meta := AgentMeta{
				Protocol:          "AFP/1.0",
				PublicKey:         pub,
				SupportedVersions: []string{"1.0"},
				InboxURL:          "/v1/federation/inbox",
				User:              "bob",
				Capabilities:      []string{"negotiation"},
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(meta)
			return
		}

		if r.URL.Path == "/v1/federation/inbox" {
			_ = json.NewDecoder(r.Body).Decode(&receivedEnvelope)
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "status": "processed"})
			return
		}

		http.NotFound(w, r)
	}))
	defer server.Close()

	// Extract host/port
	serverURL := server.URL
	domain := serverURL[len("http://"):]

	SetServerKeypair(priv, pub)

	client := NewHTTPClient(server.Client())
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	payload := map[string]any{"test": "data"}
	remoteHandle := "@bob:" + domain
	resp, err := client.SendRemoteEnvelope(ctx, "@alice:local", remoteHandle, "test_intent", payload)
	if err != nil {
		t.Fatalf("SendRemoteEnvelope error: %v", err)
	}

	if resp["status"] != "processed" {
		t.Errorf("expected response status 'processed', got %v", resp["status"])
	}

	if receivedEnvelope.Sender != "@alice:local" {
		t.Errorf("expected sender '@alice:local', got '%s'", receivedEnvelope.Sender)
	}
}

func BenchmarkSignEnvelope(b *testing.B) {
	priv, _, _ := GenerateKeypair()
	payload := map[string]any{"key1": "val1", "key2": 42}

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		_, _ = SignEnvelope("@alice:domain.org", "@bob:remote.org", "negotiate", payload, priv)
	}
}

func BenchmarkVerifyEnvelope(b *testing.B) {
	priv, pub, _ := GenerateKeypair()
	payload := map[string]any{"key1": "val1", "key2": 42}
	env, _ := SignEnvelope("@alice:domain.org", "@bob:remote.org", "negotiate", payload, priv)

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		// create a fresh nonce to avoid replay rejection during benchmark
		env.Nonce = "bench_nonce"
		_, _ = VerifyEnvelope(env, pub)
	}
}
