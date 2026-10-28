package planner

import (
	"testing"
	"time"
)

func TestBusyIndex(t *testing.T) {
	t.Parallel()

	base := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)
	events := []CalendarEvent{
		{Title: "E1", Start: base, End: base.Add(1 * time.Hour)},                      // 10:00 - 11:00
		{Title: "E2", Start: base.Add(30 * time.Minute), End: base.Add(2 * time.Hour)}, // 10:30 - 12:00 (overlaps with E1, merges to 10:00-12:00)
		{Title: "E3", Start: base.Add(3 * time.Hour), End: base.Add(4 * time.Hour)},   // 13:00 - 14:00
	}

	idx := NewBusyIndex(events, time.Time{}, time.Time{})
	if idx.Len() != 2 {
		t.Fatalf("expected 2 merged intervals, got %d", idx.Len())
	}

	// Test overlaps
	// Overlaps 10:00-12:00
	if !idx.Overlaps(base.Add(15*time.Minute), base.Add(45*time.Minute)) {
		t.Errorf("expected overlap during 10:15 - 10:45")
	}
	// Free window 12:00-13:00
	if idx.Overlaps(base.Add(2*time.Hour), base.Add(3*time.Hour)) {
		t.Errorf("expected no overlap during 12:00 - 13:00")
	}
	// Boundary touch: [9:00, 10:00) does NOT overlap [10:00, 12:00)
	if idx.Overlaps(base.Add(-1*time.Hour), base) {
		t.Errorf("boundary touch 9:00 - 10:00 should not overlap")
	}
	// Boundary touch: [12:00, 13:00) does NOT overlap [10:00, 12:00) or [13:00, 14:00)
	if idx.Overlaps(base.Add(2*time.Hour), base.Add(3*time.Hour)) {
		t.Errorf("window 12:00 - 13:00 should be free")
	}
}

func TestFindFreeSlots(t *testing.T) {
	t.Parallel()

	start := time.Date(2026, 10, 20, 9, 0, 0, 0, time.UTC)
	end := start.Add(6 * time.Hour) // 09:00 to 15:00

	window := TimeWindow{Start: start, End: end, Label: "workday"}
	prefs := DefaultPreferences()

	// Busy from 10:00 to 11:30
	calendar := []CalendarEvent{
		{Title: "Meeting", Start: start.Add(1 * time.Hour), End: start.Add(2*time.Hour + 30*time.Minute)},
	}

	slots := FindFreeSlots(window, calendar, prefs, 60, 30, 5)
	if len(slots) == 0 {
		t.Fatalf("expected free slots, got none")
	}

	for _, s := range slots {
		// Check that none overlap 10:00 - 11:30
		busyStart := start.Add(1 * time.Hour)
		busyEnd := start.Add(2*time.Hour + 30*time.Minute)
		if s.Start.Before(busyEnd) && busyStart.Before(s.End) {
			t.Errorf("slot %v overlaps with busy meeting", s)
		}
		if s.End.Sub(s.Start) != 60*time.Minute {
			t.Errorf("expected duration 60m, got %v", s.End.Sub(s.Start))
		}
	}
}

func TestIntersectSlots(t *testing.T) {
	t.Parallel()

	base := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)

	// Agent A slots: 10:00-12:00, 14:00-16:00
	slotsA := []Slot{
		{Start: base, End: base.Add(2 * time.Hour), Score: 0.8},
		{Start: base.Add(4 * time.Hour), End: base.Add(6 * time.Hour), Score: 0.7},
	}

	// Agent B slots: 11:00-13:00, 15:00-17:00
	slotsB := []Slot{
		{Start: base.Add(1 * time.Hour), End: base.Add(3 * time.Hour), Score: 0.9},
		{Start: base.Add(5 * time.Hour), End: base.Add(7 * time.Hour), Score: 0.6},
	}

	// Expected overlaps with duration 60m:
	// 1) 11:00 - 12:00 (from 10:00-12:00 and 11:00-13:00)
	// 2) 15:00 - 16:00 (from 14:00-16:00 and 15:00-17:00)
	shared := IntersectSlots(slotsA, slotsB, 60)
	if len(shared) != 2 {
		t.Fatalf("expected 2 shared slots, got %d", len(shared))
	}

	expectedFirstStart := base.Add(1 * time.Hour)
	if !shared[0].Start.Equal(expectedFirstStart) {
		t.Errorf("expected first shared slot start %v, got %v", expectedFirstStart, shared[0].Start)
	}
	expectedFirstScore := 0.85 // (0.8 + 0.9) / 2
	if shared[0].Score != expectedFirstScore {
		t.Errorf("expected score %v, got %v", expectedFirstScore, shared[0].Score)
	}
}

func TestOfferSlots(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 10, 20, 8, 0, 0, 0, time.UTC) // Tuesday
	window := TimeWindow{
		Start: now,
		End:   now.Add(48 * time.Hour),
	}
	prefs := DefaultPreferences()

	slots := OfferSlots(window, nil, prefs, "Europe/Moscow", 60, 4, 2, now, 1*time.Hour, nil)
	if len(slots) == 0 {
		t.Fatalf("expected offer slots, got none")
	}

	for _, s := range slots {
		if s.Day == "" {
			t.Errorf("slot Day is empty")
		}
		if s.Local == "" {
			t.Errorf("slot Local is empty")
		}
		if s.Start.Before(now.Add(1 * time.Hour)) {
			t.Errorf("slot %v starts before lead time threshold", s)
		}
	}
}

func TestWeeklySeries(t *testing.T) {
	t.Parallel()

	start := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC)
	occurrences := WeeklySeries(start, "Europe/Moscow", 60, 3, 7)
	if len(occurrences) != 3 {
		t.Fatalf("expected 3 occurrences, got %d", len(occurrences))
	}

	if occurrences[0].Day != "2026-10-20" {
		t.Errorf("expected first day 2026-10-20, got %s", occurrences[0].Day)
	}
	if occurrences[1].Day != "2026-10-27" {
		t.Errorf("expected second day 2026-10-27, got %s", occurrences[1].Day)
	}
	if occurrences[2].Day != "2026-11-03" {
		t.Errorf("expected third day 2026-11-03, got %s", occurrences[2].Day)
	}
}

func TestUnderstand(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 10, 20, 10, 0, 0, 0, time.UTC) // Tuesday

	tests := []struct {
		phrase         string
		expectedIntent string
		expectedPerson string
		minDuration    int
	}{
		{
			phrase:         "Напомни завтра в 15:00 купить билеты на поезд",
			expectedIntent: "reminder",
			expectedPerson: "",
			minDuration:    60,
		},
		{
			phrase:         "Давай встретимся с Анной завтра в 12:00 на полчаса обсудить проект",
			expectedIntent: "schedule_meeting",
			expectedPerson: "Анной",
			minDuration:    30,
		},
		{
			phrase:         "Запиши заметку: пароль от роутера 12345",
			expectedIntent: "note",
			expectedPerson: "",
			minDuration:    60,
		},
	}

	for _, tc := range tests {
		u := Understand(tc.phrase, now)
		if u.Intent != tc.expectedIntent {
			t.Errorf("for '%s': expected intent %s, got %s", tc.phrase, tc.expectedIntent, u.Intent)
		}
		if tc.expectedPerson != "" && u.Person != tc.expectedPerson {
			t.Errorf("for '%s': expected person %s, got %s", tc.phrase, tc.expectedPerson, u.Person)
		}
		if u.DurationMinutes != tc.minDuration {
			t.Errorf("for '%s': expected duration %d, got %d", tc.phrase, tc.minDuration, u.DurationMinutes)
		}
	}
}

func TestReminderText(t *testing.T) {
	t.Parallel()

	cleaned := ReminderText("Напомни завтра в 10:00 позвонить юристу")
	if cleaned != "Позвонить юристу" {
		t.Errorf("expected 'Позвонить юристу', got '%s'", cleaned)
	}
}

func BenchmarkFindFreeSlots(b *testing.B) {
	start := time.Date(2026, 10, 20, 8, 0, 0, 0, time.UTC)
	end := start.Add(7 * 24 * time.Hour) // 1 week
	window := TimeWindow{Start: start, End: end, Label: "week"}
	prefs := DefaultPreferences()

	// 50 busy events across the week
	calendar := make([]CalendarEvent, 50)
	for i := 0; i < 50; i++ {
		evStart := start.Add(time.Duration(i*3) * time.Hour)
		calendar[i] = CalendarEvent{
			Title: "Event",
			Start: evStart,
			End:   evStart.Add(1 * time.Hour),
		}
	}

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		_ = FindFreeSlots(window, calendar, prefs, 60, 30, 10)
	}
}

func BenchmarkIntersectSlots(b *testing.B) {
	base := time.Date(2026, 10, 20, 9, 0, 0, 0, time.UTC)
	slotsA := make([]Slot, 20)
	slotsB := make([]Slot, 20)
	for i := 0; i < 20; i++ {
		slotsA[i] = Slot{Start: base.Add(time.Duration(i*2) * time.Hour), End: base.Add(time.Duration(i*2+2) * time.Hour), Score: 0.8}
		slotsB[i] = Slot{Start: base.Add(time.Duration(i*2+1) * time.Hour), End: base.Add(time.Duration(i*2+3) * time.Hour), Score: 0.9}
	}

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		_ = IntersectSlots(slotsA, slotsB, 60)
	}
}
