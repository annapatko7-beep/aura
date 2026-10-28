package planner

import (
	"fmt"
	"math"
	"sort"
	"time"
)

const (
	OfferStepMinutes        = 30
	BookingPerDayThreshold  = 2
	DefaultOfferLeadMinutes = 60
)

// defaultWorkHours: 9..18 inclusive
var defaultWorkHours = []int{9, 10, 11, 12, 13, 14, 15, 16, 17, 18}

func parseLocation(tzName string) *time.Location {
	if tzName == "" {
		return time.UTC
	}
	loc, err := time.LoadLocation(tzName)
	if err != nil {
		return time.UTC
	}
	return loc
}

func ceilLocalHalfHour(moment time.Time, loc *time.Location) time.Time {
	local := moment.In(loc)
	// Truncate seconds/nanoseconds
	rounded := time.Date(local.Year(), local.Month(), local.Day(), local.Hour(), local.Minute(), 0, 0, loc)
	if rounded.Before(local) {
		rounded = rounded.Add(time.Minute)
	}
	extra := rounded.Minute() % OfferStepMinutes
	if extra != 0 {
		rounded = rounded.Add(time.Duration(OfferStepMinutes-extra) * time.Minute)
	}
	return rounded.UTC()
}

func withinWorkHours(start, end time.Time, loc *time.Location, workHours map[int]struct{}) bool {
	probe := start
	quarter := 15 * time.Minute
	for probe.Before(end) {
		h := probe.In(loc).Hour()
		if _, ok := workHours[h]; !ok {
			return false
		}
		probe = probe.Add(quarter)
	}
	lastH := end.Add(-1 * time.Microsecond).In(loc).Hour()
	_, ok := workHours[lastH]
	return ok
}

type timeInterval struct {
	start time.Time
	end   time.Time
}

// OfferSlots computes available slots for an invite link or booking page.
func OfferSlots(
	window TimeWindow,
	calendar []CalendarEvent,
	prefs Preferences,
	tzName string,
	durationMinutes int,
	maxSlots int,
	perDay int,
	now time.Time,
	lead time.Duration,
	weekends *bool,
) []Slot {
	if maxSlots <= 0 {
		return nil
	}
	if perDay <= 0 {
		perDay = 2
	}
	if durationMinutes < 15 {
		durationMinutes = 60
	}
	if lead < 0 {
		lead = 0
	}

	loc := parseLocation(tzName)
	if now.IsZero() {
		now = time.Now().UTC()
	} else {
		now = now.UTC()
	}

	duration := time.Duration(durationMinutes) * time.Minute
	step := time.Duration(OfferStepMinutes) * time.Minute

	workList := prefs.WorkHours
	if len(workList) == 0 {
		workList = defaultWorkHours
	}
	workMap := make(map[int]struct{}, len(workList))
	for _, h := range workList {
		workMap[h] = struct{}{}
	}

	preferredMap := make(map[int]struct{}, len(prefs.PreferredHours))
	for _, h := range prefs.PreferredHours {
		preferredMap[h] = struct{}{}
	}

	wStart := window.Start.UTC()
	wEnd := window.End.UTC()
	busy := NewBusyIndex(calendar, wStart, wEnd)

	earliest := wStart
	if leadThreshold := now.Add(lead); leadThreshold.After(earliest) {
		earliest = leadThreshold
	}
	cursor := ceilLocalHalfHour(earliest, loc)

	var weekdays []timeInterval
	var weekendSlots []timeInterval
	guard := 0

	for !cursor.Add(duration).After(wEnd) && guard < 5000 {
		guard++
		end := cursor.Add(duration)

		if withinWorkHours(cursor, end, loc, workMap) && !busy.Overlaps(cursor, end) {
			localDay := cursor.In(loc).Weekday()
			isWeekend := localDay == time.Saturday || localDay == time.Sunday
			if isWeekend {
				weekendSlots = append(weekendSlots, timeInterval{start: cursor, end: end})
			} else {
				weekdays = append(weekdays, timeInterval{start: cursor, end: end})
			}
		}
		cursor = cursor.Add(step)
	}

	var candidates []timeInterval
	if weekends == nil {
		if len(weekdays) > 0 {
			candidates = weekdays
		} else {
			candidates = weekendSlots
		}
	} else if *weekends {
		candidates = make([]timeInterval, 0, len(weekdays)+len(weekendSlots))
		candidates = append(candidates, weekdays...)
		candidates = append(candidates, weekendSlots...)
		sort.Slice(candidates, func(i, j int) bool {
			return candidates[i].start.Before(candidates[j].start)
		})
	} else {
		candidates = weekdays
	}

	if len(candidates) == 0 {
		return []Slot{}
	}

	// Group by local owner date (YYYY-MM-DD)
	byDay := make(map[string][]timeInterval)
	var dayKeys []string
	for _, cand := range candidates {
		dStr := cand.start.In(loc).Format("2006-01-02")
		if _, exists := byDay[dStr]; !exists {
			dayKeys = append(dayKeys, dStr)
		}
		byDay[dStr] = append(byDay[dStr], cand)
	}
	sort.Strings(dayKeys)

	// In each day, rank candidates: preferred hours first, then chronologically
	picksByDay := make([][]timeInterval, 0, len(dayKeys))
	for _, day := range dayKeys {
		slotsInDay := byDay[day]
		sort.Slice(slotsInDay, func(i, j int) bool {
			hI := slotsInDay[i].start.In(loc).Hour()
			hJ := slotsInDay[j].start.In(loc).Hour()
			_, prefI := preferredMap[hI]
			_, prefJ := preferredMap[hJ]
			if prefI != prefJ {
				return prefI // true comes first
			}
			return slotsInDay[i].start.Before(slotsInDay[j].start)
		})

		var chosen []timeInterval
		for _, cand := range slotsInDay {
			if len(chosen) >= perDay {
				break
			}
			// Check overlap with already chosen slots for this day
			clash := false
			for _, ch := range chosen {
				if cand.start.Before(ch.end) && ch.start.Before(cand.end) {
					clash = true
					break
				}
			}
			if !clash {
				chosen = append(chosen, cand)
			}
		}
		picksByDay = append(picksByDay, chosen)
	}

	var selected []timeInterval
	if perDay > BookingPerDayThreshold {
		// Fill days sequentially
		for _, chosen := range picksByDay {
			for _, item := range chosen {
				if len(selected) >= maxSlots {
					break
				}
				selected = append(selected, item)
			}
			if len(selected) >= maxSlots {
				break
			}
		}
	} else {
		// Round-robin: 1st best slot of each day, then 2nd best, etc.
		for rank := 0; rank < perDay; rank++ {
			for _, chosen := range picksByDay {
				if rank < len(chosen) && len(selected) < maxSlots {
					selected = append(selected, chosen[rank])
				}
			}
			if len(selected) >= maxSlots {
				break
			}
		}
	}

	sort.Slice(selected, func(i, j int) bool {
		return selected[i].start.Before(selected[j].start)
	})

	results := make([]Slot, 0, len(selected))
	for _, item := range selected {
		localTime := item.start.In(loc)
		h := localTime.Hour()
		_, isPreferred := preferredMap[h]

		score := 0.6
		if isPreferred {
			score += 0.3
		}
		wd := localTime.Weekday()
		isWeekend := wd == time.Saturday || wd == time.Sunday
		if isWeekend {
			score -= 0.05
		}

		hoursAhead := item.start.Sub(now).Hours()
		if hoursAhead < 0 {
			hoursAhead = 0
		}
		proximity := 0.05 - hoursAhead/2000.0
		if proximity > 0 {
			score += proximity
		}

		var reason string
		if isWeekend {
			if weekends != nil && *weekends {
				reason = "выходной день"
			} else {
				reason = "выходной день (в будни свободных окон нет)"
			}
		} else if isPreferred {
			reason = "удобное время"
		} else {
			reason = "рабочее время"
		}

		finalScore := math.Round(math.Max(0.0, math.Min(1.0, score))*1000) / 1000

		results = append(results, Slot{
			Start:  item.start.UTC(),
			End:    item.end.UTC(),
			Score:  finalScore,
			Reason: reason,
			Day:    localTime.Format("2006-01-02"),
			Local:  fmt.Sprintf("%04d-%02d-%02dT%02d:%02d", localTime.Year(), localTime.Month(), localTime.Day(), localTime.Hour(), localTime.Minute()),
		})
	}

	return results
}
