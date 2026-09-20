// aura/database_embedded.cpp — файловое хранилище для режима без PostgreSQL.
//
// Повторяет семантику схемы (те же таблицы, те же ограничения) и используется:
//   * в юнит-тестах сервера;
//   * в демо-режиме (AURA_DATABASE_URL не задан).
// Данные держатся в памяти и сбрасываются в JSON-файл после каждой записи.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include "aura/idatabase.h"
#include "aura/json.h"
#include "aura/log.h"

namespace aura {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

class EmbeddedDatabase : public IDatabase {
public:
    explicit EmbeddedDatabase(std::string path) : path_(std::move(path)) {}

    std::string name() const override { return "embedded:" + path_; }

    bool connect(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(path_);
        if (!file) {
            state_ = Json::object();
            return true;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const Json parsed = Json::parse(buffer.str(), &error);
        if (parsed.isNull()) {
            AURA_LOG(log::Level::Warn, "db") << "файл " << path_ << " повреждён, начинаем с чистой базы";
            state_ = Json::object();
            error.clear();
            return true;
        }
        state_ = parsed;
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
        Json notifications = Json::object();
        notifications.set("push", Json(true));
        notifications.set("email", Json(false));
        preferences.set("notifications", notifications);
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
            if (query.empty() || contains(item.getString("email"), query) ||
                contains(item.getString("display_name"), query)) {
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

    // --------------------------------------------------------- sessions
    DatabaseError createSession(const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Json session = Json::object();
        session.set("id", Json(record.id));
        session.set("user_id", Json(record.userId));
        session.set("jwt_id", Json(record.jwtId));
        session.set("device", Json(record.device));
        session.set("remote_addr", Json(record.remoteAddr));
        session.set("created_at", Json(record.createdAt));
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
        for (auto& item : mutableTable("refresh_tokens")) {
            if (item.getString("family_id") != familyId) continue;
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
        std::sort(result.begin(), result.end(),
                  [](const MemoryRecord& a, const MemoryRecord& b) { return a.weight > b.weight; });
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
        entry.set("due_at", Json(record.dueAt));
        entry.set("remind_at", Json(record.remindAt));
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

    std::vector<TaskRecord> listDueTasks(int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now = isoNow();
        std::vector<TaskRecord> result;
        for (const auto& entry : table("tasks").items()) {
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

private:
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

    Json& table(const std::string& name) {
        if (!state_.contains(name)) state_.set(name, Json::array());
        return state_[name];
    }

    Json::Array& mutableTable(const std::string& name) { return table(name).items(); }

    long long nextId(const std::string& name) {
        long long maxId = 0;
        for (const auto& item : table(name).items()) maxId = std::max(maxId, item.getInt("id"));
        return maxId + 1;
    }

    void flush() {
        if (path_.empty()) return;
        // Пишем во временный файл и переименовываем: иначе обрыв процесса
        // посреди записи оставляет усечённый JSON, и база не загружается вовсе.
        const std::string temporary = path_ + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                AURA_LOG(log::Level::Warn, "db") << "не удалось сохранить " << temporary;
                return;
            }
            file << state_.dump(2);
            file.flush();
            if (!file.good()) {
                AURA_LOG(log::Level::Warn, "db") << "ошибка записи " << temporary;
                return;
            }
        }
        if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
            AURA_LOG(log::Level::Warn, "db")
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
        record.timezone = json.getString("timezone", "Europe/Moscow");
        record.createdAt = json.getString("created_at");
        record.lastSeenAt = json.getString("last_seen_at");
        record.active = json.getBool("is_active", true);
        // Старые записи без поля считаем подтверждёнными (миграция схемы).
        record.emailVerified = json.getBool("email_verified", true);
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

}  // namespace aura
