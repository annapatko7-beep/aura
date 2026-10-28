// selfvoy/oauthauthmanager.h
// Управление входом и быстрой регистрацией через OAuth (Google, GitHub, Yandex, VK ID, Discord).
#pragma once

#include <string>
#include <vector>

#include "selfvoy/authmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/json.h"

namespace selfvoy {

class OAuthAuthManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();
        long long userId = 0;
        std::string displayName;
        std::string email;
        std::string jwtId;

        static Result success(const Json& payload = Json::object()) {
            Result r;
            r.ok = true;
            r.payload = payload;
            return r;
        }

        static Result failure(const std::string& code, const std::string& message) {
            Result r;
            r.ok = false;
            r.code = code;
            r.message = message;
            return r;
        }
    };

    struct UserProfile {
        std::string providerUserId;
        std::string email;
        std::string displayName;
        std::string avatarUrl;
    };

    OAuthAuthManager(DatabaseManager& database, AuthManager& authManager, const Config& config);

    // Список провайдеров для UI (кнопки входа)
    Json providerCatalog() const;

    // Шаг 1: генерация state + PKCE, сохранение в БД, выдача authorize_url
    Result begin(const std::string& provider, const std::string& redirectUri);

    // Шаг 2: обмен code на токены у провайдера, получение профиля, создание/поиск юзера, вход
    Result callback(const std::string& provider,
                    const std::string& code,
                    const std::string& state,
                    const std::string& device,
                    const std::string& deviceId,
                    const std::string& remoteAddr);

    // Проверка, известен ли провайдер
    static bool isKnownProvider(const std::string& provider);

private:
    Result fetchProfile(const std::string& provider,
                        const std::string& code,
                        const std::string& verifier,
                        const std::string& redirectUri,
                        UserProfile& profile);

    DatabaseManager& database_;
    AuthManager& authManager_;
    const Config& config_;
};

}  // namespace selfvoy
