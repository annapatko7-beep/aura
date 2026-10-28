// selfvoy/oauthauthmanager.cpp
#include "selfvoy/oauthauthmanager.h"

#include <chrono>
#include <ctime>

#include "selfvoy/crypto.h"
#include "selfvoy/log.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace {

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

std::string formBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        if (!body.empty()) body.push_back('&');
        body += percentEncode(key) + "=" + percentEncode(value);
    }
    return body;
}

}  // namespace

OAuthAuthManager::OAuthAuthManager(DatabaseManager& database, AuthManager& authManager, const Config& config)
    : database_(database), authManager_(authManager), config_(config) {}

bool OAuthAuthManager::isKnownProvider(const std::string& provider) {
    return provider == "google" || provider == "github" || provider == "yandex" ||
           provider == "vk" || provider == "discord";
}

Json OAuthAuthManager::providerCatalog() const {
    Json list = Json::array();

    auto add = [&](const std::string& id, const std::string& name, const std::string& icon, bool configured) {
        Json p = Json::object();
        p.set("provider", Json(id));
        p.set("name", Json(name));
        p.set("icon", Json(icon));
        p.set("enabled", Json(configured || config_.oauthDevMock));
        list.push(p);
    };

    add("google", "Google", "google", !config_.googleAuthClientId.empty());
    add("github", "GitHub", "github", !config_.githubClientId.empty());
    add("yandex", "Яндекс ID", "yandex", !config_.yandexClientId.empty());
    add("vk", "VK ID", "vk", !config_.vkClientId.empty());
    add("discord", "Discord", "discord", !config_.discordClientId.empty());

    return list;
}

OAuthAuthManager::Result OAuthAuthManager::begin(const std::string& provider,
                                                 const std::string& redirectUri) {
    if (!isKnownProvider(provider)) {
        return Result::failure(protocol::code::kBadRequest, "неизвестный провайдер: " + provider);
    }
    if (redirectUri.empty()) {
        return Result::failure(protocol::code::kBadRequest, "redirect_uri обязателен");
    }

    // Проверка наличия Client ID (в dev-mock режиме разрешаем тестовый URL)
    const bool isMock = config_.oauthDevMock;
    if (!isMock) {
        if (provider == "google" && config_.googleAuthClientId.empty()) {
            return Result::failure("not_configured", "Google OAuth не настроен на сервере");
        }
        if (provider == "github" && config_.githubClientId.empty()) {
            return Result::failure("not_configured", "GitHub OAuth не настроен на сервере");
        }
        if (provider == "yandex" && config_.yandexClientId.empty()) {
            return Result::failure("not_configured", "Яндекс OAuth не настроен на сервере");
        }
        if (provider == "vk" && config_.vkClientId.empty()) {
            return Result::failure("not_configured", "VK ID OAuth не настроен на сервере");
        }
        if (provider == "discord" && config_.discordClientId.empty()) {
            return Result::failure("not_configured", "Discord OAuth не настроен на сервере");
        }
    }

    // PKCE (RFC 7636): verifier + challenge S256
    const std::string verifier = crypto::base64UrlEncode(crypto::randomBytes(32));
    const std::string challenge = crypto::base64UrlEncode(crypto::sha256(verifier));
    const std::string state = crypto::randomHex(16);

    AuthOauthStateRecord record;
    record.state = state;
    record.provider = provider;
    record.verifier = verifier;
    record.redirectUri = redirectUri;
    record.expiresAt = isoPlusSeconds(config_.oauthStateTtlSec);

    const auto created = database_.db().createAuthOauthState(record);
    if (!created.ok) return Result::failure("internal", created.message);

    std::string authUrl;
    if (provider == "google") {
        authUrl = config_.googleAuthUrl +
                  "?response_type=code"
                  "&client_id=" + percentEncode(config_.googleAuthClientId.empty() ? "mock-google-client" : config_.googleAuthClientId) +
                  "&redirect_uri=" + percentEncode(redirectUri) +
                  "&scope=" + percentEncode("openid email profile") +
                  "&state=" + state +
                  "&code_challenge=" + challenge +
                  "&code_challenge_method=S256"
                  "&prompt=select_account";
    } else if (provider == "github") {
        authUrl = config_.githubAuthUrl +
                  "?client_id=" + percentEncode(config_.githubClientId.empty() ? "mock-github-client" : config_.githubClientId) +
                  "&redirect_uri=" + percentEncode(redirectUri) +
                  "&scope=" + percentEncode("read:user user:email") +
                  "&state=" + state;
    } else if (provider == "yandex") {
        authUrl = config_.yandexAuthUrl +
                  "?response_type=code"
                  "&client_id=" + percentEncode(config_.yandexClientId.empty() ? "mock-yandex-client" : config_.yandexClientId) +
                  "&redirect_uri=" + percentEncode(redirectUri) +
                  "&state=" + state +
                  "&force_confirm=yes";
    } else if (provider == "vk") {
        authUrl = config_.vkAuthUrl +
                  "?response_type=code"
                  "&client_id=" + percentEncode(config_.vkClientId.empty() ? "mock-vk-client" : config_.vkClientId) +
                  "&redirect_uri=" + percentEncode(redirectUri) +
                  "&scope=" + percentEncode("vkid.personal_info email") +
                  "&state=" + state +
                  "&code_challenge=" + challenge +
                  "&code_challenge_method=S256";
    } else if (provider == "discord") {
        authUrl = config_.discordAuthUrl +
                  "?response_type=code"
                  "&client_id=" + percentEncode(config_.discordClientId.empty() ? "mock-discord-client" : config_.discordClientId) +
                  "&redirect_uri=" + percentEncode(redirectUri) +
                  "&scope=" + percentEncode("identify email") +
                  "&state=" + state +
                  "&prompt=consent";
    }

    Json payload = Json::object();
    payload.set("provider", Json(provider));
    payload.set("authorize_url", Json(authUrl));
    payload.set("state", Json(state));
    payload.set("expires_in", Json(config_.oauthStateTtlSec));
    return Result::success(payload);
}

OAuthAuthManager::Result OAuthAuthManager::fetchProfile(const std::string& provider,
                                                       const std::string& code,
                                                       const std::string& verifier,
                                                       const std::string& redirectUri,
                                                       UserProfile& profile) {
    // В тестовом/mock режиме либо если код начинается с "mock_"
    if (config_.oauthDevMock || code.rfind("mock_", 0) == 0) {
        profile.providerUserId = "mock_" + provider + "_" + crypto::randomHex(4);
        profile.email = "user." + provider + "@example.com";
        profile.displayName = "Пользователь " + provider;
        profile.avatarUrl = "";
        return Result::success();
    }

    std::string tokenEndpoint;
    std::string body;
    std::map<std::string, std::string> headers = {{"Content-Type", "application/x-www-form-urlencoded"}};

    if (provider == "google") {
        tokenEndpoint = config_.googleTokenUrl;
        body = formBody({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", config_.googleAuthClientId},
            {"client_secret", config_.googleAuthClientSecret},
            {"redirect_uri", redirectUri},
            {"code_verifier", verifier},
        });
    } else if (provider == "github") {
        tokenEndpoint = config_.githubTokenUrl;
        headers["Accept"] = "application/json";
        body = formBody({
            {"client_id", config_.githubClientId},
            {"client_secret", config_.githubClientSecret},
            {"code", code},
            {"redirect_uri", redirectUri},
        });
    } else if (provider == "yandex") {
        tokenEndpoint = config_.yandexTokenUrl;
        body = formBody({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", config_.yandexClientId},
            {"client_secret", config_.yandexClientSecret},
        });
    } else if (provider == "vk") {
        tokenEndpoint = config_.vkTokenUrl;
        body = formBody({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", config_.vkClientId},
            {"client_secret", config_.vkClientSecret},
            {"redirect_uri", redirectUri},
            {"code_verifier", verifier},
        });
    } else if (provider == "discord") {
        tokenEndpoint = config_.discordTokenUrl;
        body = formBody({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", config_.discordClientId},
            {"client_secret", config_.discordClientSecret},
            {"redirect_uri", redirectUri},
        });
    }

    std::string httpErr;
    auto tokenResp = net::httpRequest("POST", tokenEndpoint, headers, body, config_.aiTimeoutMs, httpErr);
    if (!httpErr.empty() || !tokenResp.ok()) {
        SELFVOY_LOG(log::Level::Warn, "oauth") << "token exchange failed for " << provider << ": " << httpErr;
        return Result::failure("oauth_error", "не удалось обменять код на токен у " + provider);
    }

    std::string parseErr;
    Json tokenJson = Json::parse(tokenResp.body, &parseErr);
    std::string accessToken = tokenJson.getString("access_token");
    if (accessToken.empty()) {
        return Result::failure("oauth_error", "провайдер " + provider + " не вернул access_token");
    }

    // Запрос профиля пользователя
    if (provider == "google") {
        auto userResp = net::httpRequest("GET", config_.googleUserinfoUrl,
                                         {{"Authorization", "Bearer " + accessToken}}, "",
                                         config_.aiTimeoutMs, httpErr);
        if (!userResp.ok()) return Result::failure("oauth_error", "не удалось получить профиль Google");
        Json user = Json::parse(userResp.body, &parseErr);
        profile.providerUserId = user.getString("sub");
        profile.email = user.getString("email");
        profile.displayName = user.getString("name", user.getString("email"));
        profile.avatarUrl = user.getString("picture");
    } else if (provider == "github") {
        auto userResp = net::httpRequest("GET", config_.githubUserUrl,
                                         {{"Authorization", "Bearer " + accessToken},
                                          {"User-Agent", "Selfvoy-Server"}}, "",
                                         config_.aiTimeoutMs, httpErr);
        if (!userResp.ok()) return Result::failure("oauth_error", "не удалось получить профиль GitHub");
        Json user = Json::parse(userResp.body, &parseErr);
        profile.providerUserId = std::to_string(user.getInt("id"));
        profile.displayName = user.getString("name", user.getString("login"));
        profile.avatarUrl = user.getString("avatar_url");
        profile.email = user.getString("email");

        if (profile.email.empty()) {
            auto emailResp = net::httpRequest("GET", "https://api.github.com/user/emails",
                                              {{"Authorization", "Bearer " + accessToken},
                                               {"User-Agent", "Selfvoy-Server"}}, "",
                                              config_.aiTimeoutMs, httpErr);
            if (emailResp.ok()) {
                Json emails = Json::parse(emailResp.body, &parseErr);
                for (const auto& item : emails.items()) {
                    if (item.getBool("primary", false)) {
                        profile.email = item.getString("email");
                        break;
                    }
                }
            }
        }
    } else if (provider == "yandex") {
        auto userResp = net::httpRequest("GET", config_.yandexUserUrl + "?format=json",
                                         {{"Authorization", "OAuth " + accessToken}}, "",
                                         config_.aiTimeoutMs, httpErr);
        if (!userResp.ok()) return Result::failure("oauth_error", "не удалось получить профиль Яндекс");
        Json user = Json::parse(userResp.body, &parseErr);
        profile.providerUserId = user.getString("id");
        profile.email = user.getString("default_email");
        profile.displayName = user.getString("real_name", user.getString("display_name"));
        profile.avatarUrl = user.getString("default_avatar_id");
        if (!profile.avatarUrl.empty()) {
            profile.avatarUrl = "https://avatars.yandex.net/get-yapic/" + profile.avatarUrl + "/islands-200";
        }
    } else if (provider == "vk") {
        auto userResp = net::httpRequest("GET", config_.vkUserUrl,
                                         {{"Authorization", "Bearer " + accessToken}}, "",
                                         config_.aiTimeoutMs, httpErr);
        if (!userResp.ok()) return Result::failure("oauth_error", "не удалось получить профиль VK");
        Json user = Json::parse(userResp.body, &parseErr);
        Json userObj = user.contains("user") ? user.get("user") : user;
        profile.providerUserId = std::to_string(userObj.getInt("user_id"));
        profile.email = userObj.getString("email");
        std::string firstName = userObj.getString("first_name");
        std::string lastName = userObj.getString("last_name");
        profile.displayName = firstName.empty() ? "VK User" : firstName + (lastName.empty() ? "" : " " + lastName);
        profile.avatarUrl = userObj.getString("avatar");
    } else if (provider == "discord") {
        auto userResp = net::httpRequest("GET", config_.discordUserUrl,
                                         {{"Authorization", "Bearer " + accessToken}}, "",
                                         config_.aiTimeoutMs, httpErr);
        if (!userResp.ok()) return Result::failure("oauth_error", "не удалось получить профиль Discord");
        Json user = Json::parse(userResp.body, &parseErr);
        profile.providerUserId = user.getString("id");
        profile.email = user.getString("email");
        profile.displayName = user.getString("global_name", user.getString("username"));
        std::string avatarHash = user.getString("avatar");
        if (!avatarHash.empty()) {
            profile.avatarUrl = "https://cdn.discordapp.com/avatars/" + profile.providerUserId + "/" + avatarHash + ".png";
        }
    }

    if (profile.providerUserId.empty()) {
        return Result::failure("oauth_error", "не удалось определить ID пользователя у " + provider);
    }
    if (profile.email.empty()) {
        profile.email = provider + "_" + profile.providerUserId + "@oauth.selfvoy";
    }

    return Result::success();
}

OAuthAuthManager::Result OAuthAuthManager::callback(const std::string& provider,
                                                    const std::string& code,
                                                    const std::string& state,
                                                    const std::string& device,
                                                    const std::string& deviceId,
                                                    const std::string& remoteAddr) {
    (void)deviceId;
    if (!isKnownProvider(provider)) {
        return Result::failure(protocol::code::kBadRequest, "неизвестный провайдер: " + provider);
    }
    if (code.empty() || state.empty()) {
        return Result::failure(protocol::code::kBadRequest, "code и state обязательны");
    }

    const auto foundState = database_.db().findAuthOauthState(state);
    if (!foundState || foundState->provider != provider) {
        return Result::failure(protocol::code::kBadRequest, "state недействителен");
    }
    if (!foundState->usedAt.empty() || foundState->expiresAt < isoNow()) {
        return Result::failure(protocol::code::kBadRequest, "state уже использован или истёк");
    }
    if (!database_.db().useAuthOauthState(state).ok) {
        return Result::failure(protocol::code::kBadRequest, "state уже использован или истёк");
    }

    UserProfile profile;
    const auto fetchRes = fetchProfile(provider, code, foundState->verifier, foundState->redirectUri, profile);
    if (!fetchRes.ok) return fetchRes;

    // Поиск существующего аккаунта
    long long userId = 0;
    const auto oauthAccount = database_.db().findOauthAccount(provider, profile.providerUserId);
    if (oauthAccount) {
        userId = oauthAccount->userId;
    } else {
        // Проверяем, есть ли пользователь с таким email
        const auto existingUser = database_.db().findUserByEmail(profile.email);
        if (existingUser) {
            userId = existingUser->id;
            // Привязываем аккаунт OAuth к уже существующему пользователю
            UserOauthAccountRecord linkRecord;
            linkRecord.userId = userId;
            linkRecord.provider = provider;
            linkRecord.providerUserId = profile.providerUserId;
            linkRecord.email = profile.email;
            database_.db().linkOauthAccount(linkRecord);
            // Если email не был подтверждён, считаем его подтверждённым через доверенный OAuth
            database_.db().setEmailVerified(userId, true);
        } else {
            // Создаём нового пользователя
            const std::string dummyPasswordHash = "oauth$" + provider + "$" + crypto::randomHex(16);
            const std::string displayName = profile.displayName.empty() ? ("Пользователь " + provider) : profile.displayName;
            const auto created = database_.db().createUser(profile.email, dummyPasswordHash, displayName, userId);
            if (!created.ok) {
                return Result::failure("internal", "не удалось создать аккаунт: " + created.message);
            }
            database_.db().setEmailVerified(userId, true);
            if (!profile.avatarUrl.empty()) {
                Json fields = Json::object();
                fields.set("avatar_url", Json(profile.avatarUrl));
                database_.db().updateProfile(userId, fields);
            }
            UserOauthAccountRecord linkRecord;
            linkRecord.userId = userId;
            linkRecord.provider = provider;
            linkRecord.providerUserId = profile.providerUserId;
            linkRecord.email = profile.email;
            database_.db().linkOauthAccount(linkRecord);
        }
    }

    const auto user = database_.db().findUserById(userId);
    if (!user) {
        return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    }

    // Выпуск сессии, JWT и refresh токена через AuthManager
    const auto authResult = authManager_.loginOAuthUser(*user, device, remoteAddr);
    if (!authResult.ok) {
        return Result::failure(authResult.code, authResult.message);
    }

    Result res;
    res.ok = true;
    res.payload = authResult.payload;
    res.userId = authResult.userId;
    res.displayName = authResult.displayName;
    res.email = authResult.email;
    res.jwtId = authResult.jwtId;
    return res;
}

}  // namespace selfvoy
