package federation

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

const DiscoveryCacheTTL = 1 * time.Hour

// AgentMeta holds public metadata retrieved from /.well-known/aura-agent.
type AgentMeta struct {
	Protocol          string   `json:"protocol"`
	PublicKey         string   `json:"public_key"`
	SupportedVersions []string `json:"supported_versions"`
	InboxURL          string   `json:"inbox_url"`
	User              string   `json:"user"`
	Capabilities      []string `json:"capabilities"`
}

type cachedMeta struct {
	cachedAt time.Time
	meta     *AgentMeta
}

// DiscoveryCache stores remote node metadata with TTL and thread-safety.
type DiscoveryCache struct {
	mu      sync.RWMutex
	entries map[string]cachedMeta
}

// NewDiscoveryCache initializes an in-memory cache.
func NewDiscoveryCache() *DiscoveryCache {
	return &DiscoveryCache{
		entries: make(map[string]cachedMeta),
	}
}

func (c *DiscoveryCache) Get(key string) (*AgentMeta, bool) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	entry, ok := c.entries[key]
	if !ok || time.Since(entry.cachedAt) > DiscoveryCacheTTL {
		return nil, false
	}
	return entry.meta, true
}

func (c *DiscoveryCache) Set(key string, meta *AgentMeta) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.entries[key] = cachedMeta{
		cachedAt: time.Now(),
		meta:     meta,
	}
}

var defaultDiscoveryCache = NewDiscoveryCache()

// ParseAgentHandle parses @username:domain.tld into username and domain.
func ParseAgentHandle(handle string) (user, domain string) {
	clean := strings.TrimSpace(handle)
	clean = strings.TrimPrefix(clean, "@")
	if idx := strings.Index(clean, ":"); idx != -1 {
		return strings.TrimSpace(clean[:idx]), strings.TrimSpace(clean[idx+1:])
	}
	return clean, "localhost"
}

// DiscoverRemoteAgent fetches remote agent public keys and endpoints via well-known.
func DiscoverRemoteAgent(ctx context.Context, client *http.Client, agentHandle string) (*AgentMeta, error) {
	user, domain := ParseAgentHandle(agentHandle)
	cacheKey := fmt.Sprintf("%s@%s", user, domain)

	if cached, ok := defaultDiscoveryCache.Get(cacheKey); ok {
		return cached, nil
	}

	scheme := "https"
	if strings.HasPrefix(domain, "localhost") || strings.HasPrefix(domain, "127.0.0.1") {
		scheme = "http"
	}

	endpoint := fmt.Sprintf("%s://%s/.well-known/aura-agent?user=%s", scheme, domain, url.QueryEscape(user))

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, fmt.Errorf("create discovery request: %w", err)
	}

	if client == nil {
		client = defaultHTTPClient
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("discovery request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("remote node returned status %d", resp.StatusCode)
	}

	var meta AgentMeta
	if err := json.NewDecoder(resp.Body).Decode(&meta); err != nil {
		return nil, fmt.Errorf("decode discovery response: %w", err)
	}

	if meta.PublicKey == "" {
		return nil, fmt.Errorf("remote agent metadata has no public key")
	}

	defaultDiscoveryCache.Set(cacheKey, &meta)
	return &meta, nil
}
