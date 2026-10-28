package federation

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

var defaultHTTPClient = &http.Client{
	Timeout: 10 * time.Second,
	Transport: &http.Transport{
		Proxy: http.ProxyFromEnvironment,
		DialContext: (&net.Dialer{
			Timeout:   5 * time.Second,
			KeepAlive: 30 * time.Second,
		}).DialContext,
		ForceAttemptHTTP2:     true,
		MaxIdleConns:          100,
		MaxIdleConnsPerHost:   20,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   5 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
	},
}

// Client defines the federation sender interface for testing and decoupling.
type Client interface {
	SendRemoteEnvelope(ctx context.Context, senderHandle, remoteHandle, intent string, payload map[string]any) (map[string]any, error)
	DiscoverRemoteAgent(ctx context.Context, agentHandle string) (*AgentMeta, error)
}

// HTTPClient implements Client over HTTP/2.
type HTTPClient struct {
	client *http.Client
}

// NewHTTPClient creates a client with custom or default transport.
func NewHTTPClient(c *http.Client) *HTTPClient {
	if c == nil {
		c = defaultHTTPClient
	}
	return &HTTPClient{client: c}
}

// DiscoverRemoteAgent delegates to DiscoverRemoteAgent using the configured client.
func (h *HTTPClient) DiscoverRemoteAgent(ctx context.Context, agentHandle string) (*AgentMeta, error) {
	return DiscoverRemoteAgent(ctx, h.client, agentHandle)
}

// SendRemoteEnvelope delivers a signed AFP envelope to the remote agent's inbox.
func (h *HTTPClient) SendRemoteEnvelope(ctx context.Context, senderHandle, remoteHandle, intent string, payload map[string]any) (map[string]any, error) {
	meta, err := h.DiscoverRemoteAgent(ctx, remoteHandle)
	if err != nil {
		return nil, fmt.Errorf("discover remote agent '%s': %w", remoteHandle, err)
	}

	if meta.InboxURL == "" {
		return nil, fmt.Errorf("remote agent has no inbox_url configured")
	}

	_, domain := ParseAgentHandle(remoteHandle)
	inbox := meta.InboxURL
	if strings.HasPrefix(inbox, "/") {
		scheme := "https"
		if strings.HasPrefix(domain, "localhost") || strings.HasPrefix(domain, "127.0.0.1") {
			scheme = "http"
		}
		inbox = fmt.Sprintf("%s://%s%s", scheme, domain, inbox)
	}

	privKeyHex, _ := GetServerKeypair()
	envelope, err := SignEnvelope(senderHandle, remoteHandle, intent, payload, privKeyHex)
	if err != nil {
		return nil, fmt.Errorf("sign envelope: %w", err)
	}

	bodyBytes, err := json.Marshal(envelope)
	if err != nil {
		return nil, fmt.Errorf("marshal envelope: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, inbox, bytes.NewReader(bodyBytes))
	if err != nil {
		return nil, fmt.Errorf("create inbox request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := h.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("envelope delivery failed: %w", err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 1024*1024))
	if err != nil {
		return nil, fmt.Errorf("read response: %w", err)
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("remote server returned %d: %s", resp.StatusCode, string(respBody))
	}

	var result map[string]any
	if err := json.Unmarshal(respBody, &result); err != nil {
		return map[string]any{"raw": string(respBody)}, nil
	}

	return result, nil
}

// SendRemoteEnvelope helper function using default HTTP client.
func SendRemoteEnvelope(ctx context.Context, senderHandle, remoteHandle, intent string, payload map[string]any) (map[string]any, error) {
	return NewHTTPClient(defaultHTTPClient).SendRemoteEnvelope(ctx, senderHandle, remoteHandle, intent, payload)
}

// ResolveInboxURL ensures an inbox URL has an absolute scheme and host.
func ResolveInboxURL(inboxURL, domain string) string {
	if !strings.HasPrefix(inboxURL, "/") {
		return inboxURL
	}
	u, err := url.Parse(inboxURL)
	if err == nil && u.IsAbs() {
		return inboxURL
	}
	scheme := "https"
	if strings.HasPrefix(domain, "localhost") || strings.HasPrefix(domain, "127.0.0.1") {
		scheme = "http"
	}
	return fmt.Sprintf("%s://%s%s", scheme, domain, inboxURL)
}
