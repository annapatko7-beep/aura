package planner

import (
	"fmt"
	"time"
)

// WeeklySeries generates repeating meeting occurrences on the same local wall-clock time.
func WeeklySeries(start time.Time, tzName string, durationMinutes int, count int, intervalDays int) []Slot {
	if count < 1 {
		count = 1
	}
	if intervalDays < 1 {
		intervalDays = 7
	}
	if durationMinutes < 15 {
		durationMinutes = 60
	}

	loc := parseLocation(tzName)
	origin := start.UTC()
	first := origin.In(loc)
	duration := time.Duration(durationMinutes) * time.Minute

	res := make([]Slot, 0, count)
	for i := 0; i < count; i++ {
		var begin time.Time
		if i == 0 {
			begin = origin
		} else {
			// Construct same local wall clock time with added days in loc
			y, m, d := first.Date()
			h, min, s := first.Clock()
			targetDay := d + i*intervalDays
			localDate := time.Date(y, m, targetDay, h, min, s, 0, loc)
			begin = localDate.UTC()
		}

		actual := begin.In(loc)
		res = append(res, Slot{
			Start: begin,
			End:   begin.Add(duration),
			Day:   actual.Format("2006-01-02"),
			Local: fmt.Sprintf("%04d-%02d-%02dT%02d:%02d", actual.Year(), actual.Month(), actual.Day(), actual.Hour(), actual.Minute()),
		})
	}

	return res
}
