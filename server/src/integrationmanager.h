// selfvoy/integrationmanager.h — интеграции с внешними сервисами (этап 9).
//
// Google OAuth 2.0 Authorization Code + PKCE (S256). Токены хранятся
// зашифрованными (AES-256-GCM, ключ — SHA-256 от SELFVOY_2FA_KEY) в таблице
// integration_connections; одноразовые state/verifier — в
// integration_oauth_states. Наружу секреты не отдаются никогда.
#pragma once

#include <string>

#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/json.h"

namespace selfvoy {

class IntegrationManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    IntegrationManager(DatabaseManager& database, const Config& config);

    // Каталог провайдеров: [{provider, name, description, scope}].
    static Json providerCatalog();
    static bool isKnownProvider(const std::string& provider);
    static std::string scopeForProvider(const std::string& provider);

    // Шаг 1: ссылка на consent-экран Google; state/verifier одноразово
    // сохраняются на oauthStateTtlSec.
    Result begin(long long userId, const std::string& provider, const std::string& redirectUri);

    // Шаг 2: обмен code+verifier на токены, шифрование, сохранение.
    Result callback(long long userId, const std::string& provider,
                    const std::string& code, const std::string& state);

    // Список подключений пользователя + каталог (без токенов).
    Result list(long long userId);

    // Отзыв доступа: POST /revoke провайдеру + удаление записи.
    Result revoke(long long userId, long long connectionId);

    // Подключение CalDAV / iCalendar (Apple iCloud, Fastmail, Nextcloud)
    Result connectCalDav(long long userId, const std::string& provider,
                         const std::string& caldavUrl, const std::string& username,
                         const std::string& password);

    // Активная синхронизация: Google Calendar / CalDAV — события в память (schedule.*),
    // Google Gmail — проверка ящика (/users/me/profile).
    Result sync(long long userId, long long connectionId);

    // Актуальный access_token с авто-обновлением по refresh_token
    // (для ToolManager). Пустая строка = нет активного подключения.
    std::string accessToken(long long userId, const std::string& provider, std::string& errorOut);

    const std::string& gmailUrl() const { return config_.googleGmailUrl; }
    const std::string& calendarUrl() const { return config_.googleCalendarUrl; }

private:
    // Дешифрованный токен подключения (в БД — только шифртекст).
    struct TokenBlob {
        std::string accessToken;
        std::string refreshToken;
        long long expiresAtMs = 0;  // epoch ms; 0 = срок неизвестен
        std::string idToken;
        std::string account;
        std::string scope;

        Json toJson() const;
        static TokenBlob fromJson(const Json& json);
    };

    // Ключ шифрования токенов (зеркало AuthManager::twoFactorKeyBytes,
    // но с отдельным AAD, чтобы шифртексты не были взаимозаменяемы).
    std::string tokenKeyBytes() const;
    bool encryptToken(const TokenBlob& token, std::string& outBase64) const;
    bool decryptToken(const std::string& base64Blob, TokenBlob& outToken) const;

    // Запрос к Google-API с Bearer-токеном; при 401 — refresh и одна попытка.
    bool googleRequest(const IntegrationConnectionRecord& connection,
                       const std::string& method,
                       const std::string& url,
                       const std::string& contentType,
                       const std::string& body,
                       Json& outResponse,
                       std::string& errorOut);

    // Обмен refresh_token на новый access_token; обновляет запись в БД.
    bool refreshAccessToken(const IntegrationConnectionRecord& connection,
                            TokenBlob& token,
                            std::string& errorOut);

    // Email из payload id_token (без проверки подписи — только для
    // отображения: выдача токена уже подтверждена самим Google).
    static std::string accountFromIdToken(const std::string& idToken);

    DatabaseManager& database_;
    const Config& config_;
};

}  // namespace selfvoy
