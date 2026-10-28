package negotiator

import (
	"fmt"
	"time"

	"github.com/selfvoy/ai-core/pkg/planner"
)

// RunNegotiationRound advances the state machine by executing one turn between two agent digital twins.
func RunNegotiationRound(
	session *NegotiationSession,
	initiator ParticipantContext,
	responder ParticipantContext,
	candidatePlace map[string]any,
	now time.Time,
) *NegotiationSession {
	if now.IsZero() {
		now = time.Now().UTC()
	} else {
		now = now.UTC()
	}

	windowHours := session.WindowHours
	if windowHours < 1 {
		windowHours = 96
	}
	duration := session.DurationMinutes
	if duration < 15 {
		duration = 60
	}

	start := now
	end := start.Add(time.Duration(windowHours) * time.Hour)
	window := planner.TimeWindow{
		Start: start,
		End:   end,
		Label: fmt.Sprintf("раунд %d", session.RoundNumber),
	}

	initSlots := planner.FindFreeSlots(window, initiator.Calendar, initiator.Preferences, duration, 30, 8)
	respSlots := planner.FindFreeSlots(window, responder.Calendar, responder.Preferences, duration, 30, 8)
	shared := planner.IntersectSlots(initSlots, respSlots, duration)

	place := candidatePlace
	if place == nil {
		place = make(map[string]any)
	}

	placeName := "место согласовано"
	if name, ok := place["name"].(string); ok && name != "" {
		placeName = name
	}

	// 1. Direct match: AGREED
	if len(shared) > 0 {
		bestSlot := shared[0]
		session.Status = StateAgreed
		session.Accepted = true
		session.SelectedSlot = &bestSlot
		session.SelectedPlace = place

		if len(shared) > 1 {
			endIdx := 4
			if endIdx > len(shared) {
				endIdx = len(shared)
			}
			session.CounterSlots = shared[1:endIdx]
		}

		dateStr := bestSlot.Start.Format("02.01 в 15:04")
		session.Message = fmt.Sprintf("Договорились: %s, %s.", dateStr, placeName)
		session.History = append(session.History, RoundLog{
			Round:  session.RoundNumber,
			Action: "agree",
			Slot:   &bestSlot,
			Note:   "найдено прямое пересечение графиков",
		})
		session.UpdatedAt = now
		return session
	}

	// 2. Multi-turn compromise rounds: CONVERGING or COUNTER_OFFER
	if session.RoundNumber < MaxNegotiationRounds {
		shorterDuration := duration - 15
		if shorterDuration < 30 {
			shorterDuration = 30
		}

		if shorterDuration < duration {
			flexInit := planner.FindFreeSlots(window, initiator.Calendar, initiator.Preferences, shorterDuration, 30, 8)
			flexResp := planner.FindFreeSlots(window, responder.Calendar, responder.Preferences, shorterDuration, 30, 8)
			flexShared := planner.IntersectSlots(flexInit, flexResp, shorterDuration)

			if len(flexShared) > 0 {
				bestSlot := flexShared[0]
				session.Status = StateConverging
				session.SelectedSlot = &bestSlot
				session.SelectedPlace = place
				session.CounterSlots = flexShared
				session.RoundNumber++
				session.Message = fmt.Sprintf("Нашли компромиссный слот на %d мин: %s.", shorterDuration, bestSlot.Start.Format("02.01 в 15:04"))
				session.History = append(session.History, RoundLog{
					Round:    session.RoundNumber,
					Action:   "compromise_duration",
					Slot:     &bestSlot,
					Duration: shorterDuration,
				})
				session.UpdatedAt = now
				return session
			}
		}

		// Counter-offer with responder's top available slots
		session.Status = StateCounterOffer
		endIdx := 3
		if endIdx > len(respSlots) {
			endIdx = len(respSlots)
		}
		session.CounterSlots = respSlots[:endIdx]
		session.RoundNumber++
		session.History = append(session.History, RoundLog{
			Round:  session.RoundNumber - 1,
			Action: "counter_offer",
			Slots:  session.CounterSlots,
		})
		session.Message = "Прямых пересечений нет. Аура собеседника предложила встречные свободные слоты."
		session.UpdatedAt = now
		return session
	}

	// 3. Exceeded maximum rounds: REJECTED (escalate to human)
	session.Status = StateRejected
	session.Accepted = false
	session.Message = "Не удалось автоматически найти общее окно за 5 раундов. Требуется выбор пользователя."
	session.History = append(session.History, RoundLog{
		Round:  session.RoundNumber,
		Action: "escalate_to_human",
	})
	session.UpdatedAt = now
	return session
}

// RunFullNegotiation simulates the entire multi-turn conversation between agents to conclusion.
func RunFullNegotiation(
	session *NegotiationSession,
	initiator ParticipantContext,
	responder ParticipantContext,
	candidatePlace map[string]any,
	now time.Time,
) *NegotiationSession {
	for session.Status != StateAgreed && session.Status != StateRejected {
		session = RunNegotiationRound(session, initiator, responder, candidatePlace, now)
	}
	return session
}
