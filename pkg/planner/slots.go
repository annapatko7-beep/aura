package planner

import (
	"math"
	"sort"
	"time"
)

// ScoreSlot evaluates how well a slot fits user preferences.
func ScoreSlot(start time.Time, prefs Preferences, busy OverlapChecker, workHours bool, now time.Time) float64 {
	score := 0.5
	h := start.Hour()

	for _, prefH := range prefs.PreferredHours {
		if prefH == h {
			score += 0.25
			break
		}
	}

	if workHours {
		for _, workH := range prefs.WorkHours {
			if workH == h {
				score += 0.1
				break
			}
		}
	}

	// Saturday is 6, Sunday is 0 in Go weekday
	wd := start.Weekday()
	if wd == time.Saturday || wd == time.Sunday {
		score -= 0.05
	}

	if busy != nil && busy.Overlaps(start, start.Add(30*time.Minute)) {
		score -= 0.4
	}

	if !now.IsZero() && start.After(now) {
		hoursAhead := start.Sub(now).Hours()
		proximityBoost := 0.1 - hoursAhead/240.0
		if proximityBoost > 0 {
			score += proximityBoost
		}
	}

	if score < 0.0 {
		score = 0.0
	} else if score > 1.0 {
		score = 1.0
	}

	return math.Round(score*1000) / 1000
}

// FindFreeSlots locates unallocated intervals within window not clashing with calendar events.
func FindFreeSlots(
	window TimeWindow,
	calendar []CalendarEvent,
	prefs Preferences,
	durationMinutes int,
	stepMinutes int,
	maxSlots int,
) []Slot {
	if durationMinutes < 15 {
		durationMinutes = 60
	}
	if stepMinutes < 15 {
		stepMinutes = 30
	}
	if maxSlots <= 0 {
		maxSlots = 5
	}

	duration := time.Duration(durationMinutes) * time.Minute
	step := time.Duration(stepMinutes) * time.Minute

	cursor := window.Start.UTC()
	// Round up to next 30-minute mark if needed for clean scheduling
	if cursor.Minute()%30 != 0 {
		extra := 30 - (cursor.Minute() % 30)
		cursor = cursor.Add(time.Duration(extra) * time.Minute).Truncate(time.Minute)
	}

	busyEnd := window.End.Add(duration + 30*time.Minute)
	busy := NewBusyIndex(calendar, cursor, busyEnd)

	results := make([]Slot, 0, maxSlots*2)
	now := time.Now().UTC()
	guard := 0

	preferredSet := make(map[int]struct{}, len(prefs.PreferredHours))
	for _, ph := range prefs.PreferredHours {
		preferredSet[ph] = struct{}{}
	}

	for !cursor.Add(duration).After(window.End) && guard < 500 {
		guard++
		slotEnd := cursor.Add(duration)

		if !busy.Overlaps(cursor, slotEnd) {
			score := ScoreSlot(cursor, prefs, busy, true, now)
			reason := "свободно"
			if _, ok := preferredSet[cursor.Hour()]; ok {
				reason = "удобное время"
			}

			results = append(results, Slot{
				Start:  cursor,
				End:    slotEnd,
				Score:  score,
				Reason: reason,
			})
		}
		cursor = cursor.Add(step)
	}

	sort.Slice(results, func(i, j int) bool {
		if results[i].Score == results[j].Score {
			return results[i].Start.Before(results[j].Start)
		}
		return results[i].Score > results[j].Score
	})

	if len(results) > maxSlots {
		return results[:maxSlots]
	}
	return results
}

// IntersectSlots computes the intersection of available slots between two agents (A2A Core).
func IntersectSlots(a []Slot, b []Slot, durationMinutes int) []Slot {
	if durationMinutes < 15 {
		durationMinutes = 60
	}
	duration := time.Duration(durationMinutes) * time.Minute
	merged := make([]Slot, 0, len(a)*len(b))

	for _, left := range a {
		lStart := left.Start.UTC()
		lEnd := left.End.UTC()

		for _, right := range b {
			rStart := right.Start.UTC()
			rEnd := right.End.UTC()

			start := lStart
			if rStart.After(start) {
				start = rStart
			}

			end := lEnd
			if rEnd.Before(end) {
				end = rEnd
			}

			if end.Sub(start) >= duration {
				avgScore := math.Round(((left.Score+right.Score)/2.0)*1000) / 1000
				merged = append(merged, Slot{
					Start:  start,
					End:    start.Add(duration),
					Score:  avgScore,
					Reason: "общее окно в графиках",
				})
			}
		}
	}

	sort.Slice(merged, func(i, j int) bool {
		if merged[i].Score == merged[j].Score {
			return merged[i].Start.Before(merged[j].Start)
		}
		return merged[i].Score > merged[j].Score
	})

	return merged
}
