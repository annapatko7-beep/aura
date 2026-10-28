package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/selfvoy/ai-core/internal/agent"
	"github.com/selfvoy/ai-core/internal/api"
	"github.com/selfvoy/ai-core/internal/config"
	"github.com/selfvoy/ai-core/internal/llm"
	"github.com/selfvoy/ai-core/pkg/federation"
	"github.com/selfvoy/ai-core/pkg/memory"
	"github.com/selfvoy/ai-core/pkg/speech"
)

func main() {
	cfg := config.LoadConfig()

	// Initialize federation node identity
	_, pubKeyHex := federation.GetServerKeypair()

	// Initialize memory store (Postgres, File, or Memory)
	store := memory.BuildStore(context.Background(), cfg.MemoryBackend, cfg.DatabaseURL, cfg.MemoryFilePath)

	// Initialize LLM client
	llmClient := llm.BuildLLM(cfg)

	// Initialize STT provider
	sttProvider := speech.BuildSTT(cfg.STTProvider, cfg.LLMAPIKey, cfg.STTEndpoint, cfg.STTModel)

	// Initialize Agent Core
	agentInstance := agent.NewSelfvoyAgent(llmClient, store)

	// Initialize HTTP API Server
	apiServer := api.NewServer(cfg, agentInstance, store, sttProvider)

	addr := fmt.Sprintf("%s:%d", cfg.Host, cfg.Port)
	httpServer := &http.Server{
		Addr:              addr,
		Handler:           apiServer.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
	}

	log.Printf("[INFO] ========================================================")
	log.Printf("[INFO] Selfvoy AI Core (Go 1.23+) starting on %s", addr)
	log.Printf("[INFO] LLM Provider   : %s", llmClient.Name())
	log.Printf("[INFO] STT Provider   : %s", sttProvider.Name())
	log.Printf("[INFO] Memory Backend : %s", cfg.MemoryBackend)
	log.Printf("[INFO] Node Public Key: %s...", pubKeyHex[:16])
	log.Printf("[INFO] ========================================================")

	// Graceful shutdown handling
	stopChan := make(chan os.Signal, 1)
	signal.Notify(stopChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("[FATAL] HTTP server failed: %v", err)
		}
	}()

	sig := <-stopChan
	log.Printf("[INFO] Received signal %v, initiating graceful shutdown...", sig)

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := httpServer.Shutdown(shutdownCtx); err != nil {
		log.Printf("[ERROR] HTTP server shutdown error: %v", err)
	}

	log.Println("[INFO] Selfvoy AI Core stopped cleanly.")
}
