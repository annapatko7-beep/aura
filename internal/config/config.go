package config

import (
	"os"
	"strconv"
	"strings"
)

// Config holds all service configuration loaded from environment variables.
type Config struct {
	Host                 string
	Port                 int
	ServiceToken         string
	InsecureDev          bool
	DatabaseURL          string
	MemoryBackend        string
	MemoryFilePath       string
	LLMProvider          string
	LLMModel             string
	LLMAPIKey            string
	LLMBaseURL           string
	LLMTimeoutSeconds    int
	LLMTemperature       float64
	FederationPrivateKey string
	STTProvider          string
	STTEndpoint          string
	STTModel             string
	CORSOrigins          []string
}

// LoadConfig reads configuration from environment variables with sensible defaults.
// It also automatically loads .env if present.
func LoadConfig() *Config {
	loadDotEnv()

	cfg := &Config{
		Host:              getEnv("SELFVOY_AI_HOST", "127.0.0.1"),
		Port:              getEnvInt("SELFVOY_AI_PORT", 8000),
		ServiceToken:      getEnv("SELFVOY_AI_TOKEN", ""),
		InsecureDev:       getEnvBool("SELFVOY_AI_INSECURE_DEV", false),
		DatabaseURL:       getEnv("SELFVOY_DATABASE_URL", ""),
		MemoryBackend:     getEnv("SELFVOY_MEMORY_BACKEND", "memory"),
		MemoryFilePath:    getEnv("SELFVOY_MEMORY_FILE_PATH", "./.selfvoy_memory.json"),
		LLMProvider:       strings.ToLower(getEnv("SELFVOY_LLM_PROVIDER", "auto")),
		LLMModel:          getEnv("SELFVOY_LLM_MODEL", ""),
		LLMAPIKey:         firstNonEmpty(os.Getenv("SELFVOY_LLM_API_KEY"), os.Getenv("OPENAI_API_KEY")),
		LLMBaseURL:        firstNonEmpty(os.Getenv("SELFVOY_LLM_BASE_URL"), os.Getenv("OPENAI_BASE_URL")),
		LLMTimeoutSeconds: getEnvInt("SELFVOY_LLM_TIMEOUT", 30),
		LLMTemperature:    getEnvFloat("SELFVOY_LLM_TEMPERATURE", 0.3),
		FederationPrivateKey: os.Getenv("AURA_FEDERATION_PRIVATE_KEY"),
		STTProvider:       strings.ToLower(getEnv("SELFVOY_STT_PROVIDER", "auto")),
		STTEndpoint:       getEnv("SELFVOY_STT_ENDPOINT", ""),
		STTModel:          getEnv("SELFVOY_STT_MODEL", ""),
	}

	cors := getEnv("SELFVOY_CORS_ORIGINS", "*")
	if cors == "*" {
		cfg.CORSOrigins = []string{"*"}
	} else {
		for _, part := range strings.Split(cors, ",") {
			trimmed := strings.TrimSpace(part)
			if trimmed != "" {
				cfg.CORSOrigins = append(cfg.CORSOrigins, trimmed)
			}
		}
	}

	if cfg.LLMBaseURL == "" {
		if cfg.LLMProvider == "openai" || (cfg.LLMProvider == "auto" && cfg.LLMAPIKey != "") {
			cfg.LLMBaseURL = "https://api.openai.com/v1"
		} else if cfg.LLMProvider == "auto" && cfg.LLMAPIKey == "" {
			cfg.LLMBaseURL = "https://text.pollinations.ai/openai"
		}
	}

	return cfg
}

func getEnv(key, defVal string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return defVal
}

func getEnvInt(key string, defVal int) int {
	if val := os.Getenv(key); val != "" {
		if i, err := strconv.Atoi(val); err == nil {
			return i
		}
	}
	return defVal
}

func getEnvBool(key string, defVal bool) bool {
	if val := os.Getenv(key); val != "" {
		return val == "1" || strings.ToLower(val) == "true"
	}
	return defVal
}

func getEnvFloat(key string, defVal float64) float64 {
	if val := os.Getenv(key); val != "" {
		if f, err := strconv.ParseFloat(val, 64); err == nil {
			return f
		}
	}
	return defVal
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if v != "" {
			return v
		}
	}
	return ""
}

func loadDotEnv(filenames ...string) {
	if len(filenames) == 0 {
		filenames = []string{".env", "../.env"}
	}
	for _, fn := range filenames {
		data, err := os.ReadFile(fn)
		if err != nil {
			continue
		}
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			line = strings.TrimSpace(line)
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			parts := strings.SplitN(line, "=", 2)
			if len(parts) == 2 {
				key := strings.TrimSpace(parts[0])
				val := strings.TrimSpace(parts[1])
				if len(val) >= 2 && ((val[0] == '"' && val[len(val)-1] == '"') || (val[0] == '\'' && val[len(val)-1] == '\'')) {
					val = val[1 : len(val)-1]
				}
				if os.Getenv(key) == "" {
					os.Setenv(key, val)
				}
			}
		}
	}
}
