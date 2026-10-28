package agent

import (
	"context"
	"fmt"
	"math"
	"strings"
	"time"

	"golang.org/x/sync/errgroup"

	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/negotiator"
	"github.com/selfvoy/ai-core/pkg/planner"
)

// SelfvoyAgent orchestrates context collection, LLM reasoning, memory, and A2A negotiations.
type SelfvoyAgent struct {
	llm         llm.Client
	fallbackLLM *llm.MockLLM
	store       memory.Store
	tools       *ToolRunner
}

// NewSelfvoyAgent constructs a SelfvoyAgent with dependencies.
func NewSelfvoyAgent(llmClient llm.Client, store memory.Store) *SelfvoyAgent {
	if store == nil {
		store = memory.NewInMemoryStore()
	}
	if llmClient == nil {
		llmClient = llm.NewMockLLM()
	}
	return &SelfvoyAgent{
		llm:         llmClient,
		fallbackLLM: llm.NewMockLLM(),
		store:       store,
		tools:       NewToolRunner(),
	}
}

// Run executes the complete agent reasoning cycle with parallel Fan-Out context collection.
func (a *SelfvoyAgent) Run(ctx context.Context, req *AgentRequest) (*AgentResponse, error) {
	agentCtx := &req.Context
	now := time.Now().UTC()
	if agentCtx.Now != nil && !agentCtx.Now.IsZero() {
		now = agentCtx.Now.UTC()
	}

	loc := time.UTC
	if agentCtx.Timezone != "" {
		if l, err := time.LoadLocation(agentCtx.Timezone); err == nil {
			loc = l
		}
	}
	localNow := now.In(loc)

	// --- Parallel Fan-Out context collection via errgroup ---
	var (
		understanding   planner.Understanding
		relevantMemories []memory.MemoryEntry
		busyCalendar    *planner.BusyIndex
	)

	g, gCtx := errgroup.WithContext(ctx)

	// 1. Natural Language Intent & Time parsing
	g.Go(func() error {
		understanding = planner.Understand(agentCtx.Message, localNow)
		return nil
	})

	// 2. Hybrid RAG Memory retrieval
	g.Go(func() error {
		userKey := agentCtx.Email
		if userKey == "" && agentCtx.UserID != 0 {
			userKey = fmt.Sprintf("%d", agentCtx.UserID)
		}
		var err error
		relevantMemories, err = memory.LoadContextMemory(gCtx, a.store, userKey, agentCtx.Message, agentCtx.Memory, 20)
		return err
	})

	// 3. Calendar validation and indexing
	g.Go(func() error {
		windowEnd := now.Add(96 * time.Hour)
		busyCalendar = planner.NewBusyIndex(agentCtx.Calendar, now, windowEnd)
		return nil
	})

	if err := g.Wait(); err != nil {
		return nil, fmt.Errorf("context collection failed: %w", err)
	}

	_ = busyCalendar // available for fast local checks

	// Call LLM (with graceful fallback to MockLLM on failure or timeout)
	payload, usedLLM := a.complete(ctx, agentCtx, &understanding)

	intent := payload.Intent
	if intent == "" {
		intent = understanding.Intent
	}
	reply := payload.Reply
	confidence := payload.Confidence
	if len(relevantMemories) > 0 {
		confidence = math.Min(1.0, confidence+0.05)
	}

	actions := payload.Actions
	var results []ToolResult

	// Execute tools only if explicitly instructed and not in dry_run
	if len(actions) > 0 && req.Execute && !req.DryRun {
		results = make([]ToolResult, 0, len(actions))
		for _, act := range actions {
			res := a.tools.Run(act, agentCtx)
			results = append(results, res)
		}
	}

	plan := a.buildPlan(intent, understanding, actions, reply)

	// A2A Inter-Agent negotiation
	var a2aProposal *llm.A2AProposal
	if payload.A2A != nil && payload.A2A.Target != "" {
		a2aProposal = a.negotiateFor(agentCtx, payload.A2A, understanding, req)
		if a2aProposal != nil && len(a2aProposal.Slots) > 0 {
			slot := a2aProposal.Slots[0]
			placeName := "место уточняется"
			if pName, ok := a2aProposal.Place["name"].(string); ok && pName != "" {
				placeName = pName
			}
			reply = fmt.Sprintf("%s Договорился на %s — %s.", reply, slot.Start.Format("02.01 15:04"), placeName)
		}
	}

	// Memory updates
	extractedFacts := memory.ExtractFacts(agentCtx.Message)
	allUpdates := memory.MergeEntries(payload.MemoryUpdates, extractedFacts)

	if len(allUpdates) > 0 && req.Execute && !req.DryRun {
		userKey := agentCtx.Email
		if userKey == "" && agentCtx.UserID != 0 {
			userKey = fmt.Sprintf("%d", agentCtx.UserID)
		}
		if userKey != "" {
			_, _ = a.store.Save(ctx, userKey, allUpdates)
		}
	}

	return &AgentResponse{
		Reply:         reply,
		Intent:        intent,
		Confidence:    math.Round(confidence*1000) / 1000,
		Plan:          plan,
		Actions:       actions,
		Results:       results,
		MemoryUpdates: allUpdates,
		A2A:           a2aProposal,
		LLM:           usedLLM,
	}, nil
}

func (a *SelfvoyAgent) complete(ctx context.Context, agentCtx *llm.AgentContext, u *planner.Understanding) (*llm.LLMPayload, string) {
	timeoutCtx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()

	payload, err := a.llm.CompleteJSON(timeoutCtx, agentCtx, u)
	if err == nil && payload != nil {
		return payload, a.llm.Name()
	}

	// Fallback to deterministic rules
	fallbackPayload, _ := a.fallbackLLM.CompleteJSON(ctx, agentCtx, u)
	fallbackName := a.fallbackLLM.Name()
	if a.llm.Name() != a.fallbackLLM.Name() {
		fallbackName = a.llm.Name() + "-fallback"
	}
	return fallbackPayload, fallbackName
}

func (a *SelfvoyAgent) buildPlan(intent string, u planner.Understanding, actions []llm.ToolCall, reply string) Plan {
	steps := []Step{
		{
			Title:  "Понял задачу",
			Detail: u.Text,
		},
	}

	if u.Window != nil {
		steps = append(steps, Step{
			Title:  fmt.Sprintf("Окно: %s", u.Window.Label),
			Detail: fmt.Sprintf("%s → %s", u.Window.Start.Format(time.RFC3339), u.Window.End.Format(time.RFC3339)),
			At:     &u.Window.Start,
		})
	}

	for _, act := range actions {
		steps = append(steps, Step{
			Title:  fmt.Sprintf("Действие: %s", act.Tool),
			Detail: fmt.Sprintf("%v", act.Args),
			Tool:   act.Tool,
		})
	}

	steps = append(steps, Step{
		Title:  "Ответ пользователю",
		Detail: reply,
	})

	var winMap map[string]string
	if u.Window != nil {
		winMap = u.Window.AsMap()
	}

	return Plan{
		Goal:   u.Topic,
		Intent: intent,
		Steps:  steps,
		Constraints: map[string]any{
			"duration_minutes": u.DurationMinutes,
			"person":           u.Person,
			"window":           winMap,
		},
	}
}

func (a *SelfvoyAgent) negotiateFor(
	agentCtx *llm.AgentContext,
	raw *llm.A2AProposal,
	u planner.Understanding,
	req *AgentRequest,
) *llm.A2AProposal {
	peer := a.findPeer(raw.Target, req.Peers, agentCtx)
	if peer == nil {
		return &llm.A2AProposal{
			Target:  raw.Target,
			Intent:  raw.Intent,
			Topic:   raw.Topic,
			Slots:   nil,
			Message: "Selfvoy предлагает встретиться.",
		}
	}

	now := time.Now().UTC()
	start := now
	if u.Window != nil && !u.Window.Start.IsZero() {
		start = u.Window.Start
	}
	end := start.Add(96 * time.Hour)
	if u.Window != nil && !u.Window.End.IsZero() {
		end = u.Window.End
	}

	duration := u.DurationMinutes
	if duration < 15 {
		duration = 60
	}

	win := planner.TimeWindow{Start: start, End: end, Label: "окно запроса"}
	mine := planner.FindFreeSlots(win, agentCtx.Calendar, agentCtx.Preferences, duration, 30, 8)
	theirs := planner.FindFreeSlots(win, peer.Calendar, peer.Preferences, duration, 30, 8)
	shared := planner.IntersectSlots(mine, theirs, duration)

	place := a.choosePlace(agentCtx, peer)

	if len(shared) == 0 {
		return &llm.A2AProposal{
			Target:  peer.Email,
			Intent:  "schedule_meeting",
			Topic:   raw.Topic,
			Slots:   mine,
			Place:   place,
			Message: "Общих окон пока нет — вот мои свободные слоты, выбери удобный.",
		}
	}

	best := shared[0]
	return &llm.A2AProposal{
		Target:  peer.Email,
		Intent:  "schedule_meeting",
		Topic:   raw.Topic,
		Slots:   shared,
		Place:   place,
		Message: fmt.Sprintf("Мы с Selfvoy %s договорились: %s, %v.", peer.DisplayName, best.Start.Format(time.RFC3339), place["name"]),
	}
}

func (a *SelfvoyAgent) choosePlace(mine *llm.AgentContext, peer *llm.AgentContext) map[string]any {
	args := map[string]any{
		"city": mine.Preferences.City,
		"diet": mine.Preferences.Diet,
	}

	if peer != nil && mine.Preferences.Lat != nil && mine.Preferences.Lon != nil &&
		peer.Preferences.Lat != nil && peer.Preferences.Lon != nil {
		midLat := (*mine.Preferences.Lat + *peer.Preferences.Lat) / 2.0
		midLon := (*mine.Preferences.Lon + *peer.Preferences.Lon) / 2.0
		args["midpoint"] = map[string]any{"lat": midLat, "lon": midLon}
	}

	res := a.tools.Run(llm.ToolCall{
		ID:   "p1",
		Tool: "find_cafe",
		Args: args,
	}, mine)

	if res.Ok {
		if results, ok := res.Data["results"].([]map[string]any); ok && len(results) > 0 {
			return results[0]
		}
	}
	return map[string]any{"name": "Aurora Roasters"}
}

func (a *SelfvoyAgent) findPeer(target string, peers []llm.AgentContext, ctx *llm.AgentContext) *llm.AgentContext {
	needle := strings.ToLower(strings.TrimSpace(target))
	if needle == "" {
		return nil
	}

	for _, p := range peers {
		if strings.EqualFold(p.Email, needle) || strings.EqualFold(p.DisplayName, needle) {
			return &p
		}
	}

	for _, c := range ctx.Contacts {
		name, _ := c["name"].(string)
		email, _ := c["email"].(string)
		if strings.EqualFold(name, needle) || strings.EqualFold(email, needle) {
			return &llm.AgentContext{
				DisplayName: name,
				Email:       email,
				Preferences: planner.DefaultPreferences(),
			}
		}
	}
	return nil
}

// Negotiate handles multi-turn or single-turn A2A negotiation requests.
func (a *SelfvoyAgent) Negotiate(ctx context.Context, req *NegotiateRequest) (*NegotiateResponse, error) {
	sessionID := req.SessionID
	if sessionID == "" {
		sessionID = fmt.Sprintf("neg-%d", time.Now().UnixNano())
	}
	roundNum := req.RoundNumber
	if roundNum <= 0 {
		roundNum = 1
	}

	session := &negotiator.NegotiationSession{
		ID:              sessionID,
		Topic:           req.Topic,
		InitiatorEmail:  req.Initiator.Email,
		ResponderEmail:  req.Responder.Email,
		DurationMinutes: req.DurationMinutes,
		WindowHours:     req.WindowHours,
		RoundNumber:     roundNum,
		Status:          negotiator.StateProposed,
	}

	initCtx := negotiator.ParticipantContext{
		Email:       req.Initiator.Email,
		DisplayName: req.Initiator.DisplayName,
		Preferences: req.Initiator.Preferences,
		Calendar:    req.Initiator.Calendar,
	}
	respCtx := negotiator.ParticipantContext{
		Email:       req.Responder.Email,
		DisplayName: req.Responder.DisplayName,
		Preferences: req.Responder.Preferences,
		Calendar:    req.Responder.Calendar,
	}

	place := a.choosePlace(&req.Initiator, &req.Responder)

	if req.MultiTurn {
		session = negotiator.RunNegotiationRound(session, initCtx, respCtx, place, time.Now().UTC())
	} else {
		// Single turn direct intersection check
		session = negotiator.RunNegotiationRound(session, initCtx, respCtx, place, time.Now().UTC())
	}

	return &NegotiateResponse{
		Accepted:     session.Accepted,
		Status:       session.Status,
		Slot:         session.SelectedSlot,
		Place:        session.SelectedPlace,
		Message:      session.Message,
		CounterSlots: session.CounterSlots,
		RoundsLog:    session.History,
		SessionID:    session.ID,
	}, nil
}

// OfferSlots generates public booking slots matching the owner's calendar and constraints.
func (a *SelfvoyAgent) OfferSlots(ctx context.Context, req *SlotsRequest) (*SlotsResponse, error) {
	now := time.Now().UTC()
	if req.Context.Now != nil && req.Context.Now.After(now) {
		now = req.Context.Now.UTC()
	}

	windowHours := req.WindowHours
	if windowHours <= 0 {
		windowHours = 168
	}
	window := planner.TimeWindow{
		Start: now,
		End:   now.Add(time.Duration(windowHours) * time.Hour),
		Label: "окно приглашения",
	}

	slots := planner.OfferSlots(
		window,
		req.Context.Calendar,
		req.Context.Preferences,
		req.Context.Timezone,
		req.DurationMinutes,
		req.MaxSlots,
		req.PerDay,
		now,
		time.Duration(req.LeadMinutes)*time.Minute,
		req.Weekends,
	)

	return &SlotsResponse{Slots: slots}, nil
}
