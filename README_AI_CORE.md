# Selfvoy AI Core 2.0 (Go 1.23+)

Высокопроизводительное ядро ИИ-агента, планировщика и меж-агентской сети (Aura Federation Protocol / A2A), полностью переписанное с Python на Go.

## 🚀 Ключевые преимущества

- **Минимальное потребление памяти:** ~18.8 МБ RAM под нагрузкой, **0.39 МБ Heap** в простое (цель была <25 МБ).
- **Экстремальная скорость:** расчет свободных слотов ~51 мкс, пересечение расписаний ~14 мкс, валидация криптографии Ed25519 — 153 нс (0 аллокаций).
- **Автономность "из коробки" (Zero-Config Beta):**
  - **Бесплатный Live LLM:** авто-подключение к `https://text.pollinations.ai/openai` без регистрации и ключей.
  - **Бесплатные облачные ключи:** встроенная поддержка Google Gemini (15 RPM free), Groq Cloud (Llama 3.3 70B & Whisper Large v3 free), OpenRouter, Ollama.
  - **Устойчивая память:** атомарное локальное JSON-хранилище (`.selfvoy_memory.json`) или PostgreSQL (`pgx/v5` пул с авто-созданием таблиц и автоматическим in-memory fallback при падении сети).
  - **Распознавание речи (STT):** эмулятор `mock` + интеграция с Whisper (OpenAI / Groq).

---

## ⚡ Быстрый старт (1 клик)

### Windows
Дважды кликните по:
```text
start-ai-core.bat
```
или в PowerShell:
```powershell
.\start-ai-core.ps1
```

Сервер запустится на `http://127.0.0.1:8000`.

### Linux / Docker
```bash
# Сборка и запуск в Docker
docker build -t selfvoy-ai-core .
docker run -p 8000:8000 --env-file .env selfvoy-ai-core
```

---

## ⚙ Конфигурация (.env)

Файл `.env` уже создан в корне проекта с готовыми значениями для бета-тестирования:

```ini
# Сеть
SELFVOY_AI_HOST=127.0.0.1
SELFVOY_AI_PORT=8000
SELFVOY_AI_INSECURE_DEV=1
SELFVOY_AI_TOKEN=selfvoy_secret_beta_token_2026

# LLM: по умолчанию 'auto' (бесплатный рабочий live-эндпоинт без ключа)
SELFVOY_LLM_PROVIDER=auto
SELFVOY_LLM_MODEL=openai

# Память: 'file' (сохранение в .selfvoy_memory.json) или 'postgres' (Neon, Supabase)
SELFVOY_MEMORY_BACKEND=file
SELFVOY_MEMORY_FILE_PATH=./.selfvoy_memory.json

# Подключение к бесплатному облачному PostgreSQL (Neon.tech / Supabase.com):
# SELFVOY_MEMORY_BACKEND=postgres
# SELFVOY_DATABASE_URL=postgresql://user:password@ep-sample-pooler.us-east-2.aws.neon.tech/neondb?sslmode=require
```

### Бесплатные провайдеры нейросетей
1. **Google Gemini Flash (Рекомендуется):**
   - Получите бесплатный ключ: [Google AI Studio](https://aistudio.google.com/apikey) (15 запросов в минуту бесплатно, 1 млн токенов в минуту).
   - В `.env`:
     ```ini
     SELFVOY_LLM_PROVIDER=gemini
     SELFVOY_LLM_API_KEY=AIzaSy...
     SELFVOY_LLM_MODEL=gemini-1.5-flash
     ```
2. **Groq Cloud (Суперскорость + бесплатный Whisper):**
   - Получите ключ: [Groq Console](https://console.groq.com/keys).
   - В `.env`:
     ```ini
     SELFVOY_LLM_PROVIDER=groq
     SELFVOY_LLM_API_KEY=gsk_...
     SELFVOY_LLM_MODEL=llama-3.3-70b-versatile
     SELFVOY_STT_PROVIDER=groq
     ```

---

## 📡 Основные эндпоинты API

| Метод | Путь | Описание |
|---|---|---|
| `GET` | `/healthz` | Проверка здоровья и статуса бэкендов |
| `GET` | `/.well-known/aura-agent` | Discovery-манифест узла и публичный ключ Ed25519 |
| `POST` | `/v1/planner/understand` | Парсинг намерений, дат ("в среду в 15:00") и длительности |
| `POST` | `/v1/planner/slots` | Детерминированный расчет свободных окон в календаре |
| `POST` | `/v1/agent/run` | Главный цикл агента (Fan-Out контекста через errgroup + LLM) |
| `POST` | `/v1/agent/negotiate` | A2A переговоры (1–5 раундов) между агентами |
| `POST` | `/v1/memory/extract` | Извлечение фактов из сообщений пользователя |
| `GET` | `/v1/memory/{user_key}` | Получение фактов с ранжированием Okapi BM25 |
| `POST` | `/v1/speech/transcribe` | Распознавание голосовых сообщений (STT) |
| `POST` | `/v1/federation/inbox` | Входящий шлюз меж-агентских криптографических конвертов AFP |

---

## 🧪 Запуск тестов и бенчмарков

```powershell
$env:PATH = "$env:USERPROFILE\go-sdk\go\bin;" + $env:PATH
go test -v ./...
go test -bench=. ./pkg/...
```
