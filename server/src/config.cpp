// aura/config.cpp
#include "aura/config.h"

#include <cstdlib>
#include <sstream>

namespace aura {

namespace {
int intOr(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}
}  // namespace

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return value;
}

Config Config::fromEnv() {
    Config config;
    config.host = envOr("AURA_HOST", config.host);
    config.port = intOr("AURA_PORT", config.port);
    config.databaseUrl = envOr("AURA_DATABASE_URL", "");
    config.embeddedDbPath = envOr("AURA_EMBEDDED_DB", config.embeddedDbPath);
    config.aiServiceUrl = envOr("AURA_AI_URL", config.aiServiceUrl);
    config.aiServiceToken = envOr("AURA_AI_TOKEN", "");
    config.aiTimeoutMs = intOr("AURA_AI_TIMEOUT_MS", config.aiTimeoutMs);
    config.jwtSecret = envOr("AURA_JWT_SECRET", config.jwtSecret);
    config.twoFactorKey = envOr("AURA_2FA_KEY", config.twoFactorKey);
    config.tokenTtlSeconds = intOr("AURA_TOKEN_TTL", static_cast<int>(config.tokenTtlSeconds));
    config.refreshTtlSeconds = intOr("AURA_REFRESH_TTL", static_cast<int>(config.refreshTtlSeconds));
    config.verifyTtlSeconds = intOr("AURA_VERIFY_TTL", static_cast<int>(config.verifyTtlSeconds));
    config.resetTtlSeconds = intOr("AURA_RESET_TTL", static_cast<int>(config.resetTtlSeconds));
    config.mailDriver = envOr("AURA_MAIL_DRIVER", config.mailDriver);
    config.toolsMode = envOr("AURA_TOOLS_MODE", config.toolsMode);
    config.emailApiUrl = envOr("AURA_EMAIL_API_URL", "");
    config.mapsApiUrl = envOr("AURA_MAPS_API_URL", "");
    config.maxFrameBytes = static_cast<std::size_t>(intOr("AURA_MAX_FRAME", static_cast<int>(config.maxFrameBytes)));
    config.maxConnections =
        static_cast<std::size_t>(intOr("AURA_MAX_CONNECTIONS", static_cast<int>(config.maxConnections)));
    config.maxLoginAttempts = intOr("AURA_MAX_LOGIN_ATTEMPTS", config.maxLoginAttempts);
    config.loginWindowSec = intOr("AURA_LOGIN_WINDOW", config.loginWindowSec);
    config.pingIntervalSec = intOr("AURA_PING_INTERVAL", config.pingIntervalSec);
    config.socketTimeoutSec = intOr("AURA_SOCKET_TIMEOUT", config.socketTimeoutSec);
    config.schedulerIntervalMs = intOr("AURA_SCHEDULER_INTERVAL_MS", config.schedulerIntervalMs);
    config.logLevel = envOr("AURA_LOG_LEVEL", config.logLevel);
    return config;
}

std::string Config::describe() const {
    std::ostringstream out;
    out << "host=" << host << ':' << port
        << " db=" << (databaseUrl.empty() ? std::string("embedded(") + embeddedDbPath + ")" : "postgresql")
        << " ai=" << aiServiceUrl << " tools=" << toolsMode
        << " token_ttl=" << tokenTtlSeconds << 's'
        << " refresh_ttl=" << refreshTtlSeconds << 's'
        << " mail=" << mailDriver;
    return out.str();
}

}  // namespace aura
