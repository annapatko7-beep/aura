package planner

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode"
)

var (
	weekdayAliases = map[string]time.Weekday{
		"понедельник": time.Monday, "в понедельник": time.Monday, "пн": time.Monday,
		"вторник": time.Tuesday, "во вторник": time.Tuesday, "вт": time.Tuesday,
		"среда": time.Wednesday, "в среду": time.Wednesday, "ср": time.Wednesday,
		"четверг": time.Thursday, "в четверг": time.Thursday, "чт": time.Thursday,
		"пятница": time.Friday, "в пятницу": time.Friday, "пт": time.Friday,
		"суббота": time.Saturday, "в субботу": time.Saturday, "сб": time.Saturday,
		"воскресенье": time.Sunday, "в воскресенье": time.Sunday, "вск": time.Sunday, "вс": time.Sunday,
	}

	weekdayNames = [...]string{
		"воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота",
	}

	intentPrefixes = []struct {
		intent string
		re     *regexp.Regexp
	}{
		{"reminder", regexp.MustCompile(`^\s*(напомни|напомните|не забудь|не забудьте)`)},
		{"note", regexp.MustCompile(`^\s*(запиши|запишите|сделай заметку|законспектируй)`)},
		{"find_place", regexp.MustCompile(`^\s*(найди|найдите|подбери|подберите)`)},
		{"email", regexp.MustCompile(`^\s*(отправь письмо|напиши письмо|отправьте письмо)`)},
		{"message", regexp.MustCompile(`^\s*(напиши|отправь|скинь|ответь)`)},
		{"memory", regexp.MustCompile(`^\s*(запомни|запомните)`)},
	}

	intentKeywordsOrder = []string{
		"schedule_meeting", "reminder", "note", "email", "message", "find_place", "budget", "memory",
	}

	intentKeywords = map[string][]string{
		"schedule_meeting": {
			"встрет", "созвон", "встреча", "кофе", "обсудить", "обсудим", "meeting",
			"пересечься", "увидеться", "свидание", "переговоры", "созвониться",
		},
		"reminder":   {"напомни", "напомнить", "напоминание", "не забыть", "reminder", "не забудь"},
		"note":       {"заметк", "запиши", "записать", "конспект", "note"},
		"email":      {"почт", "письмо", "email", "e-mail", "напиши письмо", "отправь письмо"},
		"message":    {"напиши", "отправь", "сообщение", "скинь", "message", "ответь"},
		"find_place": {"найди", "подбери", "где", "кафе", "кофейн", "ресторан", "место", "place"},
		"budget":     {"бюджет", "потрат", "деньги", "стоимость", "budget", "оплати"},
		"memory":     {"запомни", "помни", "запомнить", "я люблю", "я не люблю", "у меня аллергия"},
	}

	dayParts = map[string][2]int{
		"утром":       {8, 12},
		"утро":        {8, 12},
		"днём":        {12, 17},
		"днем":        {12, 17},
		"после обеда": {13, 18},
		"вечером":     {17, 23},
		"вечер":       {17, 23},
		"ночью":       {0, 6},
	}

	timeRe     = regexp.MustCompile(`(?:\b|^|\s)(\d{1,2})[:.](\d{2})(?:\b|$|\s)`)
	hourRe     = regexp.MustCompile(`(?:^|\s)в\s+(\d{1,2})(?:\s*(?:час\w*|ч\b))?(?:\s+(утра|дня|вечера|ночи))?(?:$|[\s,.!?])`)
	inNRe      = regexp.MustCompile(`через\s+(\d+)\s*(минут\w*|час\w*|день|дня|дней|недел\w*)`)
	durationRe = regexp.MustCompile(`(?:^|\s)на\s+(\d+)\s*(минут\w*|час\w*|ч(?:\s|$|[.,!?]))`)

	reminderNoise = []*regexp.Regexp{
		regexp.MustCompile(`(?i)^\s*(?:напомни(?:те)?|не\s+забудь(?:те)?)(?:\s+мне)?(?:\s|$)`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])(?:послезавтра|завтра|сегодня)(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])в\s+\d{1,2}[:.]\d{2}(?:\s+(?:утра|дня|вечера|ночи))?(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])в\s+\d{1,2}(?:\s*(?:час\w*|ч\b))?(?:\s+(?:утра|дня|вечера|ночи))?(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])\d{1,2}[:.]\d{2}(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])через\s+\d+\s*(?:минут\w*|час\w*|день|дня|дней|недел\w*)(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])(?:утром|днём|днем|вечером|ночью)(?:$|[\s,.!?])`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])(?:в|во)\s+(?:понедельник|вторник|среду|четверг|пятницу|субботу|воскресенье)(?:$|[\s,.!?])`),
	}

	personRe = regexp.MustCompile(`(?:^|\s)с\s+([А-ЯЁA-Z][а-яёa-zA-Z]{1,30})(?:$|[\s,.!?])`)
	friendRe = regexp.MustCompile(`(?:^|\s)с\s+(друг(?:ом)?|подруг(?:ой)?)(?:$|[\s,.!?])`)
	topicRe  = regexp.MustCompile(`(?i)(?:обсудить|про|о|по поводу)\s+([а-яёa-zA-Z0-9 ,\-]{3,60})`)
	wordRe   = regexp.MustCompile(`[а-яёa-z]{4,}`)
)

// DetectIntent determines user intent from keywords and phrase prefixes.
func DetectIntent(text string) string {
	low := strings.ToLower(text)
	trimmed := strings.TrimSpace(low)
	if trimmed == "" {
		return "chat"
	}

	for _, p := range intentPrefixes {
		if p.re.MatchString(low) {
			return p.intent
		}
	}

	bestIntent := "chat"
	maxHits := 0

	for _, intent := range intentKeywordsOrder {
		words := intentKeywords[intent]
		hits := 0
		for _, w := range words {
			if strings.Contains(low, w) {
				hits++
			}
		}
		if hits > maxHits {
			maxHits = hits
			bestIntent = intent
		}
	}

	return bestIntent
}

// ParseDuration extracts the meeting or task duration in minutes.
func ParseDuration(text string, defaultMin int) int {
	if defaultMin <= 0 {
		defaultMin = 60
	}
	low := strings.ToLower(text)
	m := durationRe.FindStringSubmatch(low)
	if len(m) >= 3 {
		val, err := strconv.Atoi(m[1])
		if err == nil && val > 0 {
			unit := m[2]
			minutes := val
			if strings.HasPrefix(unit, "час") || unit == "ч" {
				minutes = val * 60
			}
			if minutes > MaxDurationMinutes {
				minutes = MaxDurationMinutes
			}
			if minutes < 1 {
				minutes = 1
			}
			return minutes
		}
	}

	if strings.Contains(low, "полчаса") {
		return 30
	}
	if strings.Contains(low, "на час") {
		return 60
	}
	return defaultMin
}

// ExtractPerson extracts meeting counterparty name ("встретиться с Аней" -> "Аней").
func ExtractPerson(text string) string {
	m := personRe.FindStringSubmatch(text)
	if len(m) >= 2 {
		return m[1]
	}
	low := strings.ToLower(text)
	if fMatch := friendRe.FindString(low); fMatch != "" {
		return strings.TrimPrefix(fMatch, "с ")
	}
	return ""
}

// ExtractTopic extracts the discussion topic.
func ExtractTopic(text string) string {
	m := topicRe.FindStringSubmatch(text)
	if len(m) >= 2 {
		res := strings.Trim(m[1], " .,")
		return res
	}
	return ""
}

// ReminderText strips command wrappers from a reminder phrase.
func ReminderText(text string) string {
	val := text
	for _, re := range reminderNoise {
		val = re.ReplaceAllString(val, " ")
	}
	cleanPrefix := regexp.MustCompile(`(?i)^[\s,.:;\-—]*(?:(?:о\s+том,?\s+)?чтобы?|что)\b`)
	val = cleanPrefix.ReplaceAllString(val, " ")
	val = strings.Join(strings.Fields(val), " ")
	val = strings.Trim(val, " ,.:;-—")
	if val == "" {
		return strings.TrimSpace(text)
	}

	runes := []rune(val)
	runes[0] = unicode.ToUpper(runes[0])
	return string(runes)
}

func nextWeekdayDate(now time.Time, target time.Weekday) time.Time {
	currentWd := now.Weekday()
	days := (int(target) - int(currentWd) + 7) % 7
	if days == 0 {
		days = 7
	}
	targetDate := now.AddDate(0, 0, days)
	return time.Date(targetDate.Year(), targetDate.Month(), targetDate.Day(), 0, 0, 0, 0, now.Location())
}

func parseClockTime(low string, hasDay bool) (int, int, bool) {
	if m := timeRe.FindStringSubmatch(low); len(m) >= 3 {
		h, _ := strconv.Atoi(m[1])
		min, _ := strconv.Atoi(m[2])
		if h > 23 {
			h = 23
		}
		if min > 59 {
			min = 59
		}
		return h, min, true
	}

	if m := hourRe.FindStringSubmatch(low); len(m) >= 2 {
		h, _ := strconv.Atoi(m[1])
		part := ""
		if len(m) >= 3 {
			part = m[2]
		}
		explicit := part != "" || regexp.MustCompile(`^\s*в\s+\d{1,2}\s*(?:час|ч\b)`).MatchString(m[0])
		if h > 23 || (!explicit && !hasDay) {
			return 0, 0, false
		}
		if (part == "дня" || part == "вечера") && h < 12 {
			h += 12
		} else if part == "ночи" && h == 12 {
			h = 0
		}
		return h, 0, true
	}

	return 0, 0, false
}

// ParseTimeWindow parses natural Russian time expressions into a start/end window.
func ParseTimeWindow(text string, now time.Time) *TimeWindow {
	if now.IsZero() {
		now = time.Now().UTC()
	}
	low := strings.ToLower(text)
	if strings.TrimSpace(low) == "" {
		return nil
	}

	loc := now.Location()
	y, m, d := now.Date()
	baseDay := time.Date(y, m, d, 0, 0, 0, 0, loc)
	dayStart := baseDay
	label := ""
	source := "phrase"

	if strings.Contains(low, "послезавтра") {
		dayStart = baseDay.AddDate(0, 0, 2)
		label = "послезавтра"
	} else if strings.Contains(low, "завтра") {
		dayStart = baseDay.AddDate(0, 0, 1)
		label = "завтра"
	} else if strings.Contains(low, "выходн") {
		// Nearest Saturday
		offset := (int(time.Saturday) - int(now.Weekday()) + 7) % 7
		dayStart = baseDay.AddDate(0, 0, offset)
		label = "выходные"
	} else if strings.Contains(low, "на следующей неделе") || strings.Contains(low, "следующая неделя") {
		daysToNextMonday := (int(time.Monday) - int(now.Weekday()) + 7) % 7
		if daysToNextMonday == 0 {
			daysToNextMonday = 7
		}
		dayStart = baseDay.AddDate(0, 0, daysToNextMonday)
		label = "следующая неделя"
	} else if strings.Contains(low, "на этой неделе") || strings.Contains(low, "эта неделя") {
		label = "эта неделя"
	} else if strings.Contains(low, "сегодня") {
		label = "сегодня"
	} else {
		for alias, wd := range weekdayAliases {
			re := regexp.MustCompile(fmt.Sprintf(`(^|[^а-яё])%s([^а-яё]|$)`, regexp.QuoteMeta(alias)))
			if re.MatchString(low) {
				dayStart = nextWeekdayDate(now, wd)
				label = weekdayNames[wd]
				break
			}
		}
	}

	daysSpan := 1
	if label == "выходные" {
		daysSpan = 2
	} else if strings.Contains(label, "недел") {
		daysSpan = 7
	}
	dayEnd := dayStart.AddDate(0, 0, daysSpan)

	start := dayStart
	end := dayEnd

	h, min, foundClock := parseClockTime(low, label != "")
	if foundClock && label != "" {
		start = time.Date(dayStart.Year(), dayStart.Month(), dayStart.Day(), h, min, 0, 0, loc)
		end = start.Add(3 * time.Hour)
	} else if foundClock && label == "" {
		candidate := time.Date(now.Year(), now.Month(), now.Day(), h, min, 0, 0, loc)
		if !candidate.After(now) {
			candidate = candidate.AddDate(0, 0, 1)
		}
		start = candidate
		end = candidate.Add(3 * time.Hour)
		if label == "" {
			label = "ближайшее время"
		}
	} else {
		for part, hours := range dayParts {
			if strings.Contains(low, part) {
				start = time.Date(dayStart.Year(), dayStart.Month(), dayStart.Day(), hours[0], 0, 0, 0, loc)
				end = time.Date(dayStart.Year(), dayStart.Month(), dayStart.Day(), hours[1], 0, 0, 0, loc)
				if label == "" {
					label = part
				}
				break
			}
		}
	}

	// Relative "через N ..."
	if inMatch := inNRe.FindStringSubmatch(low); len(inMatch) >= 3 {
		num, err := strconv.Atoi(inMatch[1])
		if err == nil && num > 0 && num <= 3650 {
			unit := inMatch[2]
			var offset time.Duration
			if strings.HasPrefix(unit, "минут") {
				offset = time.Duration(num) * time.Minute
			} else if strings.HasPrefix(unit, "час") {
				offset = time.Duration(num) * time.Hour
			} else if strings.HasPrefix(unit, "недел") {
				offset = time.Duration(num) * 7 * 24 * time.Hour
			} else {
				offset = time.Duration(num) * 24 * time.Hour
			}
			start = now.Add(offset)
			end = start.Add(4 * time.Hour)
			if label == "" {
				label = fmt.Sprintf("через %d %s", num, unit)
			}
			source = "relative"
		}
	}

	if !end.After(start) {
		end = start.Add(3 * time.Hour)
	}
	if start.Before(now) {
		start = now
		if !end.After(start) {
			end = start.Add(3 * time.Hour)
		}
	}

	if label == "" {
		label = "ближайшее время"
	}

	return &TimeWindow{
		Start:  start.UTC(),
		End:    end.UTC(),
		Label:  label,
		Source: source,
	}
}

// Understand performs end-to-end natural phrase parsing.
func Understand(text string, now time.Time) Understanding {
	if now.IsZero() {
		now = time.Now().UTC()
	}
	low := strings.ToLower(text)
	rawMatches := wordRe.FindAllString(low, -1)
	keywords := make([]string, 0, len(rawMatches))
	for _, w := range rawMatches {
		keywords = append(keywords, w)
	}

	return Understanding{
		Text:            text,
		Intent:          DetectIntent(text),
		Window:          ParseTimeWindow(text, now),
		DurationMinutes: ParseDuration(text, 60),
		Person:          ExtractPerson(text),
		Topic:           ExtractTopic(text),
		Keywords:        keywords,
	}
}
