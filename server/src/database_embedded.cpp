// selfvoy/database_embedded.cpp — файловое хранилище для режима без PostgreSQL.
//
// Повторяет семантику схемы (те же таблицы, те же ограничения) и используется:
//   * в юнит-тестах сервера;
//   * в демо-режиме (SELFVOY_DATABASE_URL не задан).
// Данные держатся в памяти и сбрасываются в JSON-файл после каждой записи.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "selfvoy/idatabase.h"
#include "selfvoy/json.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc

namespace selfvoy {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// UUID в каноническом виде (с дефисами) и 32 hex — один и тот же идентификатор.
std::string plainUuid(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        if (ch != '-') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

// Отметки времени сравниваются по секундам: «YYYY-MM-DDTHH:MM:SS» (первые 19
// символов) — у сессий и у created_at разная дробная часть.
std::string isoSecondsAgo(long long seconds) {
    const auto moment = std::chrono::system_clock::now() - std::chrono::seconds(seconds);
    const std::time_t time = std::chrono::system_clock::to_time_t(moment);
    std::tm tm{};
    gmtime_r(&time, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return buffer;
}

bool sessionLiveJson(const Json& item, const std::string& nowSeconds) {
    if (!item.getString("revoked_at").empty()) return false;
    const std::string expiresAt = item.getString("expires_at");
    return expiresAt.empty() || expiresAt.substr(0, 19) > nowSeconds;
}

class EmbeddedDatabase : public IDatabase {
public:
    explicit EmbeddedDatabase(std::string path) : path_(std::move(path)) {
        // Ключ счётчиков id — сразу (и для базы в памяти без connect): добавление ключа
        // позже перераспределило бы поля state_ и сделало висячими ссылки на таблицы.
        state_.set("sequences", Json::object());
    }

    std::string name() const override { return "embedded:" + path_; }

    bool connect(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(path_);
        if (!file) {
            if (::access(path_.c_str(), F_OK) == 0) {
                error = "файл встроенной БД " + path_ + " существует, но не читается (права доступа?)";
                return false;
            }
            state_ = Json::object();
            state_.set("sequences", Json::object());
            return true;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string parseError;
        const Json parsed = Json::parse(buffer.str(), &parseError);
        if (!parsed.isObject()) {
            // Раньше повреждённый файл молча заменялся пустой базой — при
            // следующей записи все аккаунты и данные терялись безвозвратно.
            error = "файл встроенной БД " + path_ + " повреждён (" +
                    (parseError.empty() ? std::string("ожидался JSON-объект") : parseError) +
                    "): восстановите его из резервной копии или удалите вручную";
            return false;
        }
        state_ = parsed;
        // Ключ счётчиков id создаётся сразу: добавление ключа позже перераспределило бы
        // поля state_ и сделало висячими ссылки на таблицы у вызывающего кода.
        if (!state_.get("sequences").isObject()) state_.set("sequences", Json::object());
        backfillBookingPrices();
        // Файл содержит хэши паролей и токенов — только владельцу процесса.
        ::chmod(path_.c_str(), 0600);
        return true;
    }

    bool healthy() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return true;
    }

    // ------------------------------------------------------------ users
    DatabaseError createUser(const std::string& email,
                             const std::string& passwordHash,
                             const std::string& displayName,
                             long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json& users = table("users");
        for (const auto& item : users.items()) {
            if (lower(item.getString("email")) == lower(email)) {
                return DatabaseError::failure("пользователь с таким email уже существует");
            }
        }
        const long long id = nextId("users");
        Json user = Json::object();
        user.set("id", Json(id));
        user.set("email", Json(email));
        user.set("password_hash", Json(passwordHash));
        user.set("display_name", Json(displayName));
        user.set("avatar_url", Json(""));
        user.set("bio", Json(""));
        user.set("timezone", Json("Europe/Moscow"));
        user.set("is_active", Json(true));
        user.set("email_verified", Json(false));  // подтверждается через auth.verifyEmail
        user.set("created_at", Json(isoNow()));
        users.push(user);

        // Триггер users_create_defaults: строка настроек создаётся сразу.
        Json preferences = Json::object();
        preferences.set("user_id", Json(id));
        preferences.set("diet", Json::array());
        preferences.set("transport", Json("walk"));
        Json hours = Json::array();
        for (const int hour : {10, 11, 12, 16, 17, 18, 19}) hours.push(Json(hour));
        preferences.set("preferred_hours", hours);
        Json work = Json::array();
        for (int hour = 9; hour <= 18; ++hour) work.push(Json(hour));
        preferences.set("work_hours", work);
        preferences.set("budget_limit", Json(0));
        preferences.set("city", Json(""));
        preferences.set("theme", Json("graphite"));
        preferences.set("birthday", Json(""));
        preferences.set("allergies", Json::array());
        preferences.set("onboarded", Json(false));
        Json notifications = Json::object();
        notifications.set("push", Json(true));
        notifications.set("email", Json(false));
        preferences.set("notifications", notifications);
        Json privacy = Json::object();
        privacy.set("share_free_slots_only", Json(true));
        privacy.set("hide_event_titles", Json(true));
        privacy.set("allow_place_choice", Json(true));
        preferences.set("privacy", privacy);
        preferences.set("updated_at", Json(isoNow()));
        table("user_preferences").push(preferences);

        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<UserRecord> findUserByEmail(const std::string& email) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("users").items()) {
            if (lower(item.getString("email")) == lower(email)) return toUser(item);
        }
        return std::nullopt;
    }

    std::optional<UserRecord> findUserById(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return findUserByIdLocked(id);
    }

    std::vector<UserRecord> searchUsers(const std::string& query, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<UserRecord> result;
        for (const auto& item : table("users").items()) {
            if (query.empty() || contains(item.getString("display_name"), query)) {
                result.push_back(toUser(item));
                if (static_cast<int>(result.size()) >= limit) break;
            }
        }
        return result;
    }

    DatabaseError touchUser(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") == userId) {
                item.set("last_seen_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("пользователь не найден");
    }

    DatabaseError updateProfile(long long userId, const Json& fields) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") != userId) continue;
            for (const char* key : {"display_name", "bio", "avatar_url"}) {
                if (fields.contains(key)) item.set(key, Json(fields.getString(key)));
            }
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("пользователь не найден");
    }

    // --------------------------------------------------------- sessions
    DatabaseError createSession(const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json session = Json::object();
        session.set("id", Json(record.id));
        session.set("user_id", Json(record.userId));
        session.set("jwt_id", Json(record.jwtId));
        session.set("device", Json(record.device));
        session.set("remote_addr", Json(record.remoteAddr));
        // Как DEFAULT now() в PostgreSQL: AuthManager время создания не задаёт,
        // и без этого sessions.list отдавал пустой created_at.
        session.set("created_at", Json(record.createdAt.empty() ? isoNow() : record.createdAt));
        session.set("expires_at", Json(record.expiresAt));
        session.set("revoked_at", Json(""));
        table("sessions").push(session);
        flush();
        return DatabaseError::success();
    }

    DatabaseError revokeSession(const std::string& jwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("sessions")) {
            if (item.getString("jwt_id") == jwtId) {
                item.set("revoked_at", Json(isoNow()));
                flush();
            }
        }
        return DatabaseError::success();
    }

    std::optional<SessionRecord> findSessionByJwt(const std::string& jwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("sessions").items()) {
            if (item.getString("jwt_id") != jwtId) continue;
            // Как и в PostgreSQL: отозванные и истёкшие сессии не находятся.
            if (!item.getString("revoked_at").empty()) return std::nullopt;
            const std::string expiresAt = item.getString("expires_at");
            if (!expiresAt.empty() && expiresAt <= isoNow()) return std::nullopt;
            return toSession(item);
        }
        return std::nullopt;
    }

    DatabaseError updatePasswordHash(long long userId, const std::string& hash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") == userId) {
                item.set("password_hash", Json(hash));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("пользователь не найден");
    }

    DatabaseError setEmailVerified(long long userId, bool verified) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") == userId) {
                item.set("email_verified", Json(verified));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("пользователь не найден");
    }

    DatabaseError setUserActive(long long userId, bool active) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") == userId) {
                item.set("is_active", Json(active));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("пользователь не найден");
    }

    DatabaseError deleteUser(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& users = mutableTable("users");
        auto it = std::find_if(users.begin(), users.end(),
                               [userId](const Json& item) { return item.getInt("id") == userId; });
        if (it == users.end()) {
            return DatabaseError::failure("пользователь не найден");
        }
        users.erase(it);

        const std::vector<std::string> userTables = {
            "sessions", "oauth_accounts", "auth_tokens", "two_factor_settings",
            "recovery_codes", "payment_accounts", "user_devices", "tasks",
            "user_memory", "user_settings"
        };
        for (const auto& tbl : userTables) {
            auto& rows = mutableTable(tbl);
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [userId](const Json& item) { return item.getInt("user_id") == userId; }),
                       rows.end());
        }
        flush();
        return DatabaseError::success();
    }

    std::vector<SessionRecord> listSessions(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionRecord> result;
        for (const auto& item : table("sessions").items()) {
            if (item.getInt("user_id") != userId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            result.push_back(toSession(item));
        }
        return result;
    }

    DatabaseError revokeSessionById(long long userId, const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("sessions")) {
            if (item.getString("id") == sessionId && item.getInt("user_id") == userId) {
                item.set("revoked_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("сессия не найдена");
    }

    DatabaseError revokeAllSessions(long long userId, const std::string& exceptJwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("sessions")) {
            if (item.getInt("user_id") != userId) continue;
            if (!exceptJwtId.empty() && item.getString("jwt_id") == exceptJwtId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            item.set("revoked_at", Json(isoNow()));
        }
        flush();
        return DatabaseError::success();
    }

    // ------------------------------------------------------- auth tokens
    DatabaseError createAuthToken(const AuthTokenRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json token = Json::object();
        token.set("id", Json(record.id));
        token.set("user_id", Json(record.userId));
        token.set("purpose", Json(record.purpose));
        token.set("token_hash", Json(record.tokenHash));
        token.set("created_at", Json(record.createdAt));
        token.set("expires_at", Json(record.expiresAt));
        token.set("used_at", Json(""));
        table("auth_tokens").push(token);
        flush();
        return DatabaseError::success();
    }

    std::optional<AuthTokenRecord> findAuthToken(const std::string& purpose,
                                                 const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("auth_tokens").items()) {
            if (item.getString("purpose") != purpose) continue;
            if (item.getString("token_hash") != tokenHash) continue;
            return toAuthToken(item);
        }
        return std::nullopt;
    }

    DatabaseError useAuthToken(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("auth_tokens")) {
            if (item.getString("id") == id) {
                item.set("used_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("код не найден");
    }

    DatabaseError deleteAuthTokens(long long userId, const std::string& purpose) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& tokens = mutableTable("auth_tokens");
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                    [&](const Json& item) {
                                        return item.getInt("user_id") == userId &&
                                               item.getString("purpose") == purpose;
                                    }),
                     tokens.end());
        flush();
        return DatabaseError::success();
    }

    // ---------------------------------------------------- refresh tokens
    DatabaseError createRefreshToken(const RefreshTokenRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json token = Json::object();
        token.set("id", Json(record.id));
        token.set("user_id", Json(record.userId));
        token.set("family_id", Json(record.familyId));
        token.set("token_hash", Json(record.tokenHash));
        token.set("device", Json(record.device));
        token.set("remote_addr", Json(record.remoteAddr));
        token.set("created_at", Json(record.createdAt));
        token.set("expires_at", Json(record.expiresAt));
        token.set("revoked_at", Json(""));
        token.set("replaced_by", Json(""));
        table("refresh_tokens").push(token);
        flush();
        return DatabaseError::success();
    }

    std::optional<RefreshTokenRecord> findRefreshToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("refresh_tokens").items()) {
            if (item.getString("token_hash") == tokenHash) return toRefreshToken(item);
        }
        return std::nullopt;
    }

    DatabaseError revokeRefreshToken(const std::string& id, const std::string& replacedBy) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("refresh_tokens")) {
            if (item.getString("id") == id) {
                if (!item.getString("revoked_at").empty()) {
                    return DatabaseError::failure("refresh-токен уже отозван");
                }
                item.set("revoked_at", Json(isoNow()));
                item.set("replaced_by", Json(replacedBy));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("refresh-токен не найден");
    }

    DatabaseError revokeRefreshFamily(const std::string& familyId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (familyId.empty()) return DatabaseError::success();
        for (auto& item : mutableTable("refresh_tokens")) {
            if (plainUuid(item.getString("family_id")) != plainUuid(familyId)) continue;
            if (!item.getString("revoked_at").empty()) continue;
            item.set("revoked_at", Json(isoNow()));
        }
        flush();
        return DatabaseError::success();
    }

    DatabaseError revokeAllUserRefreshTokens(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("refresh_tokens")) {
            if (item.getInt("user_id") != userId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            item.set("revoked_at", Json(isoNow()));
        }
        flush();
        return DatabaseError::success();
    }

    DatabaseError revokeUserRefreshTokensExcept(long long userId, const std::string& keepFamilyId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string keep = plainUuid(keepFamilyId);
        for (auto& item : mutableTable("refresh_tokens")) {
            if (item.getInt("user_id") != userId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            if (!keep.empty() && plainUuid(item.getString("family_id")) == keep) continue;
            item.set("revoked_at", Json(isoNow()));
        }
        flush();
        return DatabaseError::success();
    }

    // --------------------------------------------------------- audit log
    DatabaseError insertAudit(long long userId,
                              const std::string& kind,
                              const Json& detail,
                              const std::string& remoteAddr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json entry = Json::object();
        entry.set("id", Json(nextId("audit_logs")));
        entry.set("user_id", Json(userId));
        entry.set("kind", Json(kind));
        entry.set("detail", detail.isObject() ? detail : Json::object());
        entry.set("remote_addr", Json(remoteAddr));
        entry.set("created_at", Json(isoNow()));
        table("audit_logs").push(entry);
        flush();
        return DatabaseError::success();
    }

    std::vector<AuditRecord> listAudit(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditRecord> result;
        const auto& logs = table("audit_logs").items();
        // Последние события первыми.
        for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
            if (it->getInt("user_id") != userId) continue;
            AuditRecord record;
            record.id = it->getInt("id");
            record.userId = userId;
            record.kind = it->getString("kind");
            record.detail = it->get("detail");
            record.remoteAddr = it->getString("remote_addr");
            record.createdAt = it->getString("created_at");
            result.push_back(record);
            if (static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    // ------------------------------------------------------------ 2FA
    DatabaseError upsertTwoFactor(const TwoFactorRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rows = mutableTable("two_factor_settings");
        for (auto& item : rows) {
            if (item.getInt("user_id") == record.userId) {
                item.set("secret_encrypted", Json(record.secretEncrypted));
                item.set("enabled", Json(record.enabled));
                item.set("last_used_counter", Json(record.lastUsedCounter));
                item.set("enabled_at", Json(record.enabledAt));
                item.set("updated_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        Json row = Json::object();
        row.set("user_id", Json(record.userId));
        row.set("secret_encrypted", Json(record.secretEncrypted));
        row.set("enabled", Json(record.enabled));
        row.set("last_used_counter", Json(record.lastUsedCounter));
        row.set("created_at", Json(record.createdAt));
        row.set("enabled_at", Json(record.enabledAt));
        row.set("updated_at", Json(isoNow()));
        rows.push_back(row);
        flush();
        return DatabaseError::success();
    }

    std::optional<TwoFactorRecord> findTwoFactor(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("two_factor_settings").items()) {
            if (item.getInt("user_id") == userId) return toTwoFactor(item);
        }
        return std::nullopt;
    }

    DatabaseError setTwoFactorEnabled(long long userId, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("two_factor_settings")) {
            if (item.getInt("user_id") == userId) {
                item.set("enabled", Json(enabled));
                if (enabled && item.getString("enabled_at").empty()) {
                    item.set("enabled_at", Json(isoNow()));
                }
                item.set("updated_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError updateTwoFactorCounter(long long userId, long long counter) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("two_factor_settings")) {
            if (item.getInt("user_id") == userId) {
                item.set("last_used_counter", Json(counter));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError deleteTwoFactor(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rows = mutableTable("two_factor_settings");
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                   [&](const Json& item) { return item.getInt("user_id") == userId; }),
                   rows.end());
        flush();
        return DatabaseError::success();
    }

    DatabaseError replaceRecoveryCodes(long long userId,
                                       const std::vector<RecoveryCodeRecord>& codes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rows = mutableTable("recovery_codes");
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                   [&](const Json& item) { return item.getInt("user_id") == userId; }),
                   rows.end());
        for (const auto& code : codes) {
            Json row = Json::object();
            row.set("id", Json(code.id));
            row.set("user_id", Json(userId));
            row.set("code_hash", Json(code.codeHash));
            row.set("created_at", Json(code.createdAt));
            row.set("used_at", Json(""));
            rows.push_back(row);
        }
        flush();
        return DatabaseError::success();
    }

    std::optional<RecoveryCodeRecord> findRecoveryCode(long long userId,
                                                       const std::string& codeHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("recovery_codes").items()) {
            if (item.getInt("user_id") != userId) continue;
            if (item.getString("code_hash") != codeHash) continue;
            // Резервный код одноразовый: использованный не возвращаем.
            if (!item.getString("used_at").empty()) continue;
            return toRecoveryCode(item);
        }
        return std::nullopt;
    }

    DatabaseError useRecoveryCode(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("recovery_codes")) {
            if (item.getString("id") == id) {
                if (!item.getString("used_at").empty()) break;  // уже использован
                item.set("used_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("резервный код не найден");
    }

    int countUnusedRecoveryCodes(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const auto& item : table("recovery_codes").items()) {
            if (item.getInt("user_id") != userId) continue;
            if (item.getString("used_at").empty()) ++count;
        }
        return count;
    }

    DatabaseError addTrustedDevice(const TrustedDeviceRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json row = Json::object();
        row.set("id", Json(record.id));
        row.set("user_id", Json(record.userId));
        row.set("device_hash", Json(record.deviceHash));
        row.set("device", Json(record.device));
        row.set("remote_addr", Json(record.remoteAddr));
        row.set("created_at", Json(record.createdAt));
        row.set("expires_at", Json(record.expiresAt));
        row.set("last_used_at", Json(record.lastUsedAt));
        row.set("revoked_at", Json(""));
        table("trusted_devices").push(row);
        flush();
        return DatabaseError::success();
    }

    std::optional<TrustedDeviceRecord> findTrustedDevice(long long userId,
                                                         const std::string& deviceHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoNow();
        for (const auto& item : table("trusted_devices").items()) {
            if (item.getInt("user_id") != userId) continue;
            if (item.getString("device_hash") != deviceHash) continue;
            // Доверие действует, пока устройство не отозвано и срок не истёк.
            if (!item.getString("revoked_at").empty()) continue;
            if (item.getString("expires_at") <= now) continue;
            return toTrustedDevice(item);
        }
        return std::nullopt;
    }

    std::vector<TrustedDeviceRecord> listTrustedDevices(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoNow();
        std::vector<TrustedDeviceRecord> result;
        for (const auto& item : table("trusted_devices").items()) {
            if (item.getInt("user_id") != userId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            if (item.getString("expires_at") <= now) continue;
            result.push_back(toTrustedDevice(item));
        }
        return result;
    }

    DatabaseError touchTrustedDevice(long long userId, const std::string& deviceHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("trusted_devices")) {
            if (item.getInt("user_id") != userId) continue;
            if (item.getString("device_hash") != deviceHash) continue;
            item.set("last_used_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeTrustedDevice(long long userId, const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("trusted_devices")) {
            if (item.getInt("user_id") != userId) continue;
            if (item.getString("id") != id) continue;
            item.set("revoked_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeAllTrustedDevices(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("trusted_devices")) {
            if (item.getInt("user_id") != userId) continue;
            if (!item.getString("revoked_at").empty()) continue;
            item.set("revoked_at", Json(isoNow()));
        }
        flush();
        return DatabaseError::success();
    }

    // ------------------------------------------------------------ chats
    DatabaseError createChat(const std::string& kind,
                             const std::string& title,
                             long long createdBy,
                             const std::vector<long long>& members,
                             long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long id = nextId("chats");
        Json chat = Json::object();
        chat.set("id", Json(id));
        chat.set("kind", Json(kind));
        chat.set("title", Json(title));
        chat.set("created_by", Json(createdBy));
        chat.set("created_at", Json(isoNow()));
        table("chats").push(chat);

        for (const long long userId : members) {
            Json member = Json::object();
            member.set("chat_id", Json(id));
            member.set("user_id", Json(userId));
            member.set("role", Json(userId == createdBy ? "owner" : "member"));
            member.set("last_read_at", Json(""));
            member.set("is_muted", Json(false));
            table("chat_members").push(member);
        }
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::vector<ChatRecord> listChats(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<long long> chatIds;
        for (const auto& member : table("chat_members").items()) {
            if (member.getInt("user_id") == userId) chatIds.push_back(member.getInt("chat_id"));
        }
        std::vector<ChatRecord> result;
        for (const auto& chat : table("chats").items()) {
            const long long chatId = chat.getInt("id");
            if (std::find(chatIds.begin(), chatIds.end(), chatId) == chatIds.end()) continue;
            ChatRecord record = toChat(chat);
            attachLastMessage(record);
            result.push_back(record);
        }
        std::sort(result.begin(), result.end(), [](const ChatRecord& a, const ChatRecord& b) {
            return a.lastMessageAt > b.lastMessageAt;
        });
        if (static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    std::optional<ChatRecord> findChat(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& chat : table("chats").items()) {
            if (chat.getInt("id") == chatId) {
                ChatRecord record = toChat(chat);
                attachLastMessage(record);
                return record;
            }
        }
        return std::nullopt;
    }

    std::optional<long long> findDirectChat(long long first, long long second) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Личный чат: kind 'direct' или 'agent' и ровно два участника — эти двое.
        for (const auto& chat : table("chats").items()) {
            const std::string kind = chat.getString("kind");
            if (kind != "direct" && kind != "agent") continue;
            const long long chatId = chat.getInt("id");
            bool hasFirst = false;
            bool hasSecond = false;
            std::size_t total = 0;
            for (const auto& member : table("chat_members").items()) {
                if (member.getInt("chat_id") != chatId) continue;
                ++total;
                if (member.getInt("user_id") == first) hasFirst = true;
                if (member.getInt("user_id") == second) hasSecond = true;
            }
            if (hasFirst && hasSecond && total == 2) return chatId;
        }
        return std::nullopt;
    }

    std::vector<MemberRecord> listMembers(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MemberRecord> result;
        for (const auto& member : table("chat_members").items()) {
            if (member.getInt("chat_id") != chatId) continue;
            MemberRecord record;
            record.chatId = chatId;
            record.userId = member.getInt("user_id");
            record.role = member.getString("role", "member");
            record.lastReadAt = member.getString("last_read_at");
            if (const auto user = findUserByIdLocked(record.userId)) {
                record.displayName = user->displayName;
                record.email = user->email;
            }
            result.push_back(record);
        }
        return result;
    }

    bool isMember(long long chatId, long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& member : table("chat_members").items()) {
            if (member.getInt("chat_id") == chatId && member.getInt("user_id") == userId) return true;
        }
        return false;
    }

    DatabaseError markRead(long long chatId, long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& member : mutableTable("chat_members")) {
            if (member.getInt("chat_id") == chatId && member.getInt("user_id") == userId) {
                member.set("last_read_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("участник не найден");
    }

    // --------------------------------------------------------- messages
    DatabaseError insertMessage(const MessageRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long id = nextId("messages");
        Json message = Json::object();
        message.set("id", Json(id));
        message.set("chat_id", Json(record.chatId));
        message.set("sender_id", Json(record.senderId));
        message.set("kind", Json(record.kind));
        message.set("body", Json(record.body));
        message.set("payload", record.payload.isObject() ? record.payload : Json::object());
        message.set("created_at", Json(record.createdAt.empty() ? isoNow() : record.createdAt));
        table("messages").push(message);

        // Триггер messages_touch_chat
        for (auto& chat : mutableTable("chats")) {
            if (chat.getInt("id") == record.chatId) {
                chat.set("last_message_at", message.get("created_at"));
                break;
            }
        }
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::vector<MessageRecord> listMessages(long long chatId, long long beforeId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MessageRecord> result;
        for (const auto& message : table("messages").items()) {
            if (message.getInt("chat_id") != chatId) continue;
            if (beforeId > 0 && message.getInt("id") >= beforeId) continue;
            result.push_back(toMessage(message));
        }
        std::sort(result.begin(), result.end(),
                  [](const MessageRecord& a, const MessageRecord& b) { return a.id > b.id; });
        if (static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        std::reverse(result.begin(), result.end());  // клиенту удобнее читать сверху вниз
        for (auto& record : result) {
            if (const auto user = findUserByIdLocked(record.senderId)) {
                record.senderName = user->displayName;
            }
        }
        return result;
    }

    // ------------------------------------------------------- user_memory
    std::vector<MemoryRecord> listMemory(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MemoryRecord> result;
        for (const auto& entry : table("user_memory").items()) {
            if (entry.getInt("user_id") != userId) continue;
            MemoryRecord record;
            record.id = entry.getInt("id");
            record.userId = userId;
            record.kind = entry.getString("kind", "fact");
            record.text = entry.getString("text");
            record.weight = entry.getDouble("weight", 1.0);
            const Json tagArray = entry.get("tags");
            for (const auto& tag : tagArray.items()) record.tags.push_back(tag.asString());
            record.updatedAt = entry.getString("updated_at");
            result.push_back(record);
        }
        // Тот же порядок, что в PostgreSQL (вес, свежесть, id): при обрезке по limit
        // оба хранилища оставляют одни и те же записи.
        std::sort(result.begin(), result.end(), [](const MemoryRecord& a, const MemoryRecord& b) {
            if (a.weight != b.weight) return a.weight > b.weight;
            if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
            return a.id > b.id;
        });
        if (static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    DatabaseError upsertMemory(long long userId,
                               const std::string& kind,
                               const std::string& text,
                               double weight,
                               const std::vector<std::string>& tags) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json& entries = table("user_memory");
        for (auto& entry : mutableTable("user_memory")) {
            if (entry.getInt("user_id") == userId && entry.getString("text") == text) {
                entry.set("kind", Json(kind));
                entry.set("weight", Json(std::min(5.0, entry.getDouble("weight", 1.0) + 0.25)));
                Json tagArray = Json::array();
                for (const auto& tag : tags) tagArray.push(Json(tag));
                entry.set("tags", tagArray);
                entry.set("updated_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        Json entry = Json::object();
        entry.set("id", Json(nextId("user_memory")));
        entry.set("user_id", Json(userId));
        entry.set("kind", Json(kind));
        entry.set("text", Json(text));
        entry.set("weight", Json(weight));
        Json tagArray = Json::array();
        for (const auto& tag : tags) tagArray.push(Json(tag));
        entry.set("tags", tagArray);
        entry.set("updated_at", Json(isoNow()));
        entries.push(entry);
        flush();
        return DatabaseError::success();
    }

    // -------------------------------------------------- user_preferences
    Json getPreferences(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("user_preferences").items()) {
            if (entry.getInt("user_id") == userId) {
                Json copy = entry;
                copy.erase("user_id");
                return copy;
            }
        }
        return Json::object();
    }

    DatabaseError setPreferences(long long userId, const Json& preferences) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("user_preferences")) {
            if (entry.getInt("user_id") != userId) continue;
            for (const auto& member : preferences.members()) {
                if (member.first == "user_id") continue;
                entry.set(member.first, member.second);
            }
            entry.set("updated_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("настройки не найдены");
    }

    // ------------------------------------------------- tool_permissions
    std::string getToolPermission(long long userId, const std::string& tool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("tool_permissions").items()) {
            if (entry.getInt("user_id") == userId && entry.getString("tool") == tool) {
                return entry.getString("mode");
            }
        }
        return "";  // не задано
    }

    std::vector<ToolPermissionRecord> listToolPermissions(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ToolPermissionRecord> result;
        for (const auto& entry : table("tool_permissions").items()) {
            if (entry.getInt("user_id") != userId) continue;
            result.push_back(toToolPermission(entry));
        }
        std::sort(result.begin(), result.end(),
                  [](const ToolPermissionRecord& a, const ToolPermissionRecord& b) { return a.tool < b.tool; });
        return result;
    }

    DatabaseError setToolPermission(long long userId,
                                    const std::string& tool,
                                    const std::string& mode) override {
        if (mode != "allow" && mode != "ask" && mode != "deny") {
            return DatabaseError::failure("недопустимый режим разрешения");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("tool_permissions")) {
            if (entry.getInt("user_id") == userId && entry.getString("tool") == tool) {
                entry.set("mode", Json(mode));
                entry.set("updated_at", Json(isoNow()));
                flush();
                return DatabaseError::success();
            }
        }
        Json entry = Json::object();
        entry.set("user_id", Json(userId));
        entry.set("tool", Json(tool));
        entry.set("mode", Json(mode));
        entry.set("updated_at", Json(isoNow()));
        table("tool_permissions").push(entry);
        flush();
        return DatabaseError::success();
    }

    // ---------------------------------------------------- pending_actions
    long long createPendingAction(const PendingActionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long id = nextId("pending_actions");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(record.userId));
        entry.set("chat_id", Json(record.chatId));
        entry.set("tool", Json(record.tool));
        entry.set("args", record.args.isObject() ? record.args : Json::object());
        entry.set("summary", Json(record.summary));
        entry.set("status", Json(record.status.empty() ? std::string("pending") : record.status));
        entry.set("result", Json::object());
        entry.set("created_at", Json(isoNow()));
        table("pending_actions").push(entry);
        flush();
        return id;
    }

    std::optional<PendingActionRecord> findPendingAction(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("pending_actions").items()) {
            if (entry.getInt("id") == id) return toPendingAction(entry);
        }
        return std::nullopt;
    }

    bool claimPendingAction(long long id, long long userId, const std::string& status) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("pending_actions")) {
            if (entry.getInt("id") != id) continue;
            if (entry.getInt("user_id") != userId || entry.getString("status") != "pending") return false;
            entry.set("status", Json(status));
            entry.set("resolved_at", Json(isoNow()));
            flush();
            return true;
        }
        return false;
    }

    std::vector<PendingActionRecord> listPendingActions(long long userId,
                                                        const std::string& status,
                                                        int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingActionRecord> result;
        for (const auto& entry : table("pending_actions").items()) {
            if (entry.getInt("user_id") != userId) continue;
            if (!status.empty() && entry.getString("status") != status) continue;
            result.push_back(toPendingAction(entry));
        }
        std::sort(result.begin(), result.end(),
                  [](const PendingActionRecord& a, const PendingActionRecord& b) { return a.id > b.id; });
        if (limit > 0 && static_cast<int>(result.size()) > limit) {
            result.resize(static_cast<std::size_t>(limit));
        }
        return result;
    }

    DatabaseError resolvePendingAction(long long id,
                                       const std::string& status,
                                       const Json& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("pending_actions")) {
            if (entry.getInt("id") != id) continue;
            entry.set("status", Json(status));
            entry.set("result", result.isObject() ? result : Json::object());
            entry.set("resolved_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("отложенное действие не найдено");
    }

    // -------------------------------------------------------------- tasks
    DatabaseError createTask(const TaskRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.title.empty()) return DatabaseError::failure("пустой заголовок задачи");
        const long long id = nextId("tasks");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(record.userId));
        entry.set("chat_id", Json(record.chatId));
        entry.set("title", Json(record.title));
        entry.set("notes", Json(record.notes));
        entry.set("status", Json(record.status.empty() ? std::string("pending") : record.status));
        entry.set("priority", Json(record.priority));
        // Формат как у PostgreSQL (to_char … MS): «…:SSZ» → «…:SS.000Z».
        const auto withMillis = [](const std::string& iso) {
            return iso.size() == 20 && iso.back() == 'Z' ? iso.substr(0, 19) + ".000Z" : iso;
        };
        entry.set("due_at", Json(withMillis(record.dueAt)));
        entry.set("remind_at", Json(withMillis(record.remindAt)));
        entry.set("reminded_at", Json(record.remindedAt));
        entry.set("created_at", Json(isoNow()));
        entry.set("completed_at", Json(record.completedAt));
        table("tasks").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<TaskRecord> findTask(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("tasks").items()) {
            if (entry.getInt("id") == id) return toTask(entry);
        }
        return std::nullopt;
    }

    std::vector<TaskRecord> listTasks(long long userId, const std::string& status, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TaskRecord> result;
        for (const auto& entry : table("tasks").items()) {
            if (entry.getInt("user_id") != userId) continue;
            if (!status.empty() && entry.getString("status") != status) continue;
            result.push_back(toTask(entry));
        }
        std::sort(result.begin(), result.end(),
                  [](const TaskRecord& a, const TaskRecord& b) { return a.id > b.id; });
        if (limit > 0 && static_cast<int>(result.size()) > limit) {
            result.resize(static_cast<std::size_t>(limit));
        }
        return result;
    }

    DatabaseError setTaskStatus(long long id, const std::string& status) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("tasks")) {
            if (entry.getInt("id") != id) continue;
            entry.set("status", Json(status));
            if (status == "done") entry.set("completed_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("задача не найдена");
    }

    DatabaseError deleteTask(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json::Array& entries = mutableTable("tasks");
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->getInt("id") == id) {
                entries.erase(it);
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("задача не найдена");
    }

    std::vector<TaskRecord> listDueTasks(int limit, long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoNow();
        std::vector<TaskRecord> result;
        for (const auto& entry : table("tasks").items()) {
            if (userId != 0 && entry.getInt("user_id") != userId) continue;
            if (entry.getString("status") != "pending") continue;
            if (!entry.getString("reminded_at").empty()) continue;  // уже напомнили
            const std::string remindAt = entry.getString("remind_at");
            if (remindAt.empty() || remindAt > now) continue;        // ещё не время
            result.push_back(toTask(entry));
        }
        std::sort(result.begin(), result.end(),
                  [](const TaskRecord& a, const TaskRecord& b) { return a.remindAt < b.remindAt; });
        if (limit > 0 && static_cast<int>(result.size()) > limit) {
            result.resize(static_cast<std::size_t>(limit));
        }
        return result;
    }

    DatabaseError markTaskReminded(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("tasks")) {
            if (entry.getInt("id") != id) continue;
            if (!entry.getString("reminded_at").empty()) return DatabaseError::failure("напоминание уже отправлено");
            entry.set("reminded_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("задача не найдена");
    }

    // ------------------------------------------- integration_connections
    DatabaseError upsertIntegrationConnection(const IntegrationConnectionRecord& record,
                                              long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("integration_connections")) {
            if (entry.getInt("user_id") == record.userId && entry.getString("provider") == record.provider) {
                entry.set("account", Json(record.account));
                entry.set("scope", Json(record.scope));
                entry.set("token_encrypted", Json(record.tokenEncrypted));
                entry.set("status", Json(record.status.empty() ? std::string("active") : record.status));
                entry.set("last_error", Json(record.lastError));
                flush();
                outId = entry.getInt("id");
                return DatabaseError::success();
            }
        }
        const long long id = nextId("integration_connections");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(record.userId));
        entry.set("provider", Json(record.provider));
        entry.set("account", Json(record.account));
        entry.set("scope", Json(record.scope));
        entry.set("token_encrypted", Json(record.tokenEncrypted));
        entry.set("status", Json(record.status.empty() ? std::string("active") : record.status));
        entry.set("last_error", Json(record.lastError));
        entry.set("created_at", Json(isoNow()));
        entry.set("last_used_at", Json(record.lastUsedAt));
        table("integration_connections").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<IntegrationConnectionRecord> findIntegrationConnection(
        long long userId, const std::string& provider) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("integration_connections").items()) {
            if (entry.getInt("user_id") == userId && entry.getString("provider") == provider) {
                return toIntegration(entry);
            }
        }
        return std::nullopt;
    }

    std::vector<IntegrationConnectionRecord> listIntegrationConnections(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<IntegrationConnectionRecord> result;
        for (const auto& entry : table("integration_connections").items()) {
            if (entry.getInt("user_id") != userId) continue;
            result.push_back(toIntegration(entry));
        }
        std::sort(result.begin(), result.end(),
                  [](const IntegrationConnectionRecord& a, const IntegrationConnectionRecord& b) {
                      return a.provider < b.provider;
                  });
        return result;
    }

    DatabaseError updateIntegrationToken(long long id,
                                         const std::string& tokenEncrypted,
                                         const std::string& status,
                                         const std::string& lastError) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("integration_connections")) {
            if (entry.getInt("id") != id) continue;
            entry.set("token_encrypted", Json(tokenEncrypted));
            entry.set("status", Json(status));
            entry.set("last_error", Json(lastError));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("подключение не найдено");
    }

    DatabaseError touchIntegrationUsed(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("integration_connections")) {
            if (entry.getInt("id") != id) continue;
            entry.set("last_used_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("подключение не найдено");
    }

    DatabaseError deleteIntegrationConnection(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json::Array& entries = mutableTable("integration_connections");
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->getInt("id") == id) {
                entries.erase(it);
                flush();
                return DatabaseError::success();
            }
        }
        return DatabaseError::failure("подключение не найдено");
    }

    // --------------------------------------------- integration_oauth_states
    DatabaseError createOauthState(const OauthStateRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json entry = Json::object();
        entry.set("state", Json(record.state));
        entry.set("user_id", Json(record.userId));
        entry.set("provider", Json(record.provider));
        entry.set("verifier", Json(record.verifier));
        entry.set("redirect_uri", Json(record.redirectUri));
        entry.set("created_at", Json(isoNow()));
        entry.set("expires_at", Json(record.expiresAt));
        entry.set("used_at", Json(record.usedAt));
        table("integration_oauth_states").push(entry);
        flush();
        return DatabaseError::success();
    }

    std::optional<OauthStateRecord> findOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("integration_oauth_states").items()) {
            if (entry.getString("state") == state) return toOauthState(entry);
        }
        return std::nullopt;
    }

    DatabaseError useOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("integration_oauth_states")) {
            if (entry.getString("state") != state) continue;
            if (!entry.getString("used_at").empty()) break;  // уже использован
            entry.set("used_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("состояние OAuth не найдено");
    }

    // -------------------------------------------------- auth_oauth
    DatabaseError linkOauthAccount(const UserOauthAccountRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("user_oauth_accounts").items()) {
            if (item.getString("provider") == record.provider &&
                item.getString("provider_user_id") == record.providerUserId) {
                return DatabaseError::failure("аккаунт уже привязан");
            }
        }
        const long long id = nextId("user_oauth_accounts");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(record.userId));
        entry.set("provider", Json(record.provider));
        entry.set("provider_user_id", Json(record.providerUserId));
        entry.set("email", Json(record.email));
        entry.set("created_at", Json(record.createdAt.empty() ? isoNow() : record.createdAt));
        table("user_oauth_accounts").push(entry);
        flush();
        return DatabaseError::success();
    }

    std::optional<UserOauthAccountRecord> findOauthAccount(const std::string& provider,
                                                           const std::string& providerUserId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("user_oauth_accounts").items()) {
            if (entry.getString("provider") == provider &&
                entry.getString("provider_user_id") == providerUserId) {
                return toUserOauthAccount(entry);
            }
        }
        return std::nullopt;
    }

    std::vector<UserOauthAccountRecord> listUserOauthAccounts(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<UserOauthAccountRecord> out;
        for (const auto& entry : table("user_oauth_accounts").items()) {
            if (entry.getInt("user_id") == userId) {
                out.push_back(toUserOauthAccount(entry));
            }
        }
        return out;
    }

    DatabaseError createAuthOauthState(const AuthOauthStateRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json entry = Json::object();
        entry.set("state", Json(record.state));
        entry.set("provider", Json(record.provider));
        entry.set("verifier", Json(record.verifier));
        entry.set("redirect_uri", Json(record.redirectUri));
        entry.set("created_at", Json(isoNow()));
        entry.set("expires_at", Json(record.expiresAt));
        entry.set("used_at", Json(record.usedAt));
        table("auth_oauth_states").push(entry);
        flush();
        return DatabaseError::success();
    }

    std::optional<AuthOauthStateRecord> findAuthOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("auth_oauth_states").items()) {
            if (entry.getString("state") == state) return toAuthOauthState(entry);
        }
        return std::nullopt;
    }

    DatabaseError useAuthOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("auth_oauth_states")) {
            if (entry.getString("state") != state) continue;
            if (!entry.getString("used_at").empty()) break;
            entry.set("used_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("состояние OAuth не найдено");
    }

    // ------------------------------------------------------------ notifications
    DatabaseError createNotification(const NotificationRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long id = nextId("notifications");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(record.userId));
        entry.set("kind", Json(record.kind));
        entry.set("title", Json(record.title));
        entry.set("body", Json(record.body));
        entry.set("payload", record.payload);
        entry.set("read_at", Json(std::string()));
        entry.set("created_at", Json(isoNow()));
        table("notifications").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::vector<NotificationRecord> listNotifications(long long userId,
                                                      bool unreadOnly,
                                                      int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int ceiling = limit > 0 ? limit : 50;
        std::vector<NotificationRecord> result;
        const Json::Array& entries = table("notifications").items();
        // Свежие первыми (аналог ORDER BY id DESC).
        for (auto it = entries.rbegin();
             it != entries.rend() && static_cast<int>(result.size()) < ceiling; ++it) {
            if (it->getInt("user_id") != userId) continue;
            if (unreadOnly && !it->getString("read_at").empty()) continue;
            result.push_back(toNotification(*it));
        }
        return result;
    }

    long long unreadNotificationCount(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        long long count = 0;
        for (const auto& entry : table("notifications").items()) {
            if (entry.getInt("user_id") != userId) continue;
            if (entry.getString("read_at").empty()) ++count;
        }
        return count;
    }

    DatabaseError markNotificationRead(long long userId, long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("notifications")) {
            if (entry.getInt("id") != id || entry.getInt("user_id") != userId) continue;
            entry.set("read_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("уведомление не найдено");
    }

    DatabaseError markAllNotificationsRead(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool touched = false;
        for (auto& entry : mutableTable("notifications")) {
            if (entry.getInt("user_id") != userId) continue;
            if (!entry.getString("read_at").empty()) continue;
            entry.set("read_at", Json(isoNow()));
            touched = true;
        }
        if (touched) flush();
        return DatabaseError::success();
    }

    // ------------------------------------------------------------- push_devices
    DatabaseError registerPushDevice(long long userId,
                                     const std::string& platform,
                                     const std::string& token,
                                     long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("push_devices")) {
            if (entry.getInt("user_id") == userId && entry.getString("token") == token) {
                entry.set("platform", Json(platform));
                entry.set("enabled", Json(true));
                flush();
                outId = entry.getInt("id");
                return DatabaseError::success();
            }
        }
        const long long id = nextId("push_devices");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("user_id", Json(userId));
        entry.set("platform", Json(platform));
        entry.set("token", Json(token));
        entry.set("enabled", Json(true));
        entry.set("created_at", Json(isoNow()));
        entry.set("last_used_at", Json(std::string()));
        table("push_devices").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::vector<PushDeviceRecord> listPushDevices(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PushDeviceRecord> result;
        for (const auto& entry : table("push_devices").items()) {
            if (entry.getInt("user_id") != userId) continue;
            result.push_back(toPushDevice(entry));
        }
        return result;
    }

    DatabaseError touchPushDevice(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("push_devices")) {
            if (entry.getInt("id") != id) continue;
            entry.set("last_used_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("устройство не найдено");
    }

    DatabaseError deletePushDevice(long long userId, long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json::Array& entries = mutableTable("push_devices");
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->getInt("id") != id || it->getInt("user_id") != userId) continue;
            entries.erase(it);
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("устройство не найдено");
    }

    // ------------------------------------------------------------ admin
    DatabaseError setUserRole(long long userId, const std::string& role) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") != userId) continue;
            item.set("role", Json(role));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("пользователь не найден");
    }

    // --------------------------------------- рост (S1a): состояние, тариф, Telegram
    std::string getState(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("server_state").items()) {
            if (item.getString("key") == key) return item.getString("value");
        }
        return "";
    }

    DatabaseError setState(const std::string& key, const std::string& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("server_state")) {
            if (item.getString("key") != key) continue;
            item.set("value", Json(value));
            item.set("updated_at", Json(isoNow()));
            flush();
            return DatabaseError::success();
        }
        Json row = Json::object();
        row.set("key", Json(key));
        row.set("value", Json(value));
        row.set("updated_at", Json(isoNow()));
        table("server_state").push(row);
        flush();
        return DatabaseError::success();
    }

    DatabaseError setUserPlanDays(long long userId, int days) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") != userId) continue;
            item.set("plan_until",
                     Json(days > 0 ? isoUtc(std::time(nullptr) + static_cast<std::time_t>(days) * 86400) : std::string()));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("пользователь не найден");
    }

    DatabaseError extendUserPlanDays(long long userId, int days) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (days <= 0) return DatabaseError::failure("срок продления — больше 0 дней");
        if (!extendPlanLocked(userId, days)) return DatabaseError::failure("пользователь не найден");
        flush();
        return DatabaseError::success();
    }

    // plan_until = greatest(now, plan_until) + days; вызывается под mutex_.
    bool extendPlanLocked(long long userId, int days) {
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") != userId) continue;
            std::time_t base = std::time(nullptr);
            std::time_t until = 0;
            const std::string current = item.getString("plan_until");
            if (!current.empty() && parseIsoUtc(current, until) && until > base) base = until;
            item.set("plan_until", Json(isoUtc(base + static_cast<std::time_t>(days) * 86400)));
            return true;
        }
        return false;
    }

    DatabaseError setUserTelegram(long long userId, long long chatId, const std::string& username) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (auto& item : mutableTable("users")) {
            if (item.getInt("id") == userId) {
                found = true;
                item.set("telegram_chat_id", Json(chatId));
                item.set("telegram_username", Json(chatId != 0 ? username : std::string()));
            } else if (chatId != 0 && item.getInt("telegram_chat_id") == chatId) {
                item.set("telegram_chat_id", Json(0LL));
                item.set("telegram_username", Json(""));
            }
        }
        if (!found) return DatabaseError::failure("пользователь не найден");
        flush();
        return DatabaseError::success();
    }

    bool linkBookingTelegram(long long bookingId, long long chatId, int maxChats) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const auto& item : table("booking_telegram").items()) {
            if (item.getInt("booking_id") != bookingId) continue;
            if (item.getInt("chat_id") == chatId) return true;
            ++count;
        }
        if (count >= maxChats) return false;
        Json row = Json::object();
        row.set("booking_id", Json(bookingId));
        row.set("chat_id", Json(chatId));
        row.set("linked_at", Json(isoNow()));
        table("booking_telegram").push(row);
        flush();
        return true;
    }

    std::vector<long long> bookingTelegramChats(long long bookingId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<long long> chats;
        for (const auto& item : table("booking_telegram").items()) {
            if (item.getInt("booking_id") == bookingId) chats.push_back(item.getInt("chat_id"));
        }
        return chats;
    }

    DatabaseError unlinkTelegramChat(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chatId == 0) return DatabaseError::success();
        table("booking_telegram");  // ключи создаются до взятия ссылок на таблицы
        table("telegram_guest_chats");
        table("waitlist_entries");
        for (auto& item : mutableTable("waitlist_entries")) {
            if (item.getInt("telegram_chat_id") == chatId) item.set("telegram_chat_id", Json(0LL));
        }
        for (auto& item : mutableTable("users")) {
            if (item.getInt("telegram_chat_id") != chatId) continue;
            item.set("telegram_chat_id", Json(0LL));
            item.set("telegram_username", Json(""));
        }
        auto& links = mutableTable("booking_telegram");
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [chatId](const Json& item) { return item.getInt("chat_id") == chatId; }),
                    links.end());
        auto& guests = mutableTable("telegram_guest_chats");
        guests.erase(std::remove_if(guests.begin(), guests.end(),
                                    [chatId](const Json& item) { return item.getInt("chat_id") == chatId; }),
                     guests.end());
        flush();
        return DatabaseError::success();
    }

    // ------------------------------------------- F7: ИИ-администратор в Telegram
    static TelegramGuestChatRecord toGuestChat(const Json& entry) {
        TelegramGuestChatRecord record;
        record.chatId = entry.getInt("chat_id");
        record.pageId = entry.getInt("page_id");
        record.ownerId = entry.getInt("owner_id");
        record.guestName = entry.getString("guest_name");
        record.username = entry.getString("username");
        if (entry.get("history").isArray()) record.history = entry.get("history");
        record.createdAt = entry.getString("created_at");
        record.updatedAt = entry.getString("updated_at");
        return record;
    }

    DatabaseError saveTelegramGuestChat(const TelegramGuestChatRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.chatId == 0 || record.pageId <= 0) return DatabaseError::failure("чат без страницы");
        Json* target = nullptr;
        for (auto& item : mutableTable("telegram_guest_chats")) {
            if (item.getInt("chat_id") == record.chatId) target = &item;
        }
        if (target == nullptr) {
            Json row = Json::object();
            row.set("chat_id", Json(record.chatId));
            row.set("created_at", Json(isoNow()));
            table("telegram_guest_chats").push(row);
            target = &mutableTable("telegram_guest_chats").back();
        }
        target->set("page_id", Json(record.pageId));
        target->set("owner_id", Json(record.ownerId));
        target->set("guest_name", Json(record.guestName));
        target->set("username", Json(record.username));
        target->set("history", record.history.isArray() ? record.history : Json::array());
        target->set("updated_at", Json(isoNow()));
        flush();
        return DatabaseError::success();
    }

    std::optional<TelegramGuestChatRecord> findTelegramGuestChat(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("telegram_guest_chats").items()) {
            if (item.getInt("chat_id") == chatId) return toGuestChat(item);
        }
        return std::nullopt;
    }

    static TelegramQuestionRecord toQuestion(const Json& entry) {
        TelegramQuestionRecord record;
        record.id = entry.getInt("id");
        record.ownerId = entry.getInt("owner_id");
        record.pageId = entry.getInt("page_id");
        record.chatId = entry.getInt("chat_id");
        record.guestName = entry.getString("guest_name");
        record.username = entry.getString("username");
        record.text = entry.getString("text");
        record.status = entry.getString("status", "open");
        record.answer = entry.getString("answer");
        record.createdAt = entry.getString("created_at");
        record.answeredAt = entry.getString("answered_at");
        return record;
    }

    DatabaseError createTelegramQuestion(const TelegramQuestionRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const long long id = nextId("telegram_questions");
        Json row = Json::object();
        row.set("id", Json(id));
        row.set("owner_id", Json(record.ownerId));
        row.set("page_id", Json(record.pageId));
        row.set("chat_id", Json(record.chatId));
        row.set("guest_name", Json(record.guestName));
        row.set("username", Json(record.username));
        row.set("text", Json(record.text));
        row.set("status", Json(std::string("open")));
        row.set("answer", Json(""));
        row.set("created_at", Json(isoNow()));
        row.set("answered_at", Json(""));
        table("telegram_questions").push(row);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<TelegramQuestionRecord> findTelegramQuestion(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : table("telegram_questions").items()) {
            if (item.getInt("id") == id) return toQuestion(item);
        }
        return std::nullopt;
    }

    std::vector<TelegramQuestionRecord> listTelegramQuestions(long long ownerId, const std::string& status,
                                                              int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TelegramQuestionRecord> result;
        const auto& items = table("telegram_questions").items();
        for (auto it = items.rbegin(); it != items.rend() && static_cast<int>(result.size()) < limit; ++it) {
            if (it->getInt("owner_id") != ownerId) continue;
            if (!status.empty() && it->getString("status") != status) continue;
            result.push_back(toQuestion(*it));
        }
        return result;
    }

    bool answerTelegramQuestion(long long id, const std::string& answer) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : mutableTable("telegram_questions")) {
            if (item.getInt("id") != id) continue;
            item.set("status", Json(std::string("answered")));
            item.set("answer", Json(answer));
            item.set("answered_at", Json(isoNow()));
            flush();
            return true;
        }
        return false;
    }

    std::vector<UserRecord> adminListUsers(const std::string& query,
                                           const std::string& role,
                                           const std::string& status,
                                           int limit,
                                           int offset,
                                           long long& total) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<UserRecord> matched;
        for (const auto& item : table("users").items()) {
            const UserRecord user = toUser(item);
            if (!query.empty() && !contains(user.email, query) && !contains(user.displayName, query)) continue;
            if (!role.empty() && user.role != role) continue;
            if (status == "active" && !user.active) continue;
            if (status == "disabled" && user.active) continue;
            if (status == "unverified" && user.emailVerified) continue;
            matched.push_back(user);
        }
        std::sort(matched.begin(), matched.end(),
                  [](const UserRecord& a, const UserRecord& b) { return a.id > b.id; });
        total = static_cast<long long>(matched.size());
        std::vector<UserRecord> page;
        for (std::size_t i = static_cast<std::size_t>(std::max(0, offset));
             i < matched.size() && static_cast<int>(page.size()) < limit; ++i) {
            page.push_back(matched[i]);
        }
        return page;
    }

    Json adminStats() override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string weekAgo = isoSecondsAgo(7LL * 24 * 3600);
        long long total = 0, verified = 0, disabled = 0, twoFactor = 0, fresh = 0, moderators = 0, admins = 0;
        for (const auto& item : table("users").items()) {
            const UserRecord user = toUser(item);
            ++total;
            if (user.emailVerified) ++verified;
            if (!user.active) ++disabled;
            if (user.createdAt.substr(0, 19) >= weekAgo) ++fresh;
            if (user.role == "moderator") ++moderators;
            if (user.role == "admin") ++admins;
        }
        for (const auto& item : table("two_factor_settings").items()) {
            if (item.getBool("enabled")) ++twoFactor;
        }
        const std::string now = isoSecondsAgo(0);
        long long sessions = 0;
        for (const auto& item : table("sessions").items()) {
            if (sessionLiveJson(item, now)) ++sessions;
        }
        Json stats = Json::object();
        stats.set("users_total", Json(total));
        stats.set("users_verified", Json(verified));
        stats.set("users_disabled", Json(disabled));
        stats.set("users_2fa", Json(twoFactor));
        stats.set("users_new_7d", Json(fresh));
        stats.set("moderators", Json(moderators));
        stats.set("admins", Json(admins));
        stats.set("sessions_active", Json(sessions));
        return stats;
    }

    std::vector<AuditRecord> listAuditPage(long long userId, long long beforeId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditRecord> result;
        const auto& logs = table("audit_logs").items();
        // id растут вместе с порядком вставки: идём с конца — самые новые первыми.
        for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
            if (userId > 0 && it->getInt("user_id") != userId) continue;
            if (beforeId > 0 && it->getInt("id") >= beforeId) continue;
            AuditRecord record;
            record.id = it->getInt("id");
            record.userId = it->getInt("user_id");
            record.kind = it->getString("kind");
            record.detail = it->get("detail");
            record.remoteAddr = it->getString("remote_addr");
            record.createdAt = it->getString("created_at");
            result.push_back(record);
            if (static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    // ------------------------------------------------------- meeting_invites
    DatabaseError createInvite(const InviteRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.tokenHash.empty() || record.topic.empty()) {
            return DatabaseError::failure("приглашение без токена или темы");
        }
        for (const auto& entry : table("meeting_invites").items()) {
            if (entry.getString("token_hash") == record.tokenHash) {
                return DatabaseError::failure("токен приглашения уже существует");
            }
        }
        const long long id = nextId("meeting_invites");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("owner_id", Json(record.ownerId));
        entry.set("token_hash", Json(record.tokenHash));
        entry.set("topic", Json(record.topic));
        entry.set("note", Json(record.note));
        entry.set("duration_minutes", Json(static_cast<long long>(record.durationMinutes)));
        entry.set("slots", record.slots.isArray() ? record.slots : Json::array());
        entry.set("status", Json(std::string("open")));
        entry.set("guest_name", Json(record.guestName));
        entry.set("guest_email", Json(std::string()));
        entry.set("guest_comment", Json(std::string()));
        entry.set("chosen", Json::object());
        entry.set("expires_at", Json(record.expiresAt));
        entry.set("created_at", Json(isoNow()));
        entry.set("responded_at", Json(std::string()));
        table("meeting_invites").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<InviteRecord> findInvite(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("meeting_invites").items()) {
            if (entry.getInt("id") == id) return toInvite(entry);
        }
        return std::nullopt;
    }

    std::optional<InviteRecord> findInviteByToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tokenHash.empty()) return std::nullopt;
        for (const auto& entry : table("meeting_invites").items()) {
            if (entry.getString("token_hash") == tokenHash) return toInvite(entry);
        }
        return std::nullopt;
    }

    std::vector<InviteRecord> listInvites(long long ownerId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<InviteRecord> result;
        const auto& entries = table("meeting_invites").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId) continue;
            result.push_back(toInvite(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    int countOpenInvites(long long ownerId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoNow();
        int count = 0;
        for (const auto& entry : table("meeting_invites").items()) {
            if (entry.getInt("owner_id") == ownerId && entry.getString("status") == "open" &&
                entry.getString("expires_at") > now) {
                ++count;
            }
        }
        return count;
    }

    bool closeInvite(long long id, const std::string& status, const InviteRecord& response) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("meeting_invites")) {
            if (entry.getInt("id") != id) continue;
            if (entry.getString("status") != "open") return false;
            entry.set("status", Json(status));
            if (status != "cancelled") {
                entry.set("guest_name", Json(response.guestName));
                entry.set("guest_email", Json(response.guestEmail));
                entry.set("guest_comment", Json(response.guestComment));
                entry.set("chosen", response.chosen.isObject() ? response.chosen : Json::object());
            }
            entry.set("responded_at", Json(isoNow()));
            flush();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------- ics feed
    // Во встроенной БД — отдельная таблица ics_feeds {user_id, token_hash}
    // (в PostgreSQL — колонка users.ics_token_hash).
    std::string icsTokenHash(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("ics_feeds").items()) {
            if (entry.getInt("user_id") == userId) return entry.getString("token_hash");
        }
        return "";
    }

    DatabaseError setIcsTokenHash(long long userId, const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& users = table("users").items();
        if (std::none_of(users.begin(), users.end(), [&](const Json& user) { return user.getInt("id") == userId; })) {
            return DatabaseError::failure("пользователь не найден");
        }
        Json kept = Json::array();
        for (const auto& entry : table("ics_feeds").items()) {
            if (!tokenHash.empty() && entry.getString("token_hash") == tokenHash && entry.getInt("user_id") != userId) {
                return DatabaseError::failure("токен уже занят");
            }
            if (entry.getInt("user_id") != userId) kept.push(entry);
        }
        if (!tokenHash.empty()) {
            Json entry = Json::object();
            entry.set("user_id", Json(userId));
            entry.set("token_hash", Json(tokenHash));
            entry.set("updated_at", Json(isoNow()));
            kept.push(entry);
        }
        table("ics_feeds") = kept;
        flush();
        return DatabaseError::success();
    }

    long long findUserByIcsToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tokenHash.empty()) return 0;
        for (const auto& entry : table("ics_feeds").items()) {
            if (entry.getString("token_hash") == tokenHash) return entry.getInt("user_id");
        }
        return 0;
    }

private:
    // --------------------------------------------------------- booking_pages
    DatabaseError createBookingPage(const BookingPageRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.slug.empty() || record.title.empty()) {
            return DatabaseError::failure("страница записи без адреса или названия");
        }
        for (const auto& entry : table("booking_pages").items()) {
            if (entry.getString("slug") == record.slug) return DatabaseError::failure("адрес страницы уже занят");
        }
        const long long id = nextId("booking_pages");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("owner_id", Json(record.ownerId));
        entry.set("slug", Json(record.slug));
        writeBookingPage(entry, record);
        entry.set("created_at", Json(isoNow()));
        table("booking_pages").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<BookingPageRecord> findBookingPage(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("booking_pages").items()) {
            if (entry.getInt("id") == id) return toBookingPage(entry);
        }
        return std::nullopt;
    }

    std::optional<BookingPageRecord> findBookingPageBySlug(const std::string& slug) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slug.empty()) return std::nullopt;
        for (const auto& entry : table("booking_pages").items()) {
            if (entry.getString("slug") == slug) return toBookingPage(entry);
        }
        return std::nullopt;
    }

    std::vector<BookingPageRecord> listBookingPages(long long ownerId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingPageRecord> result;
        const auto& entries = table("booking_pages").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") == ownerId && it->getString("status") != "archived") {
                result.push_back(toBookingPage(*it));
            }
        }
        return result;
    }

    DatabaseError updateBookingPage(const BookingPageRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("booking_pages")) {
            if (entry.getInt("id") != record.id) continue;
            writeBookingPage(entry, record);
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("страница записи не найдена");
    }

    static void writeBookingPage(Json& entry, const BookingPageRecord& record) {
        entry.set("title", Json(record.title));
        entry.set("description", Json(record.description));
        entry.set("duration_minutes", Json(static_cast<long long>(record.durationMinutes)));
        entry.set("days_ahead", Json(static_cast<long long>(record.daysAhead)));
        entry.set("max_per_day", Json(static_cast<long long>(record.maxPerDay)));
        entry.set("notice_hours", Json(static_cast<long long>(record.noticeHours)));
        entry.set("max_series", Json(static_cast<long long>(record.maxSeries)));
        entry.set("weekends", Json(record.weekends));
        entry.set("status", Json(record.status));
        entry.set("guest_reminders", Json(record.guestReminders));
        entry.set("hide_branding", Json(record.hideBranding));
        entry.set("price", Json(record.price));
        entry.set("currency", Json(record.currency));
        entry.set("prepay", Json(record.prepay));
        entry.set("deposit", Json(record.deposit));
        entry.set("payment_url", Json(record.paymentUrl));
        entry.set("payment_note", Json(record.paymentNote));
        entry.set("pay_within_minutes", Json(static_cast<long long>(record.payWithinMinutes)));
        entry.set("cancel_hours", Json(static_cast<long long>(record.cancelHours)));
        entry.set("late_cancel", Json(record.lateCancel));
        entry.set("policy_text", Json(record.policyText));
        entry.set("packages", record.packages.isArray() ? record.packages : Json::array());
        entry.set("waitlist", Json(record.waitlist));
        entry.set("waitlist_hold_minutes", Json(static_cast<long long>(record.waitlistHoldMinutes)));
        entry.set("rebook_after_days", Json(static_cast<long long>(record.rebookAfterDays)));
        entry.set("ai_admin", Json(record.aiAdmin));  // F7
        entry.set("updated_at", Json(isoNow()));
    }

    static BookingPageRecord toBookingPage(const Json& entry) {
        BookingPageRecord record;
        record.id = entry.getInt("id");
        record.ownerId = entry.getInt("owner_id");
        record.slug = entry.getString("slug");
        record.title = entry.getString("title");
        record.description = entry.getString("description");
        record.durationMinutes = static_cast<int>(entry.getInt("duration_minutes", 30));
        record.daysAhead = static_cast<int>(entry.getInt("days_ahead", 14));
        record.maxPerDay = static_cast<int>(entry.getInt("max_per_day", 4));
        record.noticeHours = static_cast<int>(entry.getInt("notice_hours", 2));
        record.maxSeries = static_cast<int>(entry.getInt("max_series", 1));
        record.weekends = entry.getBool("weekends", false);
        record.status = entry.getString("status", "active");
        record.createdAt = entry.getString("created_at");
        record.updatedAt = entry.getString("updated_at");
        record.guestReminders = entry.getBool("guest_reminders", true);
        record.hideBranding = entry.getBool("hide_branding", false);
        record.price = entry.getInt("price", 0);
        record.currency = entry.getString("currency", "RUB");
        record.prepay = entry.getString("prepay", "none");
        record.deposit = entry.getInt("deposit", 0);
        record.paymentUrl = entry.getString("payment_url");
        record.paymentNote = entry.getString("payment_note");
        record.payWithinMinutes = static_cast<int>(entry.getInt("pay_within_minutes", 60));
        record.cancelHours = static_cast<int>(entry.getInt("cancel_hours", 0));
        record.lateCancel = entry.getString("late_cancel", "allow");
        record.policyText = entry.getString("policy_text");
        if (entry.get("packages").isArray()) record.packages = entry.get("packages");
        record.waitlist = entry.getBool("waitlist", false);
        record.waitlistHoldMinutes = static_cast<int>(entry.getInt("waitlist_hold_minutes", 60));
        record.rebookAfterDays = static_cast<int>(entry.getInt("rebook_after_days", 0));
        record.aiAdmin = entry.getBool("ai_admin", false);  // F7
        return record;
    }

    // ------------------------------------------------------ booking_reminders
    static BookingReminderRecord toReminder(const Json& entry) {
        BookingReminderRecord record;
        record.id = entry.getInt("id");
        record.bookingId = entry.getInt("booking_id");
        record.ownerId = entry.getInt("owner_id");
        record.occurrenceStart = entry.getString("occurrence_start");
        record.kind = entry.getString("kind");
        record.dueAt = entry.getString("due_at");
        record.sentAt = entry.getString("sent_at");
        return record;
    }

    DatabaseError addBookingReminders(const std::vector<BookingReminderRecord>& reminders) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool added = false;
        for (const auto& reminder : reminders) {
            if (reminder.kind != "24h" && reminder.kind != "2h") return DatabaseError::failure("неизвестный вид напоминания");
            bool exists = false;
            for (const auto& entry : table("booking_reminders").items()) {
                if (entry.getInt("booking_id") == reminder.bookingId &&
                    entry.getString("occurrence_start") == reminder.occurrenceStart &&
                    entry.getString("kind") == reminder.kind) {
                    exists = true;  // UNIQUE (booking_id, occurrence_start, kind) — ON CONFLICT DO NOTHING
                    break;
                }
            }
            if (exists) continue;
            Json entry = Json::object();
            entry.set("id", Json(nextId("booking_reminders")));
            entry.set("booking_id", Json(reminder.bookingId));
            entry.set("owner_id", Json(reminder.ownerId));
            entry.set("occurrence_start", Json(reminder.occurrenceStart));
            entry.set("kind", Json(reminder.kind));
            entry.set("due_at", Json(reminder.dueAt));
            entry.set("sent_at", Json(std::string()));
            table("booking_reminders").push(entry);
            added = true;
        }
        if (added) flush();
        return DatabaseError::success();
    }

    DatabaseError deleteBookingReminders(long long bookingId, const std::string& occurrenceStart) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rows = mutableTable("booking_reminders");
        const auto before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Json& entry) {
                                      return entry.getInt("booking_id") == bookingId &&
                                             entry.getString("sent_at").empty() &&
                                             (occurrenceStart.empty() ||
                                              entry.getString("occurrence_start") == occurrenceStart);
                                  }),
                   rows.end());
        if (rows.size() != before) flush();
        return DatabaseError::success();
    }

    std::vector<BookingReminderRecord> listBookingReminders(long long bookingId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingReminderRecord> result;
        for (const auto& entry : table("booking_reminders").items()) {
            if (entry.getInt("booking_id") == bookingId) result.push_back(toReminder(entry));
        }
        std::stable_sort(result.begin(), result.end(), [](const BookingReminderRecord& a, const BookingReminderRecord& b) {
            return a.dueAt < b.dueAt;
        });
        return result;
    }

    // Эквивалент UPDATE … FOR UPDATE SKIP LOCKED: выбор и отметка — под одним мьютексом.
    std::vector<BookingReminderRecord> claimDueReminders(const std::string& nowIso, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Json*> due;
        for (auto& entry : mutableTable("booking_reminders")) {
            if (entry.getString("sent_at").empty() && entry.getString("due_at") <= nowIso) due.push_back(&entry);
        }
        std::stable_sort(due.begin(), due.end(),
                         [](const Json* a, const Json* b) { return a->getString("due_at") < b->getString("due_at"); });
        if (limit > 0 && static_cast<int>(due.size()) > limit) due.resize(static_cast<std::size_t>(limit));
        std::vector<BookingReminderRecord> claimed;
        const std::string sentAt = isoNow();
        for (Json* entry : due) {
            entry->set("sent_at", Json(sentAt));
            claimed.push_back(toReminder(*entry));
        }
        if (!claimed.empty()) flush();
        return claimed;
    }

    // -------------------------------------------------------------- bookings
    DatabaseError createBooking(const BookingRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.manageHash.empty()) return DatabaseError::failure("запись без токена");
        for (const auto& entry : table("bookings").items()) {
            if (entry.getString("manage_hash") == record.manageHash) {
                return DatabaseError::failure("токен записи уже существует");
            }
        }
        const long long id = nextId("bookings");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("page_id", Json(record.pageId));
        entry.set("owner_id", Json(record.ownerId));
        entry.set("manage_hash", Json(record.manageHash));
        entry.set("guest_name", Json(record.guestName));
        entry.set("guest_email", Json(record.guestEmail));
        entry.set("guest_comment", Json(record.guestComment));
        entry.set("cc_email", Json(record.ccEmail));
        entry.set("package_id", Json(record.packageId));
        entry.set("price", Json(record.price));
        entry.set("currency", Json(record.currency.empty() ? std::string("RUB") : record.currency));
        entry.set("created_at", Json(isoNow()));
        writeBooking(entry, record);
        table("bookings").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<BookingRecord> findBooking(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("bookings").items()) {
            if (entry.getInt("id") == id) return toBooking(entry);
        }
        return std::nullopt;
    }

    std::optional<BookingRecord> findBookingByToken(const std::string& manageHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (manageHash.empty()) return std::nullopt;
        for (const auto& entry : table("bookings").items()) {
            if (entry.getString("manage_hash") == manageHash) return toBooking(entry);
        }
        return std::nullopt;
    }

    std::vector<BookingRecord> listBookings(long long ownerId, long long pageId, bool activeOnly,
                                            int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingRecord> result;
        const auto& entries = table("bookings").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId) continue;
            if (pageId > 0 && it->getInt("page_id") != pageId) continue;
            if (activeOnly && it->getString("status") != "active") continue;
            result.push_back(toBooking(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    std::vector<BookingRecord> listUpcomingBookings(long long ownerId, long long pageId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoUtc(std::time(nullptr));
        std::vector<BookingRecord> result;
        const auto& entries = table("bookings").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId || it->getString("status") != "active") continue;
            if (pageId > 0 && it->getInt("page_id") != pageId) continue;
            const std::string ends = bookingEndsAt(it->get("occurrences"));
            if (ends.empty() || ends <= now) continue;  // ISO UTC одного формата сравнивается как строка
            result.push_back(toBooking(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    DatabaseError updateBooking(const BookingRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("bookings")) {
            if (entry.getInt("id") != record.id) continue;
            writeBooking(entry, record);
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("запись не найдена");
    }

    static void writeBooking(Json& entry, const BookingRecord& record) {
        if (!record.manageHash.empty()) entry.set("manage_hash", Json(record.manageHash));
        if (!record.manageTokenEnc.empty()) entry.set("manage_token_enc", Json(record.manageTokenEnc));
        entry.set("occurrences", record.occurrences.isArray() ? record.occurrences : Json::array());
        entry.set("status", Json(record.status));
        entry.set("cancelled_by", Json(record.cancelledBy));
        entry.set("reschedules", Json(static_cast<long long>(record.reschedules)));
        entry.set("payment", record.payment.isObject() ? record.payment : Json());
        entry.set("updated_at", Json(isoNow()));
    }

    static BookingRecord toBooking(const Json& entry) {
        BookingRecord record;
        record.id = entry.getInt("id");
        record.pageId = entry.getInt("page_id");
        record.ownerId = entry.getInt("owner_id");
        record.manageHash = entry.getString("manage_hash");
        record.guestName = entry.getString("guest_name");
        record.guestEmail = entry.getString("guest_email");
        record.guestComment = entry.getString("guest_comment");
        record.occurrences = entry.get("occurrences").isArray() ? entry.get("occurrences") : Json::array();
        record.status = entry.getString("status", "active");
        record.cancelledBy = entry.getString("cancelled_by");
        record.reschedules = static_cast<int>(entry.getInt("reschedules"));
        record.createdAt = entry.getString("created_at");
        record.updatedAt = entry.getString("updated_at");
        record.manageTokenEnc = entry.getString("manage_token_enc");
        if (entry.get("payment").isObject()) record.payment = entry.get("payment");
        record.ccEmail = entry.getString("cc_email");
        record.packageId = entry.getInt("package_id");
        // Снимок цены (у записей до снимка его проставляет backfillBookingPrices при открытии).
        record.price = entry.getInt("price");
        record.currency = entry.getString("currency", "RUB");
        if (record.currency.empty()) record.currency = "RUB";
        record.rebookSentAt = entry.getString("rebook_sent_at");
        record.rebookStatus = entry.getString("rebook_status");
        return record;
    }

    std::vector<BookingRecord> listBookingsByPackage(long long packageId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingRecord> result;
        if (packageId <= 0) return result;
        for (const auto& entry : table("bookings").items()) {
            if (entry.getInt("package_id") != packageId) continue;
            result.push_back(toBooking(entry));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    // ------------------------------------------------------ package_purchases
    static bool validPackageStatus(const std::string& status) {
        return status == "pending" || status == "active" || status == "used" || status == "expired" ||
               status == "cancelled";
    }

    static void writePackage(Json& entry, const PackagePurchaseRecord& record) {
        entry.set("sessions_total", Json(static_cast<long long>(record.sessionsTotal)));
        entry.set("sessions_used", Json(static_cast<long long>(record.sessionsUsed)));
        entry.set("valid_until", Json(record.validUntil));
        entry.set("status", Json(record.status));
        entry.set("payment", record.payment.isObject() ? record.payment : Json());
        entry.set("updated_at", Json(isoNow()));
    }

    static PackagePurchaseRecord toPackage(const Json& entry) {
        PackagePurchaseRecord record;
        record.id = entry.getInt("id");
        record.ownerId = entry.getInt("owner_id");
        record.pageId = entry.getInt("page_id");
        record.offerId = entry.getInt("offer_id");
        record.title = entry.getString("title");
        record.sessionsTotal = static_cast<int>(entry.getInt("sessions_total"));
        record.sessionsUsed = static_cast<int>(entry.getInt("sessions_used"));
        record.validDays = static_cast<int>(entry.getInt("valid_days", 90));
        record.price = entry.getInt("price");
        record.currency = entry.getString("currency", "RUB");
        record.validUntil = entry.getString("valid_until");
        record.status = entry.getString("status", "pending");
        if (entry.get("payment").isObject()) record.payment = entry.get("payment");
        record.guestName = entry.getString("guest_name");
        record.guestEmail = entry.getString("guest_email");
        record.ccEmail = entry.getString("cc_email");
        record.tokenHash = entry.getString("token_hash");
        record.tokenEnc = entry.getString("token_enc");
        record.createdAt = entry.getString("created_at");
        record.updatedAt = entry.getString("updated_at");
        return record;
    }

    DatabaseError createPackagePurchase(const PackagePurchaseRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.tokenHash.empty() || record.title.empty()) return DatabaseError::failure("пакет без токена или названия");
        if (!validPackageStatus(record.status) || record.sessionsTotal < 1 || record.sessionsUsed < 0) {
            return DatabaseError::failure("некорректный пакет");
        }
        for (const auto& entry : table("package_purchases").items()) {
            if (entry.getString("token_hash") == record.tokenHash) return DatabaseError::failure("токен пакета уже существует");
        }
        const long long id = nextId("package_purchases");
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("owner_id", Json(record.ownerId));
        entry.set("page_id", Json(record.pageId));
        entry.set("offer_id", Json(record.offerId));
        entry.set("title", Json(record.title));
        entry.set("valid_days", Json(static_cast<long long>(record.validDays)));
        entry.set("price", Json(record.price));
        entry.set("currency", Json(record.currency));
        entry.set("guest_name", Json(record.guestName));
        entry.set("guest_email", Json(record.guestEmail));
        entry.set("cc_email", Json(record.ccEmail));
        entry.set("token_hash", Json(record.tokenHash));
        entry.set("token_enc", Json(record.tokenEnc));
        entry.set("created_at", Json(isoNow()));
        writePackage(entry, record);
        table("package_purchases").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<PackagePurchaseRecord> findPackagePurchase(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("package_purchases").items()) {
            if (entry.getInt("id") == id) return toPackage(entry);
        }
        return std::nullopt;
    }

    std::optional<PackagePurchaseRecord> findPackagePurchaseByToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tokenHash.empty()) return std::nullopt;
        for (const auto& entry : table("package_purchases").items()) {
            if (entry.getString("token_hash") == tokenHash) return toPackage(entry);
        }
        return std::nullopt;
    }

    std::vector<PackagePurchaseRecord> listPackagePurchases(long long ownerId, long long pageId,
                                                           const std::string& status, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PackagePurchaseRecord> result;
        const auto& entries = table("package_purchases").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId) continue;
            if (pageId > 0 && it->getInt("page_id") != pageId) continue;
            if (!status.empty() && it->getString("status") != status) continue;
            result.push_back(toPackage(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    DatabaseError updatePackagePurchase(const PackagePurchaseRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validPackageStatus(record.status) || record.sessionsTotal < 1 || record.sessionsUsed < 0) {
            return DatabaseError::failure("некорректный пакет");
        }
        for (auto& entry : mutableTable("package_purchases")) {
            if (entry.getInt("id") != record.id) continue;
            writePackage(entry, record);
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("пакет не найден");
    }

    std::vector<PackagePurchaseRecord> listPendingPackagePurchases(int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PackagePurchaseRecord> result;
        for (const auto& entry : table("package_purchases").items()) {
            if (entry.getString("status") == "pending") result.push_back(toPackage(entry));
        }
        std::stable_sort(result.begin(), result.end(), [](const PackagePurchaseRecord& a, const PackagePurchaseRecord& b) {
            return a.payment.getString("due_at") < b.payment.getString("due_at");
        });
        if (limit > 0 && static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    std::vector<PackagePurchaseRecord> listExpiredPackagePurchases(const std::string& nowIso, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PackagePurchaseRecord> result;
        for (const auto& entry : table("package_purchases").items()) {
            const std::string until = entry.getString("valid_until");
            if (entry.getString("status") != "active" || until.empty() || until > nowIso) continue;
            result.push_back(toPackage(entry));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    std::vector<BookingRecord> listAwaitingPayments(int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingRecord> result;
        for (const auto& entry : table("bookings").items()) {
            const Json payment = entry.get("payment");
            if (payment.isObject() && payment.getString("status") == "pending") result.push_back(toBooking(entry));
        }
        std::stable_sort(result.begin(), result.end(), [](const BookingRecord& a, const BookingRecord& b) {
            return a.payment.getString("due_at") < b.payment.getString("due_at");
        });
        if (limit > 0 && static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    // ------------------------------------------------------ waitlist_entries (F8)
    static bool activeWaitlist(const Json& entry) {
        const std::string status = entry.getString("status");
        return status == "waiting" || status == "offered";
    }

    static void writeWaitlist(Json& entry, const WaitlistEntryRecord& record) {
        entry.set("guest_name", Json(record.guestName));
        entry.set("days", record.days.isArray() ? record.days : Json::array());
        entry.set("part_of_day", Json(record.partOfDay));
        entry.set("comment", Json(record.comment));
        entry.set("status", Json(record.status));
        entry.set("offer_start", Json(record.offerStart));
        entry.set("offer_end", Json(record.offerEnd));
        entry.set("offer_day", Json(record.offerDay));
        entry.set("offer_local", Json(record.offerLocal));
        entry.set("offer_expires_at", Json(record.offerExpiresAt));
        entry.set("offer_status", Json(record.offerStatus));
        entry.set("booking_id", Json(record.bookingId));
        if (!record.queuedAt.empty()) entry.set("queued_at", Json(record.queuedAt));
        if (!record.expiresAt.empty()) entry.set("expires_at", Json(record.expiresAt));
        entry.set("updated_at", Json(isoNow()));
    }

    static WaitlistEntryRecord toWaitlist(const Json& entry) {
        WaitlistEntryRecord record;
        record.id = entry.getInt("id");
        record.ownerId = entry.getInt("owner_id");
        record.pageId = entry.getInt("page_id");
        record.guestName = entry.getString("guest_name");
        record.guestEmail = entry.getString("guest_email");
        record.days = entry.get("days").isArray() ? entry.get("days") : Json::array();
        record.partOfDay = entry.getString("part_of_day", "any");
        record.comment = entry.getString("comment");
        record.status = entry.getString("status", "waiting");
        record.tokenHash = entry.getString("token_hash");
        record.tokenEnc = entry.getString("token_enc");
        record.telegramChatId = entry.getInt("telegram_chat_id");
        record.offerStart = entry.getString("offer_start");
        record.offerEnd = entry.getString("offer_end");
        record.offerDay = entry.getString("offer_day");
        record.offerLocal = entry.getString("offer_local");
        record.offerExpiresAt = entry.getString("offer_expires_at");
        record.offerStatus = entry.getString("offer_status");
        record.bookingId = entry.getInt("booking_id");
        record.queuedAt = entry.getString("queued_at");
        record.expiresAt = entry.getString("expires_at");
        record.createdAt = entry.getString("created_at");
        record.updatedAt = entry.getString("updated_at");
        return record;
    }

    static bool validWaitlistStatus(const std::string& status) {
        return status == "waiting" || status == "offered" || status == "booked" || status == "left" ||
               status == "expired";
    }

    DatabaseError createWaitlistEntry(const WaitlistEntryRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.tokenHash.empty() || record.guestEmail.empty() || record.expiresAt.empty()) {
            return DatabaseError::failure("запись листа без токена, email или срока");
        }
        if (!validWaitlistStatus(record.status)) return DatabaseError::failure("некорректный статус листа ожидания");
        const std::string email = lower(record.guestEmail);
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getString("token_hash") == record.tokenHash) return DatabaseError::failure("токен уже существует");
            // Частичный UNIQUE (page_id, lower(guest_email)) WHERE status IN (waiting, offered).
            if (entry.getInt("page_id") == record.pageId && activeWaitlist(entry) &&
                lower(entry.getString("guest_email")) == email) {
                return DatabaseError::failure("гость уже в листе ожидания этой страницы");
            }
        }
        const long long id = nextId("waitlist_entries");
        const std::string now = isoNow();
        Json entry = Json::object();
        entry.set("id", Json(id));
        entry.set("owner_id", Json(record.ownerId));
        entry.set("page_id", Json(record.pageId));
        entry.set("guest_email", Json(record.guestEmail));
        entry.set("token_hash", Json(record.tokenHash));
        entry.set("token_enc", Json(record.tokenEnc));
        entry.set("telegram_chat_id", Json(0LL));
        entry.set("queued_at", Json(record.queuedAt.empty() ? now : record.queuedAt));
        entry.set("created_at", Json(now));
        writeWaitlist(entry, record);
        table("waitlist_entries").push(entry);
        flush();
        outId = id;
        return DatabaseError::success();
    }

    std::optional<WaitlistEntryRecord> findWaitlistEntry(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getInt("id") == id) return toWaitlist(entry);
        }
        return std::nullopt;
    }

    std::optional<WaitlistEntryRecord> findWaitlistEntryByToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tokenHash.empty()) return std::nullopt;
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getString("token_hash") == tokenHash) return toWaitlist(entry);
        }
        return std::nullopt;
    }

    std::optional<WaitlistEntryRecord> findActiveWaitlistEntry(long long pageId, const std::string& emailLower) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getInt("page_id") == pageId && activeWaitlist(entry) &&
                lower(entry.getString("guest_email")) == emailLower) {
                return toWaitlist(entry);
            }
        }
        return std::nullopt;
    }

    std::vector<WaitlistEntryRecord> listWaitlistEntries(long long ownerId, long long pageId, const std::string& status,
                                                         int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WaitlistEntryRecord> result;
        const auto& entries = table("waitlist_entries").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId) continue;
            if (pageId > 0 && it->getInt("page_id") != pageId) continue;
            if (!status.empty() && it->getString("status") != status) continue;
            result.push_back(toWaitlist(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    std::vector<WaitlistEntryRecord> listWaitingEntries(long long pageId, const std::string& nowIso, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WaitlistEntryRecord> result;
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getInt("page_id") != pageId || entry.getString("status") != "waiting") continue;
            if (entry.getString("expires_at") <= nowIso) continue;
            result.push_back(toWaitlist(entry));
        }
        std::stable_sort(result.begin(), result.end(), [](const WaitlistEntryRecord& a, const WaitlistEntryRecord& b) {
            return a.queuedAt != b.queuedAt ? a.queuedAt < b.queuedAt : a.id < b.id;
        });
        if (limit > 0 && static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    int countActiveWaitlistEntries(long long pageId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getInt("page_id") == pageId && activeWaitlist(entry)) ++count;
        }
        return count;
    }

    DatabaseError updateWaitlistEntry(const WaitlistEntryRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validWaitlistStatus(record.status)) return DatabaseError::failure("некорректный статус листа ожидания");
        const bool active = record.status == "waiting" || record.status == "offered";
        if (active) {
            const std::string email = lower(record.guestEmail);
            for (const auto& entry : table("waitlist_entries").items()) {
                if (entry.getInt("id") != record.id && entry.getInt("page_id") == record.pageId &&
                    activeWaitlist(entry) && lower(entry.getString("guest_email")) == email) {
                    return DatabaseError::failure("гость уже в листе ожидания этой страницы");
                }
            }
        }
        for (auto& entry : mutableTable("waitlist_entries")) {
            if (entry.getInt("id") != record.id) continue;
            writeWaitlist(entry, record);
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("запись листа ожидания не найдена");
    }

    DatabaseError setWaitlistTelegram(long long entryId, long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("waitlist_entries")) {
            if (entry.getInt("id") != entryId) continue;
            entry.set("telegram_chat_id", Json(chatId));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("запись листа ожидания не найдена");
    }

    std::vector<WaitlistEntryRecord> listWaitlistHolds(long long ownerId, const std::string& nowIso) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WaitlistEntryRecord> result;
        for (const auto& entry : table("waitlist_entries").items()) {
            if (entry.getInt("owner_id") != ownerId || entry.getString("status") != "offered") continue;
            if (entry.getString("offer_expires_at") <= nowIso || entry.getString("offer_start").empty()) continue;
            result.push_back(toWaitlist(entry));
        }
        return result;
    }

    std::vector<WaitlistEntryRecord> listDueWaitlist(const std::string& nowIso, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WaitlistEntryRecord> result;
        for (const auto& entry : table("waitlist_entries").items()) {
            const std::string status = entry.getString("status");
            const bool offerDue = status == "offered" && entry.getString("offer_expires_at") <= nowIso;
            const bool entryDue = status == "waiting" && entry.getString("expires_at") <= nowIso;
            if (offerDue || entryDue) result.push_back(toWaitlist(entry));
        }
        std::stable_sort(result.begin(), result.end(), [](const WaitlistEntryRecord& a, const WaitlistEntryRecord& b) {
            const std::string& left = a.status == "offered" ? a.offerExpiresAt : a.expiresAt;
            const std::string& right = b.status == "offered" ? b.offerExpiresAt : b.expiresAt;
            return left < right;
        });
        if (limit > 0 && static_cast<int>(result.size()) > limit) result.resize(static_cast<std::size_t>(limit));
        return result;
    }

    // ------------------------------------------------------- повторный приём (F8)
    // Эквивалент UPDATE … FOR UPDATE SKIP LOCKED: выбор и отметка — под одним мьютексом.
    std::vector<BookingRecord> claimDueRebooks(const std::string& nowIso, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        table("booking_pages");  // ключи создаются до взятия ссылок на таблицы
        table("bookings");
        std::map<long long, int> days;
        for (const auto& page : table("booking_pages").items()) {
            const long long after = page.getInt("rebook_after_days");
            if (after > 0) days[page.getInt("id")] = static_cast<int>(after);
        }
        std::vector<std::pair<std::string, Json*>> due;
        for (auto& entry : mutableTable("bookings")) {
            if (entry.getString("status", "active") != "active" || !entry.getString("rebook_sent_at").empty()) continue;
            const auto page = days.find(entry.getInt("page_id"));
            if (page == days.end()) continue;
            const std::string ends = bookingEndsAt(entry.get("occurrences"));
            std::time_t endsAt = 0;
            if (ends.empty() || !parseIsoUtc(ends, endsAt)) continue;
            if (isoUtc(endsAt + static_cast<std::time_t>(page->second) * 86400) > nowIso) continue;
            due.emplace_back(ends, &entry);
        }
        std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (limit > 0 && static_cast<int>(due.size()) > limit) due.resize(static_cast<std::size_t>(limit));
        std::vector<BookingRecord> claimed;
        for (auto& [ends, entry] : due) {
            (void)ends;
            entry->set("rebook_sent_at", Json(nowIso));
            entry->set("rebook_status", Json(std::string("claimed")));
            claimed.push_back(toBooking(*entry));
        }
        if (!claimed.empty()) flush();
        return claimed;
    }

    DatabaseError setRebookStatus(long long bookingId, const std::string& status) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("bookings")) {
            if (entry.getInt("id") != bookingId) continue;
            entry.set("rebook_status", Json(status));
            flush();
            return DatabaseError::success();
        }
        return DatabaseError::failure("запись не найдена");
    }

    DatabaseError cancelPendingRebooks(long long pageId, const std::string& nowIso) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;
        for (auto& entry : mutableTable("bookings")) {
            if (entry.getInt("page_id") != pageId || entry.getString("status", "active") != "active" ||
                !entry.getString("rebook_sent_at").empty()) {
                continue;
            }
            const std::string ends = bookingEndsAt(entry.get("occurrences"));
            if (ends.empty() || ends > nowIso) continue;
            entry.set("rebook_sent_at", Json(nowIso));
            entry.set("rebook_status", Json(std::string("cancelled")));
            changed = true;
        }
        if (changed) flush();
        return DatabaseError::success();
    }

    bool hasLaterBooking(long long pageId, const std::string& emailLower, long long exceptBookingId,
                         const std::string& endsAtIso) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (emailLower.empty()) return false;
        for (const auto& entry : table("bookings").items()) {
            if (entry.getInt("page_id") != pageId || entry.getInt("id") == exceptBookingId) continue;
            if (entry.getString("status", "active") != "active" || lower(entry.getString("guest_email")) != emailLower) {
                continue;
            }
            const std::string ends = bookingEndsAt(entry.get("occurrences"));
            if (!ends.empty() && ends > endsAtIso) return true;
        }
        return false;
    }

    // ----------------------------------------------------------- чеки (F10)
    static bool receiptPending(const Json& payment) {
        return payment.isObject() && payment.getBool("npd") && payment.getString("status") == "paid" &&
               payment.getString("receipt_at").empty();
    }

    std::vector<BookingRecord> listReceiptPendingBookings(long long ownerId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BookingRecord> result;
        const auto& entries = table("bookings").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId || !receiptPending(it->get("payment"))) continue;
            result.push_back(toBooking(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    std::vector<PackagePurchaseRecord> listReceiptPendingPackages(long long ownerId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PackagePurchaseRecord> result;
        const auto& entries = table("package_purchases").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("owner_id") != ownerId || !receiptPending(it->get("payment"))) continue;
            result.push_back(toPackage(*it));
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    // ------------------------------------------------------- payment_accounts
    std::optional<PaymentAccountRecord> findPaymentAccount(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("payment_accounts").items()) {
            if (entry.getInt("user_id") != userId) continue;
            PaymentAccountRecord record;
            record.userId = userId;
            record.provider = entry.getString("provider", "yookassa");
            record.shopId = entry.getString("shop_id");
            record.secretEnc = entry.getString("secret_enc");
            record.connectedAt = entry.getString("connected_at");
            return record;
        }
        return std::nullopt;
    }

    DatabaseError setPaymentAccount(const PaymentAccountRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.provider != "yookassa" || record.shopId.empty() || record.secretEnc.empty()) {
            return DatabaseError::failure("некорректное подключение оплаты");
        }
        auto& rows = mutableTable("payment_accounts");
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Json& entry) { return entry.getInt("user_id") == record.userId; }),
                   rows.end());
        Json entry = Json::object();
        entry.set("user_id", Json(record.userId));
        entry.set("provider", Json(record.provider));
        entry.set("shop_id", Json(record.shopId));
        entry.set("secret_enc", Json(record.secretEnc));
        entry.set("connected_at", Json(isoNow()));
        rows.push_back(entry);
        flush();
        return DatabaseError::success();
    }

    DatabaseError deletePaymentAccount(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& rows = mutableTable("payment_accounts");
        const auto before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Json& entry) { return entry.getInt("user_id") == userId; }),
                   rows.end());
        if (rows.size() != before) flush();
        return DatabaseError::success();
    }

    // ------------------------------------------------------------- referrals
    std::string referralCode(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : table("referral_codes").items()) {
            if (entry.getInt("user_id") == userId) return entry.getString("code");
        }
        return "";
    }

    bool setReferralCode(long long userId, const std::string& code) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (code.empty()) return false;
        for (const auto& entry : table("referral_codes").items()) {
            if (entry.getInt("user_id") == userId || entry.getString("code") == code) return false;
        }
        Json entry = Json::object();
        entry.set("user_id", Json(userId));
        entry.set("code", Json(code));
        entry.set("created_at", Json(isoNow()));
        table("referral_codes").push(entry);
        flush();
        return true;
    }

    long long findReferrer(const std::string& code) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (code.empty()) return 0;
        for (const auto& entry : table("referral_codes").items()) {
            if (entry.getString("code") == code) return entry.getInt("user_id");
        }
        return 0;
    }

    DatabaseError createReferral(long long referrerId, long long referredId, const std::string& source) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (referrerId == referredId) return DatabaseError::failure("нельзя пригласить себя");
        for (const auto& entry : table("referrals").items()) {
            if (entry.getInt("referred_id") == referredId) return DatabaseError::failure("уже приглашён");
        }
        Json entry = Json::object();
        entry.set("id", Json(nextId("referrals")));
        entry.set("referrer_id", Json(referrerId));
        entry.set("referred_id", Json(referredId));
        entry.set("source", Json(source));
        entry.set("created_at", Json(isoNow()));
        entry.set("verified_at", Json(std::string()));
        table("referrals").push(entry);
        flush();
        return DatabaseError::success();
    }

    long long markReferralVerified(long long referredId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : mutableTable("referrals")) {
            if (entry.getInt("referred_id") != referredId) continue;
            if (!entry.getString("verified_at").empty()) return 0;
            entry.set("verified_at", Json(isoNow()));
            flush();
            return entry.getInt("referrer_id");
        }
        return 0;
    }

    long long rewardReferral(long long referredId, int days, int cap, bool& referrerRewarded) override {
        std::lock_guard<std::mutex> lock(mutex_);
        referrerRewarded = false;
        table("users");  // ключи — до взятия ссылок на таблицы
        Json* row = nullptr;
        for (auto& entry : mutableTable("referrals")) {
            if (entry.getInt("referred_id") == referredId) {
                row = &entry;
                break;
            }
        }
        if (!row || row->getString("verified_at").empty() || !row->getString("rewarded_at").empty() || days <= 0) {
            return 0;
        }
        const long long referrerId = row->getInt("referrer_id");
        int rewarded = 0;
        for (const auto& entry : table("referrals").items()) {
            if (entry.getInt("referrer_id") == referrerId && !entry.getString("rewarded_at").empty()) ++rewarded;
        }
        row->set("rewarded_at", Json(isoNow()));
        extendPlanLocked(referredId, days);
        if (rewarded < cap) {
            extendPlanLocked(referrerId, days);
            referrerRewarded = true;
        }
        flush();
        return referrerId;
    }

    std::vector<ReferralRecord> listReferrals(long long referrerId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ReferralRecord> result;
        const auto& entries = table("referrals").items();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->getInt("referrer_id") != referrerId) continue;
            ReferralRecord record;
            record.referrerId = referrerId;
            record.referredId = it->getInt("referred_id");
            record.source = it->getString("source");
            record.createdAt = it->getString("created_at");
            record.verifiedAt = it->getString("verified_at");
            record.rewardedAt = it->getString("rewarded_at");
            for (const auto& user : table("users").items()) {
                if (user.getInt("id") == record.referredId) {
                    record.displayName = user.getString("display_name");
                    break;
                }
            }
            result.push_back(record);
            if (limit > 0 && static_cast<int>(result.size()) >= limit) break;
        }
        return result;
    }

    InviteRecord toInvite(const Json& entry) const {
        InviteRecord record;
        record.id = entry.getInt("id");
        record.ownerId = entry.getInt("owner_id");
        record.tokenHash = entry.getString("token_hash");
        record.topic = entry.getString("topic");
        record.note = entry.getString("note");
        record.durationMinutes = static_cast<int>(entry.getInt("duration_minutes", 60));
        record.slots = entry.get("slots").isArray() ? entry.get("slots") : Json::array();
        record.status = entry.getString("status", "open");
        record.guestName = entry.getString("guest_name");
        record.guestEmail = entry.getString("guest_email");
        record.guestComment = entry.getString("guest_comment");
        record.chosen = entry.get("chosen").isObject() ? entry.get("chosen") : Json::object();
        record.expiresAt = entry.getString("expires_at");
        record.createdAt = entry.getString("created_at");
        record.respondedAt = entry.getString("responded_at");
        return record;
    }

    NotificationRecord toNotification(const Json& entry) const {
        NotificationRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.kind = entry.getString("kind");
        record.title = entry.getString("title");
        record.body = entry.getString("body");
        record.payload = entry.get("payload");
        record.readAt = entry.getString("read_at");
        record.createdAt = entry.getString("created_at");
        return record;
    }

    PushDeviceRecord toPushDevice(const Json& entry) const {
        PushDeviceRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.platform = entry.getString("platform", "apns");
        record.token = entry.getString("token");
        record.enabled = entry.getBool("enabled", true);
        record.createdAt = entry.getString("created_at");
        record.lastUsedAt = entry.getString("last_used_at");
        return record;
    }

    IntegrationConnectionRecord toIntegration(const Json& entry) const {
        IntegrationConnectionRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.provider = entry.getString("provider");
        record.account = entry.getString("account");
        record.scope = entry.getString("scope");
        record.tokenEncrypted = entry.getString("token_encrypted");
        record.status = entry.getString("status", "active");
        record.lastError = entry.getString("last_error");
        record.createdAt = entry.getString("created_at");
        record.lastUsedAt = entry.getString("last_used_at");
        return record;
    }

    OauthStateRecord toOauthState(const Json& entry) const {
        OauthStateRecord record;
        record.state = entry.getString("state");
        record.userId = entry.getInt("user_id");
        record.provider = entry.getString("provider");
        record.verifier = entry.getString("verifier");
        record.redirectUri = entry.getString("redirect_uri");
        record.createdAt = entry.getString("created_at");
        record.expiresAt = entry.getString("expires_at");
        record.usedAt = entry.getString("used_at");
        return record;
    }

    UserOauthAccountRecord toUserOauthAccount(const Json& entry) const {
        UserOauthAccountRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.provider = entry.getString("provider");
        record.providerUserId = entry.getString("provider_user_id");
        record.email = entry.getString("email");
        record.createdAt = entry.getString("created_at");
        return record;
    }

    AuthOauthStateRecord toAuthOauthState(const Json& entry) const {
        AuthOauthStateRecord record;
        record.state = entry.getString("state");
        record.provider = entry.getString("provider");
        record.verifier = entry.getString("verifier");
        record.redirectUri = entry.getString("redirect_uri");
        record.createdAt = entry.getString("created_at");
        record.expiresAt = entry.getString("expires_at");
        record.usedAt = entry.getString("used_at");
        return record;
    }

    TaskRecord toTask(const Json& entry) const {
        TaskRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.chatId = entry.getInt("chat_id");
        record.title = entry.getString("title");
        record.notes = entry.getString("notes");
        record.status = entry.getString("status", "pending");
        record.priority = static_cast<int>(entry.getInt("priority"));
        record.dueAt = entry.getString("due_at");
        record.remindAt = entry.getString("remind_at");
        record.remindedAt = entry.getString("reminded_at");
        record.createdAt = entry.getString("created_at");
        record.completedAt = entry.getString("completed_at");
        return record;
    }

    ToolPermissionRecord toToolPermission(const Json& entry) const {
        ToolPermissionRecord record;
        record.userId = entry.getInt("user_id");
        record.tool = entry.getString("tool");
        record.mode = entry.getString("mode", "ask");
        record.updatedAt = entry.getString("updated_at");
        return record;
    }

    PendingActionRecord toPendingAction(const Json& entry) const {
        PendingActionRecord record;
        record.id = entry.getInt("id");
        record.userId = entry.getInt("user_id");
        record.chatId = entry.getInt("chat_id");
        record.tool = entry.getString("tool");
        record.args = entry.get("args").isObject() ? entry.get("args") : Json::object();
        record.summary = entry.getString("summary");
        record.status = entry.getString("status", "pending");
        record.result = entry.get("result").isObject() ? entry.get("result") : Json::object();
        record.createdAt = entry.getString("created_at");
        record.resolvedAt = entry.getString("resolved_at");
        return record;
    }

    // Версия без блокировки: вызывается из методов, которые уже держат mutex_.
    std::optional<UserRecord> findUserByIdLocked(long long id) const {
        const Json* users = state_.find("users");
        if (users == nullptr) return std::nullopt;
        for (const auto& item : users->items()) {
            if (item.getInt("id") == id) return toUser(item);
        }
        return std::nullopt;
    }

    // Записи из файла до снимка цены (нет поля price) получают цену своей страницы,
    // как при миграции 2026_10_17_booking_price.sql; запись по пакету — 0.
    void backfillBookingPrices() {
        if (!state_.get("bookings").isArray() || !state_.get("booking_pages").isArray()) return;
        const Json::Array& pages = state_["booking_pages"].items();
        for (Json& entry : state_["bookings"].items()) {
            if (entry.contains("price")) continue;
            long long price = 0;
            std::string currency = "RUB";
            if (entry.getInt("package_id") == 0) {
                for (const Json& page : pages) {
                    if (page.getInt("id") != entry.getInt("page_id")) continue;
                    price = page.getInt("price");
                    currency = page.getString("currency", "RUB");
                    break;
                }
            }
            entry.set("price", Json(price));
            entry.set("currency", Json(currency.empty() ? std::string("RUB") : currency));
        }
    }

    Json& table(const std::string& name) {
        if (!state_.contains(name)) state_.set(name, Json::array());
        return state_[name];
    }

    Json::Array& mutableTable(const std::string& name) { return table(name).items(); }

    // id не переиспользуются: после удаления последней строки max(id)+1 вернул бы
    // тот же id, и старые ссылки на него (task_id в записи) указали бы на чужую строку.
    long long nextId(const std::string& name) {
        long long maxId = 0;
        for (const auto& item : table(name).items()) maxId = std::max(maxId, item.getInt("id"));
        Json& sequences = state_["sequences"];  // создан при открытии (open)
        const long long next = std::max(maxId, sequences.getInt(name)) + 1;
        sequences.set(name, Json(next));
        return next;
    }

    void flush() {
        if (path_.empty()) return;
        // Пишем во временный файл и переименовываем: иначе обрыв процесса
        // посреди записи оставляет усечённый JSON, и база не загружается вовсе.
        // Права 0600 с момента создания: в файле хэши паролей и токенов.
        const std::string temporary = path_ + ".tmp";
        const std::string data = state_.dump(2);
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            SELFVOY_LOG(log::Level::Warn, "db") << "не удалось сохранить " << temporary << ": " << std::strerror(errno);
            return;
        }
        ::fchmod(fd, 0600);  // файл мог остаться от старой версии с другими правами
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t chunk = ::write(fd, data.data() + written, data.size() - written);
            if (chunk < 0) {
                if (errno == EINTR) continue;
                SELFVOY_LOG(log::Level::Warn, "db") << "ошибка записи " << temporary << ": " << std::strerror(errno);
                ::close(fd);
                return;
            }
            written += static_cast<std::size_t>(chunk);
        }
        ::close(fd);
        if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
            SELFVOY_LOG(log::Level::Warn, "db")
                << "не удалось заменить " << path_ << ": " << std::strerror(errno);
        }
    }

    static UserRecord toUser(const Json& json) {
        UserRecord record;
        record.id = json.getInt("id");
        record.email = json.getString("email");
        record.passwordHash = json.getString("password_hash");
        record.displayName = json.getString("display_name");
        record.avatarUrl = json.getString("avatar_url");
        record.bio = json.getString("bio");
        record.timezone = json.getString("timezone", "Europe/Moscow");
        record.createdAt = json.getString("created_at");
        record.lastSeenAt = json.getString("last_seen_at");
        record.active = json.getBool("is_active", true);
        // Старые записи без поля считаем подтверждёнными (миграция схемы).
        record.emailVerified = json.getBool("email_verified", true);
        record.role = json.getString("role", "user");
        if (record.role.empty()) record.role = "user";
        record.planUntil = json.getString("plan_until");
        record.telegramChatId = json.getInt("telegram_chat_id");
        record.telegramUsername = json.getString("telegram_username");
        return record;
    }

    static SessionRecord toSession(const Json& json) {
        SessionRecord record;
        record.id = json.getString("id");
        record.userId = json.getInt("user_id");
        record.jwtId = json.getString("jwt_id");
        record.device = json.getString("device");
        record.remoteAddr = json.getString("remote_addr");
        record.createdAt = json.getString("created_at");
        record.expiresAt = json.getString("expires_at");
        record.revokedAt = json.getString("revoked_at");
        return record;
    }

    static AuthTokenRecord toAuthToken(const Json& json) {
        AuthTokenRecord record;
        record.id = json.getString("id");
        record.userId = json.getInt("user_id");
        record.purpose = json.getString("purpose");
        record.tokenHash = json.getString("token_hash");
        record.createdAt = json.getString("created_at");
        record.expiresAt = json.getString("expires_at");
        record.usedAt = json.getString("used_at");
        return record;
    }

    static RefreshTokenRecord toRefreshToken(const Json& json) {
        RefreshTokenRecord record;
        record.id = json.getString("id");
        record.userId = json.getInt("user_id");
        record.familyId = json.getString("family_id");
        record.tokenHash = json.getString("token_hash");
        record.device = json.getString("device");
        record.remoteAddr = json.getString("remote_addr");
        record.createdAt = json.getString("created_at");
        record.expiresAt = json.getString("expires_at");
        record.revokedAt = json.getString("revoked_at");
        record.replacedBy = json.getString("replaced_by");
        return record;
    }

    static TwoFactorRecord toTwoFactor(const Json& json) {
        TwoFactorRecord record;
        record.userId = json.getInt("user_id");
        record.secretEncrypted = json.getString("secret_encrypted");
        record.enabled = json.getBool("enabled", false);
        record.lastUsedCounter = json.getInt("last_used_counter");
        record.createdAt = json.getString("created_at");
        record.enabledAt = json.getString("enabled_at");
        record.updatedAt = json.getString("updated_at");
        return record;
    }

    static RecoveryCodeRecord toRecoveryCode(const Json& json) {
        RecoveryCodeRecord record;
        record.id = json.getString("id");
        record.userId = json.getInt("user_id");
        record.codeHash = json.getString("code_hash");
        record.createdAt = json.getString("created_at");
        record.usedAt = json.getString("used_at");
        return record;
    }

    static TrustedDeviceRecord toTrustedDevice(const Json& json) {
        TrustedDeviceRecord record;
        record.id = json.getString("id");
        record.userId = json.getInt("user_id");
        record.deviceHash = json.getString("device_hash");
        record.device = json.getString("device");
        record.remoteAddr = json.getString("remote_addr");
        record.createdAt = json.getString("created_at");
        record.expiresAt = json.getString("expires_at");
        record.lastUsedAt = json.getString("last_used_at");
        record.revokedAt = json.getString("revoked_at");
        return record;
    }


    ChatRecord toChat(const Json& json) const {
        ChatRecord record;
        record.id = json.getInt("id");
        record.kind = json.getString("kind", "direct");
        record.title = json.getString("title");
        record.createdBy = json.getInt("created_by");
        record.createdAt = json.getString("created_at");
        record.lastMessageAt = json.getString("last_message_at");
        return record;
    }

    void attachLastMessage(ChatRecord& record) const {
        const Json* messages = state_.find("messages");
        if (!messages) return;
        const Json* best = nullptr;
        for (const auto& message : messages->items()) {
            if (message.getInt("chat_id") != record.id) continue;
            if (best == nullptr || message.getInt("id") > best->getInt("id")) best = &message;
        }
        if (best == nullptr) return;
        record.lastMessageBody = best->getString("body");
        record.lastMessageAt = best->getString("created_at");
        record.lastMessageSenderId = best->getInt("sender_id");
        const Json* users = state_.find("users");
        if (users) {
            for (const auto& user : users->items()) {
                if (user.getInt("id") == record.lastMessageSenderId) {
                    record.lastMessageSender = user.getString("display_name");
                    break;
                }
            }
        }
    }

    MessageRecord toMessage(const Json& json) const {
        MessageRecord record;
        record.id = json.getInt("id");
        record.chatId = json.getInt("chat_id");
        record.senderId = json.getInt("sender_id");
        record.kind = json.getString("kind", "text");
        record.body = json.getString("body");
        record.payload = json.get("payload").isObject() ? json.get("payload") : Json::object();
        record.createdAt = json.getString("created_at");
        return record;
    }

    const std::string path_;
    mutable std::mutex mutex_;
    Json state_ = Json::object();
};

}  // namespace

std::unique_ptr<IDatabase> makeEmbeddedDatabase(const std::string& path) {
    return std::make_unique<EmbeddedDatabase>(path);
}

}  // namespace selfvoy
