package api

import (
	"encoding/base64"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/selfvoy/ai-core/internal/agent"
	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/federation"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/planner"
)

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	s.writeJSON(w, http.StatusOK, map[string]any{
		"status":         "ok",
		"service":        "selfvoy-ai",
		"llm":            s.cfg.LLMProvider,
		"tools_backend":  "native",
		"memory_backend": s.cfg.MemoryBackend,
		"time":           time.Now().UTC().Format(time.RFC3339),
	})
}

func (s *Server) handleWellKnownAuraAgent(w http.ResponseWriter, r *http.Request) {
	_, pubHex := federation.GetServerKeypair()
	user := r.URL.Query().Get("user")
	if user == "" {
		user = "default"
	}

	s.writeJSON(w, http.StatusOK, federation.AgentMeta{
		Protocol:          "AFP/1.0",
		PublicKey:         pubHex,
		SupportedVersions: []string{"1.0"},
		InboxURL:          "/v1/federation/inbox",
		User:              user,
		Capabilities:      []string{"negotiation", "calendar_sync", "places_search", "booking"},
	})
}

type FederationInboxResponse struct {
	Ok     bool           `json:"ok"`
	Status string         `json:"status"`
	Reply  map[string]any `json:"reply,omitempty"`
	Error  string         `json:"error,omitempty"`
}

func (s *Server) handleFederationInbox(w http.ResponseWriter, r *http.Request) {
	var env federation.Envelope
	if err := s.decodeJSON(r, &env); err != nil {
		s.writeJSON(w, http.StatusBadRequest, FederationInboxResponse{
			Ok:    false,
			Error: "invalid json envelope",
		})
		return
	}

	if env.Sender == "" {
		s.writeJSON(w, http.StatusBadRequest, FederationInboxResponse{
			Ok:    false,
			Error: "missing sender handle",
		})
		return
	}

	// Resolve sender public key via discovery or envelope
	pubKey := env.PublicKey
	if pubKey == "" {
		meta, err := federation.DiscoverRemoteAgent(r.Context(), nil, env.Sender)
		if err == nil && meta != nil {
			pubKey = meta.PublicKey
		}
	}

	if pubKey == "" {
		s.writeJSON(w, http.StatusBadRequest, FederationInboxResponse{
			Ok:    false,
			Error: "cannot resolve public key for sender",
		})
		return
	}

	valid, reason := federation.VerifyEnvelope(&env, pubKey)
	if !valid {
		s.writeJSON(w, http.StatusUnauthorized, FederationInboxResponse{
			Ok:    false,
			Error: "signature verification failed: " + reason,
		})
		return
	}

	intent := env.Intent
	if intent == "ping" || intent == "hello" {
		s.writeJSON(w, http.StatusOK, FederationInboxResponse{
			Ok:     true,
			Status: "pong",
			Reply: map[string]any{
				"node":     "aura",
				"protocol": "AFP/1.0",
				"time":     time.Now().UTC().Format(time.RFC3339),
			},
		})
		return
	}

	if intent == "negotiate" || intent == "schedule_meeting" || intent == "propose_meeting" {
		s.writeJSON(w, http.StatusOK, FederationInboxResponse{
			Ok:     true,
			Status: "processed",
			Reply: map[string]any{
				"message":   "Aura accepted proposal from " + env.Sender + " for " + env.Recipient,
				"intent":    intent,
				"timestamp": time.Now().UTC().Format(time.RFC3339),
			},
		})
		return
	}

	s.writeJSON(w, http.StatusOK, FederationInboxResponse{
		Ok:     true,
		Status: "accepted",
		Reply:  map[string]any{"intent": intent},
	})
}

type FederationSendRequest struct {
	RecipientHandle string         `json:"recipient_handle"`
	SenderHandle    string         `json:"sender_handle"`
	Intent          string         `json:"intent"`
	Payload         map[string]any `json:"payload"`
}

func (s *Server) handleFederationSend(w http.ResponseWriter, r *http.Request) {
	var req FederationSendRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "invalid json"})
		return
	}

	res, err := federation.SendRemoteEnvelope(r.Context(), req.SenderHandle, req.RecipientHandle, req.Intent, req.Payload)
	if err != nil {
		s.writeJSON(w, http.StatusBadGateway, map[string]any{"error": err.Error()})
		return
	}

	s.writeJSON(w, http.StatusOK, res)
}

func (s *Server) handleAgentRun(w http.ResponseWriter, r *http.Request) {
	var req agent.AgentRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	resp, err := s.agent.Run(r.Context(), &req)
	if err != nil {
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"detail": err.Error()})
		return
	}

	s.writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handleAgentNegotiate(w http.ResponseWriter, r *http.Request) {
	var req agent.NegotiateRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	resp, err := s.agent.Negotiate(r.Context(), &req)
	if err != nil {
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"detail": err.Error()})
		return
	}

	s.writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handleAgentSlots(w http.ResponseWriter, r *http.Request) {
	var req agent.SlotsRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	resp, err := s.agent.OfferSlots(r.Context(), &req)
	if err != nil {
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"detail": err.Error()})
		return
	}

	s.writeJSON(w, http.StatusOK, resp)
}

type SeriesRequest struct {
	Timezone        string    `json:"timezone"`
	Start           time.Time `json:"start"`
	DurationMinutes int       `json:"duration_minutes"`
	Count           int       `json:"count"`
	IntervalDays    int       `json:"interval_days"`
}

type SeriesResponse struct {
	Occurrences []planner.Slot `json:"occurrences"`
}

func (s *Server) handleAgentSeries(w http.ResponseWriter, r *http.Request) {
	var req SeriesRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	tz := req.Timezone
	if tz == "" {
		tz = "UTC"
	}
	slots := planner.WeeklySeries(req.Start, tz, req.DurationMinutes, req.Count, req.IntervalDays)
	s.writeJSON(w, http.StatusOK, SeriesResponse{Occurrences: slots})
}

type MemoryExtractRequest struct {
	UserKey string           `json:"user_key"`
	Message string           `json:"message"`
	Context llm.AgentContext `json:"context"`
	Commit  bool             `json:"commit"`
}

type MemoryExtractResponse struct {
	Entries []memory.MemoryEntry `json:"entries"`
	Stored  []memory.MemoryEntry `json:"stored"`
}

func (s *Server) handleMemoryExtract(w http.ResponseWriter, r *http.Request) {
	var req MemoryExtractRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "invalid input"})
		return
	}

	msg := req.Message
	if msg == "" {
		msg = req.Context.Message
	}
	entries := memory.ExtractFacts(msg)

	var stored []memory.MemoryEntry
	if len(entries) > 0 && req.Commit && s.store != nil {
		userKey := req.UserKey
		if userKey == "" {
			userKey = req.Context.Email
		}
		var err error
		stored, err = s.store.Save(r.Context(), userKey, entries)
		if err != nil {
			stored = entries
		}
	}

	s.writeJSON(w, http.StatusOK, MemoryExtractResponse{
		Entries: entries,
		Stored:  stored,
	})
}

func (s *Server) handleMemoryLoad(w http.ResponseWriter, r *http.Request) {
	userKey := r.PathValue("user_key")
	query := r.URL.Query().Get("query")

	entries, err := s.store.Load(r.Context(), userKey)
	if err != nil {
		s.writeJSON(w, http.StatusOK, map[string]any{"user_key": userKey, "entries": []any{}})
		return
	}

	if query != "" {
		entries = memory.BM25Rank(query, entries, 10)
	}

	s.writeJSON(w, http.StatusOK, map[string]any{
		"user_key": userKey,
		"entries":  entries,
	})
}

type PlannerSlotsRequest struct {
	Window          *planner.TimeWindow     `json:"window,omitempty"`
	Calendar        []planner.CalendarEvent `json:"calendar"`
	Preferences     planner.Preferences     `json:"preferences"`
	DurationMinutes int                     `json:"duration_minutes"`
	MaxSlots        int                     `json:"max_slots"`
}

func (s *Server) handlePlannerSlots(w http.ResponseWriter, r *http.Request) {
	var req PlannerSlotsRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	win := req.Window
	if win == nil {
		now := time.Now().UTC()
		win = &planner.TimeWindow{
			Start: now,
			End:   now.Add(24 * time.Hour),
			Label: "окно",
		}
	}

	dur := req.DurationMinutes
	if dur <= 0 {
		dur = 60
	}
	maxSlots := req.MaxSlots
	if maxSlots <= 0 {
		maxSlots = 5
	}

	slots := planner.FindFreeSlots(*win, req.Calendar, req.Preferences, dur, 30, maxSlots)
	s.writeJSON(w, http.StatusOK, map[string]any{
		"window": win.AsMap(),
		"slots":  slots,
	})
}

type UnderstandRequest struct {
	Text string `json:"text"`
}

func (s *Server) handlePlannerUnderstand(w http.ResponseWriter, r *http.Request) {
	var req UnderstandRequest
	if err := s.decodeJSON(r, &req); err != nil {
		s.writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"detail": "invalid input"})
		return
	}

	res := planner.Understand(req.Text, time.Now().UTC())
	s.writeJSON(w, http.StatusOK, res)
}

func (s *Server) handleListTools(w http.ResponseWriter, r *http.Request) {
	toolList := []map[string]any{
		{"name": "check_calendar", "description": "Проверить свободные слоты в календаре"},
		{"name": "suggest_time", "description": "Предложить подходящее время встречи"},
		{"name": "find_cafe", "description": "Подобрать место для встречи"},
		{"name": "create_reminder", "description": "Создать напоминание"},
		{"name": "create_note", "description": "Сохранить заметку"},
		{"name": "send_message", "description": "Отправить сообщение (server only)"},
		{"name": "send_email", "description": "Отправить email (server only)"},
		{"name": "book_table", "description": "Забронировать стол (server only)"},
	}
	s.writeJSON(w, http.StatusOK, map[string]any{"tools": toolList})
}

type SpeechJSONRequest struct {
	Audio       string `json:"audio"`
	AudioBase64 string `json:"audio_base64"`
	Language    string `json:"language"`
	Filename    string `json:"filename"`
}

func (s *Server) handleSpeechTranscribe(w http.ResponseWriter, r *http.Request) {
	var (
		audioData []byte
		language  string
		filename  = "audio.webm"
	)

	contentType := r.Header.Get("Content-Type")
	if strings.HasPrefix(contentType, "multipart/form-data") {
		if err := r.ParseMultipartForm(10 * 1024 * 1024); err != nil {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "invalid multipart data"})
			return
		}
		file, header, err := r.FormFile("file")
		if err != nil {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "missing audio file"})
			return
		}
		defer file.Close()
		audioData, err = io.ReadAll(io.LimitReader(file, 10*1024*1024))
		if err != nil {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "error reading audio"})
			return
		}
		filename = header.Filename
		language = r.FormValue("language")
	} else {
		var jsonReq SpeechJSONRequest
		if err := s.decodeJSON(r, &jsonReq); err != nil {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "invalid json body"})
			return
		}
		b64 := jsonReq.Audio
		if b64 == "" {
			b64 = jsonReq.AudioBase64
		}
		if b64 == "" {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "missing audio data"})
			return
		}
		var err error
		audioData, err = base64.StdEncoding.DecodeString(b64)
		if err != nil {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"detail": "invalid base64 audio"})
			return
		}
		language = jsonReq.Language
		if jsonReq.Filename != "" {
			filename = jsonReq.Filename
		}
	}

	res, err := s.stt.Transcribe(r.Context(), audioData, language, filename)
	if err != nil {
		s.writeJSON(w, http.StatusBadGateway, map[string]string{"detail": err.Error()})
		return
	}

	s.writeJSON(w, http.StatusOK, res)
}
