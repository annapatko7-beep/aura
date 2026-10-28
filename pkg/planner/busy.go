package planner

import (
	"sort"
	"time"
)

// OverlapChecker defines the interface for checking whether an interval intersects busy time.
type OverlapChecker interface {
	Overlaps(start, end time.Time) bool
}

// Interval represents a continuous block of busy time [Start, End).
type Interval struct {
	Start time.Time
	End   time.Time
}

// BusyIndex is an optimized interval index using binary search (O(log n) per check).
type BusyIndex struct {
	starts []time.Time
	ends   []time.Time
}

// NewBusyIndex builds an index from events within an optional [windowStart, windowEnd) boundary.
// It filters out irrelevant events, sorts, and merges overlapping/contiguous intervals.
func NewBusyIndex(calendar []CalendarEvent, windowStart, windowEnd time.Time) *BusyIndex {
	hasStartFilter := !windowStart.IsZero()
	hasEndFilter := !windowEnd.IsZero()

	intervals := make([]Interval, 0, len(calendar))
	for _, event := range calendar {
		bStart := event.Start.UTC()
		bEnd := event.End.UTC()

		if bEnd.Before(bStart) {
			continue // skip corrupted events
		}

		if hasStartFilter && !bEnd.After(windowStart) {
			continue
		}
		if hasEndFilter && !bStart.Before(windowEnd) {
			continue
		}

		intervals = append(intervals, Interval{Start: bStart, End: bEnd})
	}

	sort.Slice(intervals, func(i, j int) bool {
		if intervals[i].Start.Equal(intervals[j].Start) {
			return intervals[i].End.Before(intervals[j].End)
		}
		return intervals[i].Start.Before(intervals[j].Start)
	})

	starts := make([]time.Time, 0, len(intervals))
	ends := make([]time.Time, 0, len(intervals))

	for _, interval := range intervals {
		n := len(ends)
		if n > 0 && !interval.Start.After(ends[n-1]) {
			if interval.End.After(ends[n-1]) {
				ends[n-1] = interval.End
			}
		} else {
			starts = append(starts, interval.Start)
			ends = append(ends, interval.End)
		}
	}

	return &BusyIndex{
		starts: starts,
		ends:   ends,
	}
}

// Overlaps returns true if [aStart, aEnd) intersects any busy interval in O(log n).
func (b *BusyIndex) Overlaps(aStart, aEnd time.Time) bool {
	if len(b.starts) == 0 {
		return false
	}
	// Find first interval where ends[i] > aStart
	idx := sort.Search(len(b.ends), func(i int) bool {
		return b.ends[i].After(aStart)
	})
	return idx < len(b.starts) && b.starts[idx].Before(aEnd)
}

// Len returns number of merged busy intervals.
func (b *BusyIndex) Len() int {
	return len(b.starts)
}
