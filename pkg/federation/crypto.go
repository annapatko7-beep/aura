package federation

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"sync"
)

var (
	serverKeyMu         sync.RWMutex
	serverPrivateKeyHex string
	serverPublicKeyHex  string
)

// GenerateKeypair creates a new Ed25519 keypair, returning hex-encoded strings.
// Ed25519 private key is 64 bytes (seed + pub), public key is 32 bytes.
func GenerateKeypair() (privHex, pubHex string, err error) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return "", "", fmt.Errorf("generate ed25519 key: %w", err)
	}
	return hex.EncodeToString(priv), hex.EncodeToString(pub), nil
}

// GetServerKeypair retrieves or initializes the node's Ed25519 keypair.
// It checks AURA_FEDERATION_PRIVATE_KEY environment variable first.
func GetServerKeypair() (privHex, pubHex string) {
	serverKeyMu.RLock()
	if serverPrivateKeyHex != "" && serverPublicKeyHex != "" {
		p, pb := serverPrivateKeyHex, serverPublicKeyHex
		serverKeyMu.RUnlock()
		return p, pb
	}
	serverKeyMu.RUnlock()

	serverKeyMu.Lock()
	defer serverKeyMu.Unlock()

	if serverPrivateKeyHex != "" && serverPublicKeyHex != "" {
		return serverPrivateKeyHex, serverPublicKeyHex
	}

	envKey := os.Getenv("AURA_FEDERATION_PRIVATE_KEY")
	if envKey != "" {
		privBytes, err := hex.DecodeString(envKey)
		if err == nil && len(privBytes) == ed25519.PrivateKeySize {
			privKey := ed25519.PrivateKey(privBytes)
			pubKey := privKey.Public().(ed25519.PublicKey)
			serverPrivateKeyHex = envKey
			serverPublicKeyHex = hex.EncodeToString(pubKey)
			return serverPrivateKeyHex, serverPublicKeyHex
		}
	}

	// Generate runtime node keypair
	priv, pub, err := GenerateKeypair()
	if err == nil {
		serverPrivateKeyHex = priv
		serverPublicKeyHex = pub
	}
	return serverPrivateKeyHex, serverPublicKeyHex
}

// SetServerKeypair overrides the node keypair (used for tests or explicit configuration).
func SetServerKeypair(privHex, pubHex string) {
	serverKeyMu.Lock()
	defer serverKeyMu.Unlock()
	serverPrivateKeyHex = privHex
	serverPublicKeyHex = pubHex
}
