package memory

import (
	"math"
	"regexp"
	"sort"
	"strings"
	"time"
)

var (
	factPatterns = []*regexp.Regexp{
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])я\s+(люблю|не люблю|предпочитаю|ем|не ем|живу|работаю)\s+([^.;!?]{2,60})`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])у меня\s+(аллергия на|есть|нет)\s+([^.;!?]{2,60})`),
		regexp.MustCompile(`(?i)(?:^|[\s,.!?])мо(?:й|я|ё)\s+(друг|подруга|коллега|начальник)\s+([А-ЯЁA-Z][а-яёa-zA-Z]{1,30})`),
	}

	stopwords = map[string]struct{}{
		"что": {}, "как": {}, "это": {}, "для": {}, "мне": {}, "тебе": {}, "хочу": {}, "надо": {}, "быть": {}, "чтобы": {},
		"сегодня": {}, "завтра": {}, "встрет": {}, "можно": {}, "просто": {}, "очень": {}, "котор": {}, "когда": {},
	}

	tokenRe = regexp.MustCompile(`[а-яёa-z0-9]{3,}`)
)

// Stem truncates a word to its 4-rune base for rough linguistic matching.
func Stem(word string) string {
	runes := []rune(word)
	if len(runes) >= 4 {
		return string(runes[:4])
	}
	return word
}

// Tokenize splits text into lowercased tokens, ignoring standard stopwords.
func Tokenize(text string) []string {
	low := strings.ToLower(text)
	matches := tokenRe.FindAllString(low, -1)
	res := make([]string, 0, len(matches))
	for _, m := range matches {
		if _, isStop := stopwords[m]; !isStop {
			res = append(res, m)
		}
	}
	return res
}

// ExtractFacts extracts persistent personal facts/preferences from natural user input.
func ExtractFacts(text string) []MemoryEntry {
	var facts []MemoryEntry
	now := time.Now().UTC()

	for _, pattern := range factPatterns {
		submatches := pattern.FindAllStringSubmatch(text, -1)
		for _, m := range submatches {
			if len(m) >= 3 {
				action := strings.TrimSpace(m[1])
				detail := strings.TrimSpace(m[2])
				phrase := action + " " + detail

				kind := KindFact
				lowPhrase := strings.ToLower(phrase)
				if strings.Contains(lowPhrase, "люблю") || strings.Contains(lowPhrase, "предпоч") {
					kind = KindPreference
				}

				facts = append(facts, MemoryEntry{
					Kind:      kind,
					Text:      phrase,
					Weight:    1.0,
					Tags:      []string{"extracted"},
					UpdatedAt: now,
				})
			}
		}
	}

	return facts
}

// MergeEntries combines existing and newly arrived memories, deduplicating and boosting weight.
func MergeEntries(existing, additions []MemoryEntry) []MemoryEntry {
	byText := make(map[string]MemoryEntry)

	combine := func(entry MemoryEntry) {
		key := strings.ToLower(strings.TrimSpace(entry.Text))
		if key == "" {
			return
		}

		if prev, ok := byText[key]; ok {
			newWeight := math.Min(5.0, math.Round((prev.Weight+0.25)*1000)/1000)

			// Union of tags
			tagSet := make(map[string]struct{})
			for _, t := range prev.Tags {
				tagSet[t] = struct{}{}
			}
			for _, t := range entry.Tags {
				tagSet[t] = struct{}{}
			}
			tags := make([]string, 0, len(tagSet))
			for t := range tagSet {
				tags = append(tags, t)
			}
			sort.Strings(tags)

			latestTime := prev.UpdatedAt
			if entry.UpdatedAt.After(latestTime) {
				latestTime = entry.UpdatedAt
			}

			prev.Weight = newWeight
			prev.Tags = tags
			prev.UpdatedAt = latestTime
			byText[key] = prev
		} else {
			if entry.Weight <= 0 {
				entry.Weight = 1.0
			}
			if entry.UpdatedAt.IsZero() {
				entry.UpdatedAt = time.Now().UTC()
			}
			byText[key] = entry
		}
	}

	for _, e := range existing {
		combine(e)
	}
	for _, a := range additions {
		combine(a)
	}

	merged := make([]MemoryEntry, 0, len(byText))
	for _, e := range byText {
		merged = append(merged, e)
	}

	sort.Slice(merged, func(i, j int) bool {
		if merged[i].Weight == merged[j].Weight {
			return merged[i].UpdatedAt.After(merged[j].UpdatedAt)
		}
		return merged[i].Weight > merged[j].Weight
	})

	return merged
}
