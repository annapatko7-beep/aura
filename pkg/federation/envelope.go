package federation

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"time"
)

const (
	MaxTimestampSkewSeconds = 300 // 5 minutes window
	NonceTTLSeconds         = 600 // 10 minutes cache retention
)

// Envelope represents an authenticated AFP message container.
type Envelope struct {
	Sender    string         `json:"sender"`
	Recipient string         `json:"recipient"`
	Timestamp int64          `json:"timestamp"`
	Nonce     string         `json:"nonce"`
	Intent    string         `json:"intent"`
	Payload   map[string]any `json:"payload"`
	Signature string         `json:"signature"`
	PublicKey string         `json:"public_key,omitempty"`
}

// NonceCache guards against replay attacks by tracking recently seen nonces.
type NonceCache struct {
	mu     sync.RWMutex
	nonces map[string]int64
}

// NewNonceCache creates a thread-safe nonce tracker.
func NewNonceCache() *NonceCache {
	return &NonceCache{
		nonces: make(map[string]int64),
	}
}

// CheckAndRecord tests if nonce was already used; if not, records it.
func (c *NonceCache) CheckAndRecord(nonce string, nowUnix int64) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	// Periodic purge of expired nonces
	for k, ts := range c.nonces {
		if nowUnix-ts > NonceTTLSeconds {
			delete(c.nonces, k)
		}
	}

	if _, exists := c.nonces[nonce]; exists {
		return false // replay attack
	}

	c.nonces[nonce] = nowUnix
	return true
}

var defaultNonceCache = NewNonceCache()

// CanonicalBytes builds the exact string to be signed or verified.
// Standard Go json.Marshal sorts map keys alphabetically with no whitespace separators.
func CanonicalBytes(sender, recipient string, timestamp int64, nonce, intent string, payload map[string]any) ([]byte, error) {
	if payload == nil {
		payload = make(map[string]any)
	}

	payloadDump, err := json.Marshal(payload)
	if err != nil {
		return nil, fmt.Errorf("canonical json marshal: %w", err)
	}

	payloadHashBytes := sha256.Sum256(payloadDump)
	payloadHashHex := hex.EncodeToString(payloadHashBytes[:])

	raw := fmt.Sprintf("%s|%s|%d|%s|%s|%s", sender, recipient, timestamp, nonce, intent, payloadHashHex)
	return []byte(raw), nil
}

// SignEnvelope constructs a cryptographically signed AFP envelope using Ed25519.
func SignEnvelope(sender, recipient, intent string, payload map[string]any, privateKeyHex string) (*Envelope, error) {
	privBytes, err := hex.DecodeString(privateKeyHex)
	if err != nil || len(privBytes) != ed25519.PrivateKeySize {
		return nil, fmt.Errorf("invalid ed25519 private key: %w", err)
	}
	privKey := ed25519.PrivateKey(privBytes)

	now := time.Now().Unix()

	nonceBytes := make([]byte, 16)
	if _, err := rand.Read(nonceBytes); err != nil {
		return nil, fmt.Errorf("generate nonce: %w", err)
	}
	nonce := hex.EncodeToString(nonceBytes)

	dataToSign, err := CanonicalBytes(sender, recipient, now, nonce, intent, payload)
	if err != nil {
		return nil, err
	}

	sigBytes := ed25519.Sign(privKey, dataToSign)
	sigHex := hex.EncodeToString(sigBytes)

	return &Envelope{
		Sender:    sender,
		Recipient: recipient,
		Timestamp: now,
		Nonce:     nonce,
		Intent:    intent,
		Payload:   payload,
		Signature: sigHex,
	}, nil
}

// VerifyEnvelope validates the signature, time window, and nonce freshness of an AFP envelope.
func VerifyEnvelope(env *Envelope, publicKeyHex string) (bool, string) {
	if env == nil {
		return false, "empty envelope"
	}
	if env.Sender == "" || env.Recipient == "" || env.Nonce == "" || env.Intent == "" || env.Signature == "" {
		return false, "missing required envelope fields"
	}

	now := time.Now().Unix()
	skew := now - env.Timestamp
	if skew < -MaxTimestampSkewSeconds || skew > MaxTimestampSkewSeconds {
		return false, "envelope expired or timestamp skew too large"
	}

	if !defaultNonceCache.CheckAndRecord(env.Nonce, now) {
		return false, "replay attack detected: nonce already used"
	}

	pubBytes, err := hex.DecodeString(publicKeyHex)
	if err != nil || len(pubBytes) != ed25519.PublicKeySize {
		return false, "invalid public key hex"
	}
	pubKey := ed25519.PublicKey(pubBytes)

	sigBytes, err := hex.DecodeString(env.Signature)
	if err != nil || len(sigBytes) != ed25519.SignatureSize {
		return false, "invalid signature hex length"
	}

	dataToVerify, err := CanonicalBytes(env.Sender, env.Recipient, env.Timestamp, env.Nonce, env.Intent, env.Payload)
	if err != nil {
		return false, fmt.Sprintf("canonical bytes error: %v", err)
	}

	if !ed25519.Verify(pubKey, dataToVerify, sigBytes) {
		return false, "signature verification failed"
	}

	return true, "valid"
}
