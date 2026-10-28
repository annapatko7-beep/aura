package main

import (
	"runtime"
	"runtime/debug"
	"testing"
	"time"

	"github.com/selfvoy/ai-core/internal/agent"
	"github.com/selfvoy/ai-core/internal/api"
	"github.com/selfvoy/ai-core/internal/config"
	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/memory"
)

func TestIdleRAMUsage(t *testing.T) {
	cfg := config.LoadConfig()
	store := memory.NewInMemoryStore()
	llmClient := llm.NewMockLLM()
	agentInstance := agent.NewSelfvoyAgent(llmClient, store)
	_ = api.NewServer(cfg, agentInstance, store, nil)

	// Trigger GC and return memory to OS
	runtime.GC()
	debug.FreeOSMemory()
	time.Sleep(50 * time.Millisecond)

	var m runtime.MemStats
	runtime.ReadMemStats(&m)

	heapAllocMB := float64(m.HeapAlloc) / (1024 * 1024)
	heapInUseMB := float64(m.HeapInuse) / (1024 * 1024)
	stackInUseMB := float64(m.StackInuse) / (1024 * 1024)
	sysMB := float64(m.Sys) / (1024 * 1024)

	t.Logf("Go Runtime Idle Stats:")
	t.Logf("  HeapAlloc:    %.2f MB", heapAllocMB)
	t.Logf("  HeapInUse:    %.2f MB", heapInUseMB)
	t.Logf("  StackInUse:   %.2f MB", stackInUseMB)
	t.Logf("  Sys Reserved: %.2f MB", sysMB)

	if heapAllocMB > 25.0 {
		t.Errorf("HeapAlloc %.2f MB exceeds 25 MB limit", heapAllocMB)
	}
}
