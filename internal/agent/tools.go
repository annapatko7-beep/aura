package agent

import (
	"fmt"
	"math"
	"strings"
	"time"

	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/planner"
)

var ServerOnlyTools = map[string]struct{}{
	"send_message": {},
	"send_email":   {},
	"book_table":   {},
}

// CafeItem represents a venue candidate.
type CafeItem struct {
	Name   string   `json:"name"`
	City   string   `json:"city"`
	Tags   []string `json:"tags"`
	Rating float64  `json:"rating"`
	Price  int      `json:"price"`
	Lat    float64  `json:"lat"`
	Lon    float64  `json:"lon"`
}

var cafeDatabase = []CafeItem{
	{Name: "Кофе на полпути", City: "Керкраде", Tags: []string{"coffee", "quiet", "vegan"}, Rating: 4.7, Price: 2, Lat: 50.861, Lon: 6.064},
	{Name: "Aurora Roasters", City: "Керкраде", Tags: []string{"coffee", "workspace", "gluten_free"}, Rating: 4.6, Price: 3, Lat: 50.857, Lon: 6.071},
	{Name: "Grey Garden", City: "Херлен", Tags: []string{"brunch", "vegan", "quiet"}, Rating: 4.5, Price: 2, Lat: 50.888, Lon: 5.978},
	{Name: "Steak House Nord", City: "Херлен", Tags: []string{"dinner", "meat"}, Rating: 4.4, Price: 4, Lat: 50.885, Lon: 5.982},
	{Name: "Matcha Point", City: "Маастрихт", Tags: []string{"coffee", "vegan", "workspace"}, Rating: 4.8, Price: 3, Lat: 50.849, Lon: 5.691},
}

// ToolRunner manages action execution and permission validation.
type ToolRunner struct{}

func NewToolRunner() *ToolRunner {
	return &ToolRunner{}
}

// Run executes a tool call with permission enforcement.
func (tr *ToolRunner) Run(call llm.ToolCall, agentCtx *llm.AgentContext) ToolResult {
	if _, isServerOnly := ServerOnlyTools[call.Tool]; isServerOnly {
		return ToolResult{
			ID:    call.ID,
			Tool:  call.Tool,
			Ok:    false,
			Error: "server_only: действие исполняет сервер после проверки разрешений",
		}
	}

	switch call.Tool {
	case "check_calendar":
		window := planner.TimeWindow{
			Start: time.Now().UTC(),
			End:   time.Now().UTC().Add(48 * time.Hour),
		}
		slots := planner.FindFreeSlots(window, agentCtx.Calendar, agentCtx.Preferences, 60, 30, 5)
		return ToolResult{
			ID:   call.ID,
			Tool: call.Tool,
			Ok:   true,
			Data: map[string]any{
				"free_slots_count": len(slots),
				"slots":            slots,
			},
		}

	case "suggest_time":
		duration := 60
		if d, ok := call.Args["duration_minutes"].(float64); ok && d > 0 {
			duration = int(d)
		}
		window := planner.TimeWindow{
			Start: time.Now().UTC(),
			End:   time.Now().UTC().Add(48 * time.Hour),
		}
		slots := planner.FindFreeSlots(window, agentCtx.Calendar, agentCtx.Preferences, duration, 30, 3)
		return ToolResult{
			ID:   call.ID,
			Tool: call.Tool,
			Ok:   true,
			Data: map[string]any{
				"suggested_slots": slots,
			},
		}

	case "find_cafe":
		results := searchCafes(agentCtx.Preferences, call.Args)
		return ToolResult{
			ID:   call.ID,
			Tool: call.Tool,
			Ok:   true,
			Data: map[string]any{
				"results": results,
				"count":   len(results),
			},
		}

	case "create_reminder":
		text, _ := call.Args["text"].(string)
		at, _ := call.Args["at"].(string)
		return ToolResult{
			ID:   call.ID,
			Tool: call.Tool,
			Ok:   true,
			Data: map[string]any{
				"created": true,
				"text":    text,
				"at":      at,
			},
		}

	case "create_note":
		text, _ := call.Args["text"].(string)
		return ToolResult{
			ID:   call.ID,
			Tool: call.Tool,
			Ok:   true,
			Data: map[string]any{
				"created": true,
				"text":    text,
			},
		}

	default:
		return ToolResult{
			ID:    call.ID,
			Tool:  call.Tool,
			Ok:    false,
			Error: fmt.Sprintf("unknown tool: %s", call.Tool),
		}
	}
}

func searchCafes(prefs planner.Preferences, args map[string]any) []map[string]any {
	city, _ := args["city"].(string)
	if city == "" {
		city = prefs.City
	}

	var diet []string
	if dList, ok := args["diet"].([]any); ok {
		for _, d := range dList {
			if s, ok := d.(string); ok {
				diet = append(diet, s)
			}
		}
	}
	if len(diet) == 0 {
		diet = prefs.Diet
	}

	var midpoint *struct{ Lat, Lon float64 }
	if mp, ok := args["midpoint"].(map[string]any); ok {
		lat, okLat := mp["lat"].(float64)
		lon, okLon := mp["lon"].(float64)
		if okLat && okLon {
			midpoint = &struct{ Lat, Lon float64 }{Lat: lat, Lon: lon}
		}
	}

	type scoredCafe struct {
		score float64
		cafe  CafeItem
	}

	var scored []scoredCafe
	for _, c := range cafeDatabase {
		if city != "" && !strings.EqualFold(c.City, city) {
			continue
		}
		sc := c.Rating / 5.0
		for _, tag := range c.Tags {
			for _, d := range diet {
				if tag == d {
					sc += 0.2
				}
			}
		}
		if midpoint != nil {
			dist := math.Hypot(c.Lat-midpoint.Lat, c.Lon-midpoint.Lon)
			if 0.3-dist > 0 {
				sc += (0.3 - dist)
			}
		}
		scored = append(scored, scoredCafe{score: sc, cafe: c})
	}

	var res []map[string]any
	for _, s := range scored {
		res = append(res, map[string]any{
			"name":   s.cafe.Name,
			"city":   s.cafe.City,
			"rating": s.cafe.Rating,
			"score":  math.Round(s.score*1000) / 1000,
		})
	}
	if len(res) == 0 && len(cafeDatabase) > 0 {
		// Fallback to first cafe
		res = append(res, map[string]any{
			"name":   cafeDatabase[0].Name,
			"city":   cafeDatabase[0].City,
			"rating": cafeDatabase[0].Rating,
			"score":  0.8,
		})
	}

	return res
}
