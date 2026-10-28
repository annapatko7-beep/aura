package api

import (
	"crypto/subtle"
	"encoding/json"
	"net/http"
	"sync"

	"github.com/selfvoy/ai-core/internal/agent"
	"github.com/selfvoy/ai-core/internal/config"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/speech"
)

// Server encapsulates the HTTP API routing and dependencies.
type Server struct {
	cfg   *config.Config
	agent *agent.SelfvoyAgent
	store memory.Store
	stt   speech.Provider
	mux   *http.ServeMux
}

// JSON buffer pool to minimize heap allocations for high-throughput endpoints.
var bufferPool = sync.Pool{
	New: func() any {
		b := make([]byte, 0, 4096)
		return &b
	},
}

// NewServer initializes routes, middleware, and handlers.
func NewServer(cfg *config.Config, agentInstance *agent.SelfvoyAgent, store memory.Store, sttProvider speech.Provider) *Server {
	if sttProvider == nil {
		sttProvider = speech.NewMockSTT()
	}
	s := &Server{
		cfg:   cfg,
		agent: agentInstance,
		store: store,
		stt:   sttProvider,
		mux:   http.NewServeMux(),
	}
	s.routes()
	return s
}

// Handler returns the HTTP handler with CORS middleware.
func (s *Server) Handler() http.Handler {
	return s.corsMiddleware(s.mux)
}

func (s *Server) routes() {
	// Public Health & Discovery
	s.mux.HandleFunc("GET /healthz", s.handleHealthz)
	s.mux.HandleFunc("GET /.well-known/aura-agent", s.handleWellKnownAuraAgent)
	s.mux.HandleFunc("POST /v1/federation/inbox", s.handleFederationInbox)

	// Protected Federation
	s.mux.HandleFunc("POST /v1/federation/send", s.requireAuth(s.handleFederationSend))

	// Protected Agent
	s.mux.HandleFunc("POST /v1/agent/run", s.requireAuth(s.handleAgentRun))
	s.mux.HandleFunc("POST /v1/agent/negotiate", s.requireAuth(s.handleAgentNegotiate))
	s.mux.HandleFunc("POST /v1/agent/slots", s.requireAuth(s.handleAgentSlots))
	s.mux.HandleFunc("POST /v1/agent/series", s.requireAuth(s.handleAgentSeries))

	// Protected Memory & RAG
	s.mux.HandleFunc("POST /v1/memory/extract", s.requireAuth(s.handleMemoryExtract))
	s.mux.HandleFunc("GET /v1/memory/{user_key}", s.requireAuth(s.handleMemoryLoad))

	// Protected Planner
	s.mux.HandleFunc("POST /v1/planner/slots", s.requireAuth(s.handlePlannerSlots))
	s.mux.HandleFunc("POST /v1/planner/understand", s.requireAuth(s.handlePlannerUnderstand))

	// Protected Speech / STT
	s.mux.HandleFunc("POST /v1/speech/transcribe", s.requireAuth(s.handleSpeechTranscribe))

	// Protected Tools
	s.mux.HandleFunc("GET /v1/tools", s.requireAuth(s.handleListTools))
}

func (s *Server) requireAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		isLoopback := s.cfg.Host == "127.0.0.1" || s.cfg.Host == "localhost" || s.cfg.Host == "::1"
		if s.cfg.InsecureDev && isLoopback {
			next(w, r)
			return
		}

		expected := s.cfg.ServiceToken
		if expected == "" {
			s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{
				"detail": "AI service token is not configured",
			})
			return
		}

		token := r.Header.Get("X-Selfvoy-Token")
		if token == "" || subtle.ConstantTimeCompare([]byte(token), []byte(expected)) != 1 {
			s.writeJSON(w, http.StatusUnauthorized, map[string]string{
				"detail": "invalid service token",
			})
			return
		}

		next(w, r)
	}
}

func (s *Server) corsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origins := s.cfg.CORSOrigins
		if len(origins) == 1 && origins[0] == "*" {
			w.Header().Set("Access-Control-Allow-Origin", "*")
		} else if len(origins) > 0 {
			reqOrigin := r.Header.Get("Origin")
			for _, o := range origins {
				if o == reqOrigin {
					w.Header().Set("Access-Control-Allow-Origin", reqOrigin)
					break
				}
			}
		}

		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Selfvoy-Token")

		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}

		next.ServeHTTP(w, r)
	})
}

func (s *Server) writeJSON(w http.ResponseWriter, statusCode int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	_ = json.NewEncoder(w).Encode(data)
}

func (s *Server) decodeJSON(r *http.Request, dst any) error {
	defer r.Body.Close()
	return json.NewDecoder(r.Body).Decode(dst)
}
