// aura/server/integrationmanager.cpp — Google OAuth 2.0 + PKCE (этап 9).
#include "integrationmanager.h"

#include <chrono>
#include <ctime>

#include "aura/crypto.h"
#include "aura/log.h"
#include "aura/net.h"

namespace aura {
namespace {

constexpr const char* kTokenAad = "aura-integration-token";

std::string percentEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

std::string isoPlusSeconds(int seconds) {
    const std::time_t at = std::time(nullptr) + seconds;
    std::tm tm{};
    gmtime_r(&at, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

long long nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string formBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        if (!body.empty()) body.push_back('&');
        body += percentEncode(key) + "=" + percentEncode(value);
    }
    return body;
}

}  // namespace

IntegrationManager::IntegrationManager(DatabaseManager& database, const Config& config)
    : database_(database), config_(config) {}

Json IntegrationManager::providerCatalog() {
    Json providers = Json::array();
    Json calendar = Json::object();
    calendar.set("provider", Json("google_calendar"));
    calendar.set("name", Json("Google Календарь"));
    calendar.set("description", Json("Чтение событий расписания (calendar.events.readonly)"));
    calendar.set("scope", Json(scopeForProvider("google_calendar")));
    providers.push(calendar);
    Json gmail = Json::object();
    gmail.set("provider", Json("google_gmail"));
    gmail.set("name", Json("Gmail"));
    gmail.set("description", Json("Отправка писем от имени пользователя (gmail.send)"));
    gmail.set("scope", Json(scopeForProvider("google_gmail")));
    providers.push(gmail);
    return providers;
}

bool IntegrationManager::isKnownProvider(const std::string& provider) {
    return provider == "google_calendar" || provider == "google_gmail";
}

std::string IntegrationManager::scopeForProvider(const std::string& provider) {
    if (provider == "google_calendar") return "https://www.googleapis.com/auth/calendar.events.readonly";
    if (provider == "google_gmail") return "https://www.googleapis.com/auth/gmail.send";
    return {};
}

// ------------------------------------------------------------- begin (шаг 1)

IntegrationManager::Result IntegrationManager::begin(long long userId,
                                                     const std::string& provider,
                                                     const std::string& redirectUri) {
    if (!isKnownProvider(provider)) {
        return Result::failure("bad_request", "неизвестный провайдер: " + provider);
    }
    if (config_.googleClientId.empty() || config_.googleClientSecret.empty()) {
        return Result::failure("not_configured",
                               "Google OAuth не настроен на сервере (AURA_GOOGLE_CLIENT_ID/SECRET)");
    }
    if (redirectUri.empty()) {
        return Result::failure("bad_request", "redirect_uri обязателен");
    }

    // PKCE (RFC 7636): verifier — случайность, challenge = BASE64URL(SHA256(verifier)).
    const std::string verifier = crypto::base64UrlEncode(crypto::randomBytes(32));
    const std::string challenge = crypto::base64UrlEncode(crypto::sha256(verifier));
    const std::string state = crypto::randomHex(16);

    OauthStateRecord record;
    record.state = state;
    record.userId = userId;
    record.provider = provider;
    record.verifier = verifier;
    record.redirectUri = redirectUri;
    record.expiresAt = isoPlusSeconds(config_.oauthStateTtlSec);
    const auto created = database_.db().createOauthState(record);
    if (!created.ok) return Result::failure("internal", created.message);

    const std::string scope = scopeForProvider(provider);
    std::string url = config_.googleAuthUrl;
    url += "?response_type=code";
    url += "&client_id=" + percentEncode(config_.googleClientId);
    url += "&redirect_uri=" + percentEncode(redirectUri);
    url += "&scope=" + percentEncode(scope);
    url += "&state=" + state;
    url += "&code_challenge=" + challenge;
    url += "&code_challenge_method=S256";
    url += "&access_type=offline&prompt=consent";

    Result result;
    result.ok = true;
    result.payload.set("provider", Json(provider));
    result.payload.set("authorize_url", Json(url));
    result.payload.set("state", Json(state));
    result.payload.set("expires_in", Json(config_.oauthStateTtlSec));
    return result;
}

// ---------------------------------------------------------- callback (шаг 2)

IntegrationManager::Result IntegrationManager::callback(long long userId,
                                                        const std::string& provider,
                                                        const std::string& code,
                                                        const std::string& state) {
    if (!isKnownProvider(provider)) {
        return Result::failure("bad_request", "неизвестный провайдер: " + provider);
    }
    if (code.empty() || state.empty()) {
        return Result::failure("bad_request", "code и state обязательны");
    }
    if (config_.googleClientId.empty() || config_.googleClientSecret.empty()) {
        return Result::failure("not_configured",
                               "Google OAuth не настроен на сервере (AURA_GOOGLE_CLIENT_ID/SECRET)");
    }

    const auto found = database_.db().findOauthState(state);
    if (!found || found->userId != userId || found->provider != provider) {
        return Result::failure("bad_request", "state недействителен");
    }
    if (!found->usedAt.empty() || found->expiresAt < isoNow()) {
        return Result::failure("bad_request", "state уже использован или истёк");
    }
    // Одноразовость: помечаем до обмена, чтобы параллельный повтор не прошёл.
    database_.db().useOauthState(state);

    const std::string body = formBody({
        {"grant_type", "authorization_code"},
        {"code", code},
        {"client_id", config_.googleClientId},
        {"client_secret", config_.googleClientSecret},
        {"redirect_uri", found->redirectUri},
        {"code_verifier", found->verifier},
    });
    std::string error;
    const auto response = net::httpRequest("POST", config_.googleTokenUrl,
                                           {{"Content-Type", "application/x-www-form-urlencoded"}},
                                           body, config_.aiTimeoutMs, error);
    if (!error.empty()) {
        AURA_LOG(log::Level::Warn, "integrations") << "token endpoint недоступен: " << error;
        return Result::failure("upstream_error", "не удалось связаться с эндаунтом токенов: " + error);
    }
    Json token = Json::parse(response.body, &error);
    if (!response.ok()) {
        const std::string oauthError = token.getString("error", "HTTP " + std::to_string(response.status));
        const std::string description = token.getString("error_description");
        return Result::failure("oauth_error", description.empty() ? oauthError : oauthError + ": " + description);
    }
    if (token.getString("access_token").empty()) {
        return Result::failure("oauth_error", "провайдер не вернул access_token");
    }

    TokenBlob blob;
    blob.accessToken = token.getString("access_token");
    blob.refreshToken = token.getString("refresh_token");
    blob.idToken = token.getString("id_token");
    blob.scope = token.getString("scope", scopeForProvider(provider));
    blob.account = accountFromIdToken(blob.idToken);
    const long long expiresIn = token.getInt("expires_in", 0);
    if (expiresIn > 0) blob.expiresAtMs = nowMillis() + expiresIn * 1000;

    std::string encrypted;
    if (!encryptToken(blob, encrypted)) {
        return Result::failure("internal", "не удалось зашифровать токены");
    }

    IntegrationConnectionRecord record;
    record.userId = userId;
    record.provider = provider;
    record.account = blob.account;
    record.scope = blob.scope;
    record.tokenEncrypted = encrypted;
    record.status = "active";
    long long connectionId = 0;
    const auto upserted = database_.db().upsertIntegrationConnection(record, connectionId);
    if (!upserted.ok) return Result::failure("internal", upserted.message);

    record.id = connectionId;
    const auto saved = database_.db().findIntegrationConnection(userId, provider);
    Result result;
    result.ok = true;
    result.payload = saved ? saved->toJson() : record.toJson();
    AURA_LOG(log::Level::Info, "integrations")
        << "подключено: user=" << userId << " provider=" << provider
        << " account=" << (blob.account.empty() ? "?" : blob.account);
    return result;
}

// ------------------------------------------------------------------- list

IntegrationManager::Result IntegrationManager::list(long long userId) {
    Result result;
    result.ok = true;
    result.payload.set("providers", providerCatalog());
    Json connections = Json::array();
    for (const auto& record : database_.db().listIntegrationConnections(userId)) {
        connections.push(record.toJson());  // без token_encrypted
    }
    result.payload.set("connections", connections);
    return result;
}

// ----------------------------------------------------------------- revoke

IntegrationManager::Result IntegrationManager::revoke(long long userId, long long connectionId) {
    std::optional<IntegrationConnectionRecord> target;
    for (const auto& record : database_.db().listIntegrationConnections(userId)) {
        if (record.id == connectionId) {
            target = record;
            break;
        }
    }
    if (!target) return Result::failure("not_found", "подключение не найдено");

    // Отзыв на стороне провайдера — лучший результат: при сетевом сбое
    // запись сохраняем, чтобы пользователь мог повторить.
    TokenBlob token;
    if (decryptToken(target->tokenEncrypted, token)) {
        const std::string revokeToken =
            token.refreshToken.empty() ? token.accessToken : token.refreshToken;
        if (!revokeToken.empty()) {
            std::string error;
            const auto response = net::httpRequest(
                "POST", config_.googleRevokeUrl,
                {{"Content-Type", "application/x-www-form-urlencoded"}},
                formBody({{"token", revokeToken}}), config_.aiTimeoutMs, error);
            if (!error.empty()) {
                return Result::failure("upstream_error",
                                       "не удалось связаться с Google для отзыва: " + error);
            }
            if (!response.ok() && response.status != 400) {
                // 400 = токен уже недействителен — локально всё равно чистим.
                return Result::failure("upstream_error",
                                       "Google отклонил отзыв: HTTP " + std::to_string(response.status));
            }
        }
    } else {
        AURA_LOG(log::Level::Warn, "integrations")
            << "токен подключения " << connectionId << " не расшифрован — удаляю локально";
    }

    const auto removed = database_.db().deleteIntegrationConnection(connectionId);
    if (!removed.ok) return Result::failure("internal", removed.message);
    Result result;
    result.ok = true;
    result.payload.set("id", Json(connectionId));
    result.payload.set("status", Json("revoked"));
    return result;
}

// ------------------------------------------------------------------- sync

IntegrationManager::Result IntegrationManager::sync(long long userId, long long connectionId) {
    std::optional<IntegrationConnectionRecord> target;
    for (const auto& record : database_.db().listIntegrationConnections(userId)) {
        if (record.id == connectionId) {
            target = record;
            break;
        }
    }
    if (!target) return Result::failure("not_found", "подключение не найдено");

    Result result;
    result.payload.set("provider", Json(target->provider));
    std::string error;

    if (target->provider == "google_calendar") {
        const std::string url = config_.googleCalendarUrl +
                                "/calendars/primary/events?timeMin=" + percentEncode(isoNow()) +
                                "&maxResults=10&singleEvents=true&orderBy=startTime";
        Json response;
        if (!googleRequest(*target, "GET", url, "", "", response, error)) {
            return Result::failure("upstream_error", error);
        }
        const Json items = response.get("items");
        int count = 0;
        for (const auto& item : items.items()) {
            const std::string summary = item.getString("summary", "(без названия)");
            const Json start = item.get("start");
            std::string when = start.getString("dateTime");
            if (when.empty()) when = start.getString("date");
            const std::string text = "Google Календарь: " + (when.empty() ? "?" : when) + " — " + summary;
            const auto saved = database_.db().upsertMemory(userId, "schedule.google", text, 0.8,
                                                           {"google", "calendar"});
            if (saved.ok) ++count;
        }
        database_.db().touchIntegrationUsed(connectionId);
        result.ok = true;
        result.payload.set("events", Json(static_cast<long long>(items.items().size())));
        result.payload.set("facts", Json(static_cast<long long>(count)));
        return result;
    }

    if (target->provider == "google_gmail") {
        Json response;
        if (!googleRequest(*target, "GET", config_.googleGmailUrl + "/users/me/profile", "", "",
                           response, error)) {
            return Result::failure("upstream_error", error);
        }
        database_.db().touchIntegrationUsed(connectionId);
        result.ok = true;
        result.payload.set("profile_email", Json(response.getString("emailAddress")));
        return result;
    }

    return Result::failure("bad_request", "синхронизация не поддерживается: " + target->provider);
}

// ------------------------------------------------------------ accessToken

std::string IntegrationManager::accessToken(long long userId,
                                            const std::string& provider,
                                            std::string& errorOut) {
    const auto found = database_.db().findIntegrationConnection(userId, provider);
    if (!found || found->status != "active") {
        errorOut = "нет активного подключения " + provider;
        return {};
    }
    TokenBlob token;
    if (!decryptToken(found->tokenEncrypted, token)) {
        errorOut = "токен подключения повреждён — переподключите " + provider;
        return {};
    }
    const bool expiring = token.expiresAtMs > 0 && nowMillis() + 60000 >= token.expiresAtMs;
    if (expiring && !token.refreshToken.empty()) {
        if (!refreshAccessToken(*found, token, errorOut)) return {};
    }
    database_.db().touchIntegrationUsed(found->id);
    return token.accessToken;
}

// ---------------------------------------------------------------- private

std::string IntegrationManager::tokenKeyBytes() const {
    // Тот же вывод ключа, что у AuthManager::twoFactorKeyBytes (AURA_2FA_KEY,
    // fallback jwtSecret); AAD отделяет шифртексты токенов от TOTP-секретов.
    const std::string base = config_.twoFactorKey.empty() ? config_.jwtSecret : config_.twoFactorKey;
    return crypto::sha256(base);
}

bool IntegrationManager::encryptToken(const TokenBlob& token, std::string& outBase64) const {
    std::string blob;
    if (!crypto::aes256GcmEncrypt(tokenKeyBytes(), token.toJson().dump(), kTokenAad, blob)) return false;
    outBase64 = crypto::base64Encode(blob);
    return true;
}

bool IntegrationManager::decryptToken(const std::string& base64Blob, TokenBlob& outToken) const {
    std::string blob;
    if (!crypto::base64Decode(base64Blob, blob)) return false;
    std::string plaintext;
    if (!crypto::aes256GcmDecrypt(tokenKeyBytes(), blob, kTokenAad, plaintext)) return false;
    std::string error;
    const Json json = Json::parse(plaintext, &error);
    if (!error.empty() || !json.isObject()) return false;
    outToken = TokenBlob::fromJson(json);
    return !outToken.accessToken.empty();
}

bool IntegrationManager::googleRequest(const IntegrationConnectionRecord& connection,
                                       const std::string& method,
                                       const std::string& url,
                                       const std::string& contentType,
                                       const std::string& body,
                                       Json& outResponse,
                                       std::string& errorOut) {
    TokenBlob token;
    if (!decryptToken(connection.tokenEncrypted, token)) {
        errorOut = "токен подключения повреждён — переподключите сервис";
        return false;
    }
    const bool expiring = token.expiresAtMs > 0 && nowMillis() + 60000 >= token.expiresAtMs;
    if (expiring && !token.refreshToken.empty()) {
        std::string refreshError;
        if (!refreshAccessToken(connection, token, refreshError)) {
            errorOut = refreshError;
            return false;
        }
    }

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.accessToken},
        {"Accept", "application/json"},
    };
    if (!contentType.empty()) headers["Content-Type"] = contentType;

    std::string error;
    auto response = net::httpRequest(method, url, headers, body, config_.aiTimeoutMs, error);
    if (!error.empty()) {
        errorOut = "Google API недоступен: " + error;
        return false;
    }
    if (response.status == 401 && !token.refreshToken.empty()) {
        std::string refreshError;
        if (refreshAccessToken(connection, token, refreshError)) {
            headers["Authorization"] = "Bearer " + token.accessToken;
            response = net::httpRequest(method, url, headers, body, config_.aiTimeoutMs, error);
            if (!error.empty()) {
                errorOut = "Google API недоступен: " + error;
                return false;
            }
        }
    }
    if (!response.ok()) {
        std::string detail;
        const Json parsed = Json::parse(response.body, nullptr);
        const Json err = parsed.get("error");
        detail = err.getString("message");
        errorOut = "Google API вернул HTTP " + std::to_string(response.status);
        if (!detail.empty()) errorOut += ": " + detail;
        return false;
    }
    if (!response.body.empty()) {
        const Json parsed = Json::parse(response.body, &error);
        if (!error.empty()) {
            errorOut = "некорректный JSON от Google API";
            return false;
        }
        outResponse = parsed;
    }
    return true;
}

bool IntegrationManager::refreshAccessToken(const IntegrationConnectionRecord& connection,
                                            TokenBlob& token,
                                            std::string& errorOut) {
    if (token.refreshToken.empty()) {
        errorOut = "access_token истёк, refresh_token отсутствует — переподключите сервис";
        return false;
    }
    const std::string body = formBody({
        {"grant_type", "refresh_token"},
        {"refresh_token", token.refreshToken},
        {"client_id", config_.googleClientId},
        {"client_secret", config_.googleClientSecret},
    });
    std::string error;
    const auto response = net::httpRequest("POST", config_.googleTokenUrl,
                                           {{"Content-Type", "application/x-www-form-urlencoded"}},
                                           body, config_.aiTimeoutMs, error);
    if (!error.empty()) {
        errorOut = "не удалось связаться с эндаунтом токенов: " + error;
        return false;
    }
    const Json json = Json::parse(response.body, nullptr);
    if (!response.ok() || json.getString("access_token").empty()) {
        errorOut = "Google отклонил обновление токена: " +
                   json.getString("error", "HTTP " + std::to_string(response.status));
        database_.db().updateIntegrationToken(connection.id, connection.tokenEncrypted, "expired", errorOut);
        return false;
    }
    token.accessToken = json.getString("access_token");
    if (!json.getString("refresh_token").empty()) token.refreshToken = json.getString("refresh_token");
    const long long expiresIn = json.getInt("expires_in", 0);
    token.expiresAtMs = expiresIn > 0 ? nowMillis() + expiresIn * 1000 : 0;

    std::string encrypted;
    if (!encryptToken(token, encrypted)) {
        errorOut = "не удалось зашифровать обновлённые токены";
        return false;
    }
    const auto updated = database_.db().updateIntegrationToken(connection.id, encrypted, "active", "");
    if (!updated.ok) {
        errorOut = updated.message;
        return false;
    }
    return true;
}

std::string IntegrationManager::accountFromIdToken(const std::string& idToken) {
    // id_token = header.payload.signature; payload — base64url(JSON).
    const auto first = idToken.find('.');
    const auto second = idToken.find('.', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) return {};
    std::string payloadRaw;
    if (!crypto::base64Decode(idToken.substr(first + 1, second - first - 1), payloadRaw)) return {};
    const Json payload = Json::parse(payloadRaw, nullptr);
    return payload.getString("email");
}

Json IntegrationManager::TokenBlob::toJson() const {
    Json json = Json::object();
    json.set("access_token", Json(accessToken));
    json.set("refresh_token", Json(refreshToken));
    json.set("expires_at_ms", Json(expiresAtMs));
    json.set("id_token", Json(idToken));
    json.set("account", Json(account));
    json.set("scope", Json(scope));
    return json;
}

IntegrationManager::TokenBlob IntegrationManager::TokenBlob::fromJson(const Json& json) {
    TokenBlob token;
    token.accessToken = json.getString("access_token");
    token.refreshToken = json.getString("refresh_token");
    token.expiresAtMs = json.getInt("expires_at_ms", 0);
    token.idToken = json.getString("id_token");
    token.account = json.getString("account");
    token.scope = json.getString("scope");
    return token;
}

}  // namespace aura
