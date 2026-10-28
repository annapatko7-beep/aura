package planner

import (
	"encoding/json"
	"time"
)

const (
	MaxDurationMinutes = 24 * 60
	MaxWindowHours     = 31 * 24
	MaxSlots           = 50
	MaxCalendarEvents  = 2000
)

// TimeWindow represents a time window mentioned or requested.
type TimeWindow struct {
	Start  time.Time `json:"start"`
	End    time.Time `json:"end"`
	Label  string    `json:"label,omitempty"`
	Source string    `json:"source,omitempty"`
}

// AsMap returns a map representation compatible with Python's TimeWindow.as_dict().
func (tw TimeWindow) AsMap() map[string]string {
	return map[string]string{
		"start":  tw.Start.UTC().Format(time.RFC3339),
		"end":    tw.End.UTC().Format(time.RFC3339),
		"label":  tw.Label,
		"source": tw.Source,
	}
}

// Slot represents a discrete available or agreed time slot.
type Slot struct {
	Start  time.Time `json:"start"`
	End    time.Time `json:"end"`
	Score  float64   `json:"score"`
	Reason string    `json:"reason,omitempty"`
	Day    string    `json:"day,omitempty"`   // YYYY-MM-DD
	Local  string    `json:"local,omitempty"` // YYYY-MM-DDTHH:MM
}

// CalendarEvent represents a user's busy event.
type CalendarEvent struct {
	Title string    `json:"title"`
	Start time.Time `json:"start"`
	End   time.Time `json:"end"`
}

// Preferences holds user scheduling and habit preferences.
type Preferences struct {
	Diet                 []string           `json:"diet,omitempty"`
	Transport            string             `json:"transport,omitempty"`
	PreferredHours       []int              `json:"preferred_hours,omitempty"`
	BudgetLimit          float64            `json:"budget_limit,omitempty"`
	City                 string             `json:"city,omitempty"`
	WorkHours            []int              `json:"work_hours,omitempty"`
	Lat                  *float64           `json:"lat,omitempty"`
	Lon                  *float64           `json:"lon,omitempty"`
	CommuteRadiusKm      float64            `json:"commute_radius_km,omitempty"`
	NoisePreference      string             `json:"noise_preference,omitempty"`
	SocialBufferMinutes  int                `json:"social_buffer_minutes,omitempty"`
	MeetingTypeBudget    map[string]float64 `json:"meeting_type_budget,omitempty"`
}

// DefaultPreferences returns standard preferences with default hours.
func DefaultPreferences() Preferences {
	return Preferences{
		Transport:           "walk",
		PreferredHours:      []int{10, 11, 12, 16, 17, 18, 19},
		WorkHours:           []int{9, 10, 11, 12, 13, 14, 15, 16, 17, 18},
		CommuteRadiusKm:     15.0,
		NoisePreference:     "any",
		SocialBufferMinutes: 15,
		MeetingTypeBudget:   make(map[string]float64),
	}
}

// Understanding holds parsed phrase intent, temporal window, and entities.
type Understanding struct {
	Text            string      `json:"text"`
	Intent          string      `json:"intent"`
	Window          *TimeWindow `json:"window,omitempty"`
	DurationMinutes int         `json:"duration_minutes"`
	Person          string      `json:"person,omitempty"`
	Topic           string      `json:"topic,omitempty"`
	Keywords        []string    `json:"keywords,omitempty"`
}

// Custom JSON marshaling for Slot to ensure UTC RFC3339 format.
func (s Slot) MarshalJSON() ([]byte, error) {
	type Alias Slot
	return json.Marshal(&struct {
		Start string `json:"start"`
		End   string `json:"end"`
		Alias
	}{
		Start: s.Start.UTC().Format(time.RFC3339),
		End:   s.End.UTC().Format(time.RFC3339),
		Alias: (Alias)(s),
	})
}
