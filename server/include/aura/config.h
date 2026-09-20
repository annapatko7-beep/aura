// aura/config.h — конфигурация сервера из переменных окружения.
#pragma once

#include <string>

namespace aura {

struct Config {
    std::string host = "0.0.0.0";              // AURA_HOST
    int port = 9000;                           // AURA_PORT
    std::string databaseUrl;                   // AURA_DATABASE_URL (postgres://...)
    std::string embeddedDbPath = ".aura-data.json";  // AURA_EMBEDDED_DB (режим без PostgreSQL)

    std::string aiServiceUrl = "http://127.0.0.1:8000";  // AURA_AI_URL
    std::string aiServiceToken;                          // AURA_AI_TOKEN
    int aiTimeoutMs = 15000;                             // AURA_AI_TIMEOUT_MS

    std::string jwtSecret = "aura-dev-secret";           // AURA_JWT_SECRET
    std::string twoFactorKey;                            // AURA_2FA_KEY (ключ AES-256-GCM для TOTP-секретов)
    long long tokenTtlSeconds = 60 * 60 * 24 * 7;        // AURA_TOKEN_TTL
    long long refreshTtlSeconds = 60 * 60 * 24 * 30;     // AURA_REFRESH_TTL
    long long verifyTtlSeconds = 60 * 60 * 24;           // AURA_VERIFY_TTL (код подтверждения email)
    long long resetTtlSeconds = 60 * 60;                 // AURA_RESET_TTL (код сброса пароля)
    std::string mailDriver = "dev";                      // AURA_MAIL_DRIVER: dev|log
    std::string toolsMode = "sandbox";                   // AURA_TOOLS_MODE: sandbox|http
    std::string emailApiUrl;                             // AURA_EMAIL_API_URL
    std::string mapsApiUrl;                              // AURA_MAPS_API_URL

    std::size_t maxFrameBytes = 1024 * 1024;             // AURA_MAX_FRAME
    std::size_t maxConnections = 1024;                   // AURA_MAX_CONNECTIONS
    int maxLoginAttempts = 8;                            // AURA_MAX_LOGIN_ATTEMPTS
    int loginWindowSec = 300;                            // AURA_LOGIN_WINDOW
    int pingIntervalSec = 25;                            // AURA_PING_INTERVAL
    int socketTimeoutSec = 120;                          // AURA_SOCKET_TIMEOUT
    int schedulerIntervalMs = 30000;                     // AURA_SCHEDULER_INTERVAL_MS (0 = выключен)

    // Интеграции (этап 9): Google OAuth 2.0 + PKCE. URL по умолчанию —
    // реальные эндпоинты Google (прод идёт за TLS-прокси); e2e подменяет
    // их локальным mock-сервером.
    std::string googleClientId;                          // AURA_GOOGLE_CLIENT_ID
    std::string googleClientSecret;                      // AURA_GOOGLE_CLIENT_SECRET
    std::string googleAuthUrl = "https://accounts.google.com/o/oauth2/v2/auth";  // AURA_GOOGLE_AUTH_URL
    std::string googleTokenUrl = "https://oauth2.googleapis.com/token";          // AURA_GOOGLE_TOKEN_URL
    std::string googleRevokeUrl = "https://oauth2.googleapis.com/revoke";        // AURA_GOOGLE_REVOKE_URL
    std::string googleCalendarUrl = "https://www.googleapis.com/calendar/v3";    // AURA_GOOGLE_CALENDAR_URL
    std::string googleGmailUrl = "https://gmail.googleapis.com/gmail/v1";        // AURA_GOOGLE_GMAIL_URL
    int oauthStateTtlSec = 600;                          // AURA_OAUTH_STATE_TTL — жизнь одноразового state
    std::string logLevel = "info";                       // AURA_LOG_LEVEL

    static Config fromEnv();
    std::string describe() const;  // для стартового лога (без секретов)
};

std::string envOr(const char* name, const std::string& fallback);

}  // namespace aura
