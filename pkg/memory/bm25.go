package memory

import (
	"math"
	"sort"
)

const (
	BM25K1 = 1.5
	BM25B  = 0.75
)

// stemTokens converts text to stemmed tokens.
func stemTokens(text string) []string {
	toks := Tokenize(text)
	res := make([]string, len(toks))
	for i, t := range toks {
		res[i] = Stem(t)
	}
	return res
}

type scoredEntry struct {
	score float64
	entry MemoryEntry
}

// BM25Rank ranks memories using the Okapi BM25 ranking algorithm weighted by entry weight.
func BM25Rank(query string, entries []MemoryEntry, limit int) []MemoryEntry {
	if limit <= 0 {
		limit = 8
	}
	if len(entries) == 0 {
		return nil
	}

	queryTokens := stemTokens(query)
	queryStems := make(map[string]struct{}, len(queryTokens))
	for _, t := range queryTokens {
		queryStems[t] = struct{}{}
	}

	// If no searchable stems, sort by weight and recency
	if len(queryStems) == 0 {
		sorted := make([]MemoryEntry, len(entries))
		copy(sorted, entries)
		sort.Slice(sorted, func(i, j int) bool {
			if sorted[i].Weight == sorted[j].Weight {
				return sorted[i].Text < sorted[j].Text
			}
			return sorted[i].Weight > sorted[j].Weight
		})
		if len(sorted) > limit {
			return sorted[:limit]
		}
		return sorted
	}

	type docItem struct {
		entry MemoryEntry
		stems []string
	}

	docs := make([]docItem, 0, len(entries))
	totalLen := 0
	for _, e := range entries {
		st := stemTokens(e.Text)
		if len(st) > 0 {
			docs = append(docs, docItem{entry: e, stems: st})
			totalLen += len(st)
		}
	}

	if len(docs) == 0 {
		return nil
	}

	total := float64(len(docs))
	avgdl := float64(totalLen) / total

	// Document Frequency for query stems
	df := make(map[string]float64, len(queryStems))
	for _, d := range docs {
		present := make(map[string]struct{})
		for _, s := range d.stems {
			present[s] = struct{}{}
		}
		for qs := range queryStems {
			if _, ok := present[qs]; ok {
				df[qs]++
			}
		}
	}

	// Calculate IDF for each query stem
	idf := make(map[string]float64, len(queryStems))
	for qs := range queryStems {
		n := df[qs]
		idf[qs] = math.Log(1.0 + (total-n+0.5)/(n+0.5))
	}

	scored := make([]scoredEntry, 0, len(docs))
	for _, d := range docs {
		tf := make(map[string]float64)
		for _, s := range d.stems {
			tf[s]++
		}

		score := 0.0
		dLen := float64(len(d.stems))

		for qs := range queryStems {
			freq := tf[qs]
			if freq == 0 {
				continue
			}
			denom := freq + BM25K1*(1.0-BM25B+BM25B*(dLen/avgdl))
			score += idf[qs] * (freq * (BM25K1 + 1.0)) / denom
		}

		if score > 0 {
			// Boost with entry weight
			finalScore := score * (0.5 + d.entry.Weight)
			scored = append(scored, scoredEntry{
				score: math.Round(finalScore*10000) / 10000,
				entry: d.entry,
			})
		}
	}

	sort.Slice(scored, func(i, j int) bool {
		if scored[i].score == scored[j].score {
			return scored[i].entry.Text < scored[j].entry.Text
		}
		return scored[i].score > scored[j].score
	})

	results := make([]MemoryEntry, 0, limit)
	for i := 0; i < len(scored) && i < limit; i++ {
		results = append(results, scored[i].entry)
	}

	return results
}
