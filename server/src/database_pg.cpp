// aura/database_pg.cpp — реализация IDatabase поверх libpq (PostgreSQL).
//
// Компилируется только при AURA_WITH_LIBPQ (CMake находит libpq автоматически).
// Без него makePostgresDatabase() возвращает nullptr, и DatabaseManager
// переключается на встроенное хранилище с предупреждением в лог.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

#include "aura/idatabase.h"
#include "aura/json.h"
#include "aura/log.h"

#ifdef AURA_WITH_LIBPQ
#include <libpq-fe.h>
#endif

namespace aura {

#ifdef AURA_WITH_LIBPQ

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Литерал массива PostgreSQL: {"a","b"}
std::string arrayLiteral(const std::vector<std::string>& values) {
    std::string out = "{";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += "\"";
        for (const char ch : values[i]) {
            if (ch == '"' || ch == '\\') out += '\\';
            out += ch;
        }
        out += "\"";
    }
    out += "}";
    return out;
}

std::vector<std::string> parseArray(const char* raw) {
    std::vector<std::string> result;
    if (raw == nullptr) return result;
    std::string text = raw;
    if (text.size() >= 2 && text.front() == '{' && text.back() == '}') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty()) return result;

    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\' && i + 1 < text.size()) {
            current.push_back(text[++i]);
            continue;
        }
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (ch == ',' && !quoted) {
            result.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    result.push_back(current);
    return result;
}

class PostgresDatabase : public IDatabase {
public:
    explicit PostgresDatabase(std::string url) : url_(std::move(url)) {}

    ~PostgresDatabase() override {
        if (connection_ != nullptr) PQfinish(connection_);
    }

    std::string name() const override { return "postgresql"; }

    bool connect(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = PQconnectdb(url_.c_str());
        if (PQstatus(connection_) != CONNECTION_OK) {
            error = std::string("PostgreSQL: ") + PQerrorMessage(connection_);
            PQfinish(connection_);
            connection_ = nullptr;
            return false;
        }
        const char* parameters = "SET TIME ZONE 'UTC'";
        PGresult* result = PQexec(connection_, parameters);
        const bool ok = PQresultStatus(result) == PGRES_COMMAND_OK;
        PQclear(result);
        return ok;
    }

    bool healthy() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ == nullptr) return false;
        return PQstatus(connection_) == CONNECTION_OK;
    }

    // ------------------------------------------------------------ users
    DatabaseError createUser(const std::string& email,
                             const std::string& passwordHash,
                             const std::string& displayName,
                             long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        const std::vector<std::string> args{lower(email), passwordHash, displayName};
        if (!execute(
                "INSERT INTO users (email, password_hash, display_name) "
                "VALUES (LOWER($1), $2, $3) RETURNING id",
                args, &result)) {
            return DatabaseError::failure(lastError_);
        }
        outId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<UserRecord> findUserByEmail(const std::string& email) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified "
                     "FROM users WHERE lower(email) = LOWER($1) LIMIT 1",
                     {lower(email)}, &result)) {
            return std::nullopt;
        }
        std::optional<UserRecord> record;
        if (PQntuples(result) > 0) record = readUser(result, 0);
        PQclear(result);
        return record;
    }

    std::optional<UserRecord> findUserById(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified "
                     "FROM users WHERE id = $1",
                     {std::to_string(id)}, &result)) {
            return std::nullopt;
        }
        std::optional<UserRecord> record;
        if (PQntuples(result) > 0) record = readUser(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<UserRecord> searchUsers(const std::string& query, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<UserRecord> users;
        PGresult* result = nullptr;
        // Экранируем LIKE-маски: иначе «%» в запросе матчит всё подряд.
        std::string escaped;
        escaped.reserve(query.size());
        for (const char ch : query) {
            if (ch == '\\' || ch == '%' || ch == '_') escaped.push_back('\\');
            escaped.push_back(ch);
        }
        const std::string pattern = "%" + escaped + "%";
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified "
                     "FROM users WHERE lower(email) LIKE LOWER($1) ESCAPE '\\' "
                     "OR lower(display_name) LIKE LOWER($1) ESCAPE '\\' "
                     "ORDER BY id LIMIT $2",
                     {pattern, std::to_string(limit)}, &result)) {
            return users;
        }
        for (int row = 0; row < PQntuples(result); ++row) users.push_back(readUser(result, row));
        PQclear(result);
        return users;
    }

    DatabaseError touchUser(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET last_seen_at = now() WHERE id = $1", {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- sessions
    DatabaseError createSession(const SessionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO sessions (id, user_id, jwt_id, device, remote_addr, created_at, expires_at) "
                "VALUES ($1::uuid, $2, $3, $4, $5, now(), $6::timestamptz)",
                {record.id, std::to_string(record.userId), record.jwtId, record.device, record.remoteAddr,
                 record.expiresAt},
                &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError revokeSession(const std::string& jwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() WHERE jwt_id = $1 AND revoked_at IS NULL",
                     {jwtId}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<SessionRecord> findSessionByJwt(const std::string& jwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, jwt_id, device, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM sessions WHERE jwt_id = $1 AND revoked_at IS NULL AND expires_at > now() LIMIT 1",
                     {jwtId}, &result)) {
            return std::nullopt;
        }
        std::optional<SessionRecord> record;
        if (PQntuples(result) > 0) record = readSession(result, 0);
        PQclear(result);
        return record;
    }

    DatabaseError updatePasswordHash(long long userId, const std::string& hash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET password_hash = $2 WHERE id = $1",
                     {std::to_string(userId), hash}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    DatabaseError setEmailVerified(long long userId, bool verified) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET email_verified = $2 WHERE id = $1",
                     {std::to_string(userId), verified ? "TRUE" : "FALSE"}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    std::vector<SessionRecord> listSessions(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionRecord> sessions;
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, jwt_id, device, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM sessions WHERE user_id = $1 AND revoked_at IS NULL AND expires_at > now() "
                     "ORDER BY created_at DESC",
                     {std::to_string(userId)}, &result)) {
            return sessions;
        }
        for (int row = 0; row < PQntuples(result); ++row) sessions.push_back(readSession(result, row));
        PQclear(result);
        return sessions;
    }

    DatabaseError revokeSessionById(long long userId, const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() "
                     "WHERE id = $1::uuid AND user_id = $2 AND revoked_at IS NULL",
                     {sessionId, std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("сессия не найдена");
    }

    DatabaseError revokeAllSessions(long long userId, const std::string& exceptJwtId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL AND jwt_id <> $2",
                     {std::to_string(userId), exceptJwtId}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ------------------------------------------------------- auth tokens
    DatabaseError createAuthToken(const AuthTokenRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO auth_tokens (id, user_id, purpose, token_hash, created_at, expires_at) "
                     "VALUES ($1::uuid, $2, $3, $4, now(), $5::timestamptz)",
                     {record.id, std::to_string(record.userId), record.purpose, record.tokenHash,
                      record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<AuthTokenRecord> findAuthToken(const std::string& purpose,
                                                 const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, purpose, token_hash, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM auth_tokens WHERE purpose = $1 AND token_hash = $2 "
                     "AND used_at IS NULL AND expires_at > now() LIMIT 1",
                     {purpose, tokenHash}, &result)) {
            return std::nullopt;
        }
        std::optional<AuthTokenRecord> record;
        if (PQntuples(result) > 0) {
            AuthTokenRecord value;
            value.id = value_(result, 0, 0);
            value.userId = std::atoll(value_(result, 0, 1).c_str());
            value.purpose = value_(result, 0, 2);
            value.tokenHash = value_(result, 0, 3);
            value.createdAt = value_(result, 0, 4);
            value.expiresAt = value_(result, 0, 5);
            value.usedAt = value_(result, 0, 6);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError useAuthToken(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE auth_tokens SET used_at = now() WHERE id = $1::uuid AND used_at IS NULL",
                     {id}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("код не найден");
    }

    DatabaseError deleteAuthTokens(long long userId, const std::string& purpose) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM auth_tokens WHERE user_id = $1 AND purpose = $2",
                     {std::to_string(userId), purpose}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ---------------------------------------------------- refresh tokens
    DatabaseError createRefreshToken(const RefreshTokenRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO refresh_tokens (id, user_id, family_id, token_hash, device, "
                     "remote_addr, created_at, expires_at) "
                     "VALUES ($1::uuid, $2, $3::uuid, $4, $5, $6, now(), $7::timestamptz)",
                     {record.id, std::to_string(record.userId), record.familyId, record.tokenHash,
                      record.device, record.remoteAddr, record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<RefreshTokenRecord> findRefreshToken(const std::string& tokenHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, family_id::text, token_hash, device, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "COALESCE(replaced_by::text, '') "
                     "FROM refresh_tokens WHERE token_hash = $1 LIMIT 1",
                     {tokenHash}, &result)) {
            return std::nullopt;
        }
        std::optional<RefreshTokenRecord> record;
        if (PQntuples(result) > 0) {
            RefreshTokenRecord value;
            value.id = value_(result, 0, 0);
            value.userId = std::atoll(value_(result, 0, 1).c_str());
            value.familyId = value_(result, 0, 2);
            value.tokenHash = value_(result, 0, 3);
            value.device = value_(result, 0, 4);
            value.remoteAddr = value_(result, 0, 5);
            value.createdAt = value_(result, 0, 6);
            value.expiresAt = value_(result, 0, 7);
            value.revokedAt = value_(result, 0, 8);
            value.replacedBy = value_(result, 0, 9);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError revokeRefreshToken(const std::string& id, const std::string& replacedBy) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now(), replaced_by = NULLIF($2, '')::uuid "
                     "WHERE id = $1::uuid",
                     {id, replacedBy}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("refresh-токен не найден");
    }

    DatabaseError revokeRefreshFamily(const std::string& familyId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now() "
                     "WHERE family_id = $1::uuid AND revoked_at IS NULL",
                     {familyId}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError revokeAllUserRefreshTokens(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- audit log
    DatabaseError insertAudit(long long userId,
                              const std::string& kind,
                              const Json& detail,
                              const std::string& remoteAddr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO audit_logs (user_id, kind, detail, remote_addr) "
                     "VALUES (NULLIF($1, '0')::bigint, $2, $3::jsonb, $4)",
                     {std::to_string(userId), kind,
                      detail.isObject() ? detail.dump() : std::string("{}"), remoteAddr},
                     &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<AuditRecord> listAudit(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditRecord> logs;
        PGresult* result = nullptr;
        if (!execute("SELECT id, user_id, kind, detail::text, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM audit_logs WHERE user_id = $1 ORDER BY id DESC LIMIT $2",
                     {std::to_string(userId), std::to_string(limit)}, &result)) {
            return logs;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            AuditRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.userId = userId;
            record.kind = value_(result, row, 2);
            std::string error;
            const Json detail = Json::parse(value_(result, row, 3), &error);
            record.detail = detail.isNull() ? Json::object() : detail;
            record.remoteAddr = value_(result, row, 4);
            record.createdAt = value_(result, row, 5);
            logs.push_back(record);
        }
        PQclear(result);
        return logs;
    }

    // ------------------------------------------------------------ 2FA
    DatabaseError upsertTwoFactor(const TwoFactorRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO two_factor_settings (user_id, secret_encrypted, enabled, last_used_counter, enabled_at) "
                "VALUES ($1, $2, $3, $4, NULLIF($5, '')::timestamptz) "
                "ON CONFLICT (user_id) DO UPDATE SET secret_encrypted = EXCLUDED.secret_encrypted, "
                "enabled = EXCLUDED.enabled, last_used_counter = EXCLUDED.last_used_counter, updated_at = now()",
                {std::to_string(record.userId), record.secretEncrypted,
                 record.enabled ? "true" : "false", std::to_string(record.lastUsedCounter), record.enabledAt},
                &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TwoFactorRecord> findTwoFactor(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT user_id, secret_encrypted, enabled, last_used_counter, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(enabled_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM two_factor_settings WHERE user_id = $1 LIMIT 1",
                     {std::to_string(userId)}, &result)) {
            return std::nullopt;
        }
        std::optional<TwoFactorRecord> record;
        if (PQntuples(result) > 0) {
            TwoFactorRecord value;
            value.userId = std::atoll(value_(result, 0, 0).c_str());
            value.secretEncrypted = value_(result, 0, 1);
            value.enabled = value_(result, 0, 2) == "t";
            value.lastUsedCounter = std::atoll(value_(result, 0, 3).c_str());
            value.createdAt = value_(result, 0, 4);
            value.enabledAt = value_(result, 0, 5);
            value.updatedAt = value_(result, 0, 6);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError setTwoFactorEnabled(long long userId, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE two_factor_settings SET enabled = $2, "
                     "enabled_at = CASE WHEN $2 AND enabled_at IS NULL THEN now() ELSE enabled_at END, "
                     "updated_at = now() WHERE user_id = $1",
                     {std::to_string(userId), enabled ? "true" : "false"}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError updateTwoFactorCounter(long long userId, long long counter) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE two_factor_settings SET last_used_counter = $2 WHERE user_id = $1",
                     {std::to_string(userId), std::to_string(counter)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError deleteTwoFactor(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM two_factor_settings WHERE user_id = $1",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError replaceRecoveryCodes(long long userId,
                                       const std::vector<RecoveryCodeRecord>& codes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM recovery_codes WHERE user_id = $1", {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        for (const auto& code : codes) {
            PGresult* insertResult = nullptr;
            const bool ok = execute("INSERT INTO recovery_codes (id, user_id, code_hash) "
                                    "VALUES ($1::uuid, $2, $3)",
                                    {code.id, std::to_string(userId), code.codeHash}, &insertResult);
            if (insertResult) PQclear(insertResult);
            if (!ok) return DatabaseError::failure(lastError_);
        }
        return DatabaseError::success();
    }

    std::optional<RecoveryCodeRecord> findRecoveryCode(long long userId,
                                                       const std::string& codeHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, code_hash, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM recovery_codes WHERE user_id = $1 AND code_hash = $2 AND used_at IS NULL "
                     "LIMIT 1",
                     {std::to_string(userId), codeHash}, &result)) {
            return std::nullopt;
        }
        std::optional<RecoveryCodeRecord> record;
        if (PQntuples(result) > 0) {
            RecoveryCodeRecord value;
            value.id = value_(result, 0, 0);
            value.userId = std::atoll(value_(result, 0, 1).c_str());
            value.codeHash = value_(result, 0, 2);
            value.createdAt = value_(result, 0, 3);
            value.usedAt = value_(result, 0, 4);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError useRecoveryCode(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE recovery_codes SET used_at = now() WHERE id = $1::uuid AND used_at IS NULL",
                     {id}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("резервный код не найден");
    }

    int countUnusedRecoveryCodes(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT count(*) FROM recovery_codes WHERE user_id = $1 AND used_at IS NULL",
                     {std::to_string(userId)}, &result)) {
            return 0;
        }
        const int count = std::atoi(value_(result, 0, 0).c_str());
        PQclear(result);
        return count;
    }

    DatabaseError addTrustedDevice(const TrustedDeviceRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO trusted_devices (id, user_id, device_hash, device, remote_addr, "
                     "expires_at, last_used_at) VALUES ($1::uuid, $2, $3, $4, $5, $6::timestamptz, now()) "
                     "ON CONFLICT (user_id, device_hash) DO UPDATE SET revoked_at = NULL, "
                     "expires_at = EXCLUDED.expires_at, last_used_at = now()",
                     {record.id, std::to_string(record.userId), record.deviceHash, record.device,
                      record.remoteAddr, record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TrustedDeviceRecord> findTrustedDevice(long long userId,
                                                         const std::string& deviceHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, device_hash, device, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM trusted_devices WHERE user_id = $1 AND device_hash = $2 "
                     "AND revoked_at IS NULL AND expires_at > now() LIMIT 1",
                     {std::to_string(userId), deviceHash}, &result)) {
            return std::nullopt;
        }
        std::optional<TrustedDeviceRecord> record;
        if (PQntuples(result) > 0) record = readTrustedDevice(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<TrustedDeviceRecord> listTrustedDevices(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TrustedDeviceRecord> devices;
        PGresult* result = nullptr;
        if (!execute("SELECT id::text, user_id, device_hash, device, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM trusted_devices WHERE user_id = $1 AND revoked_at IS NULL "
                     "AND expires_at > now() ORDER BY created_at DESC",
                     {std::to_string(userId)}, &result)) {
            return devices;
        }
        for (int row = 0; row < PQntuples(result); ++row) devices.push_back(readTrustedDevice(result, row));
        PQclear(result);
        return devices;
    }

    DatabaseError touchTrustedDevice(long long userId, const std::string& deviceHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET last_used_at = now() "
                     "WHERE user_id = $1 AND device_hash = $2 AND revoked_at IS NULL",
                     {std::to_string(userId), deviceHash}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeTrustedDevice(long long userId, const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET revoked_at = now() "
                     "WHERE id = $1::uuid AND user_id = $2 AND revoked_at IS NULL",
                     {id, std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeAllTrustedDevices(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ------------------------------------------------------------ chats
    DatabaseError createChat(const std::string& kind,
                             const std::string& title,
                             long long createdBy,
                             const std::vector<long long>& members,
                             long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO chats (kind, title, created_by) VALUES ($1, $2, NULLIF($3, 0)) RETURNING id",
                {kind, title, std::to_string(createdBy)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        outId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);

        for (const long long userId : members) {
            PGresult* memberResult = nullptr;
            const bool ok = execute(
                "INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, $3) "
                "ON CONFLICT DO NOTHING",
                {std::to_string(outId), std::to_string(userId), userId == createdBy ? "owner" : "member"},
                &memberResult);
            if (memberResult) PQclear(memberResult);
            if (!ok) return DatabaseError::failure(lastError_);
        }
        return DatabaseError::success();
    }

    std::vector<ChatRecord> listChats(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ChatRecord> chats;
        PGresult* result = nullptr;
        if (!execute(
                "SELECT chat_id, kind, title, created_at, last_message_at, last_message_body, "
                "COALESCE(last_message_sender_id, 0), COALESCE(last_message_sender, '') "
                "FROM chat_previews WHERE chat_id IN (SELECT chat_id FROM chat_members WHERE user_id = $1) "
                "ORDER BY last_message_at DESC NULLS LAST, chat_id DESC LIMIT $2",
                {std::to_string(userId), std::to_string(limit)}, &result)) {
            return chats;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            ChatRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.kind = value_(result, row, 1);
            record.title = value_(result, row, 2);
            record.createdAt = value_(result, row, 3);
            record.lastMessageAt = value_(result, row, 4);
            record.lastMessageBody = value_(result, row, 5);
            record.lastMessageSenderId = std::atoll(value_(result, row, 6).c_str());
            record.lastMessageSender = value_(result, row, 7);
            chats.push_back(record);
        }
        PQclear(result);
        return chats;
    }

    std::optional<ChatRecord> findChat(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT chat_id, kind, title, created_at, last_message_at, last_message_body, "
                     "COALESCE(last_message_sender_id, 0), COALESCE(last_message_sender, '') "
                     "FROM chat_previews WHERE chat_id = $1",
                     {std::to_string(chatId)}, &result)) {
            return std::nullopt;
        }
        std::optional<ChatRecord> record;
        if (PQntuples(result) > 0) {
            ChatRecord value;
            value.id = chatId;
            value.kind = value_(result, 0, 1);
            value.title = value_(result, 0, 2);
            value.createdAt = value_(result, 0, 3);
            value.lastMessageAt = value_(result, 0, 4);
            value.lastMessageBody = value_(result, 0, 5);
            value.lastMessageSenderId = std::atoll(value_(result, 0, 6).c_str());
            value.lastMessageSender = value_(result, 0, 7);
            record = value;
        }
        PQclear(result);
        return record;
    }

    std::optional<long long> findDirectChat(long long first, long long second) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT c.id FROM chats c "
                "JOIN chat_members a ON a.chat_id = c.id AND a.user_id = $1 "
                "JOIN chat_members b ON b.chat_id = c.id AND b.user_id = $2 "
                "WHERE c.kind IN ('direct', 'agent') "
                "AND (SELECT count(*) FROM chat_members m WHERE m.chat_id = c.id) = 2 "
                "ORDER BY c.id LIMIT 1",
                {std::to_string(first), std::to_string(second)}, &result)) {
            return std::nullopt;
        }
        std::optional<long long> chatId;
        if (PQntuples(result) > 0) chatId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return chatId;
    }

    std::vector<MemberRecord> listMembers(long long chatId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MemberRecord> members;
        PGresult* result = nullptr;
        if (!execute("SELECT m.chat_id, m.user_id, m.role, COALESCE(u.display_name, ''), COALESCE(u.email, ''), "
                     "to_char(m.last_read_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM chat_members m LEFT JOIN users u ON u.id = m.user_id WHERE m.chat_id = $1 "
                     "ORDER BY m.user_id",
                     {std::to_string(chatId)}, &result)) {
            return members;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            MemberRecord record;
            record.chatId = std::atoll(value_(result, row, 0).c_str());
            record.userId = std::atoll(value_(result, row, 1).c_str());
            record.role = value_(result, row, 2);
            record.displayName = value_(result, row, 3);
            record.email = value_(result, row, 4);
            record.lastReadAt = value_(result, row, 5);
            members.push_back(record);
        }
        PQclear(result);
        return members;
    }

    bool isMember(long long chatId, long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT 1 FROM chat_members WHERE chat_id = $1 AND user_id = $2",
                     {std::to_string(chatId), std::to_string(userId)}, &result)) {
            return false;
        }
        const bool member = PQntuples(result) > 0;
        PQclear(result);
        return member;
    }

    DatabaseError markRead(long long chatId, long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("UPDATE chat_members SET last_read_at = now() WHERE chat_id = $1 AND user_id = $2",
                     {std::to_string(chatId), std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- messages
    DatabaseError insertMessage(const MessageRecord& record, long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO messages (chat_id, sender_id, kind, body, payload) "
                "VALUES ($1, NULLIF($2, 0), $3, $4, $5::jsonb) RETURNING id",
                {std::to_string(record.chatId), std::to_string(record.senderId), record.kind, record.body,
                 record.payload.isObject() ? record.payload.dump() : std::string("{}")},
                &result)) {
            return DatabaseError::failure(lastError_);
        }
        outId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<MessageRecord> listMessages(long long chatId, long long beforeId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MessageRecord> messages;
        PGresult* result = nullptr;
        const std::string sql =
            "SELECT m.id, m.chat_id, COALESCE(m.sender_id, 0), COALESCE(u.display_name, ''), m.kind, m.body, "
            "m.payload::text, to_char(m.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
            "FROM messages m LEFT JOIN users u ON u.id = m.sender_id "
            "WHERE m.chat_id = $1 AND ($2 = 0 OR m.id < $2) "
            "ORDER BY m.created_at DESC, m.id DESC LIMIT $3";
        if (!execute(sql,
                     {std::to_string(chatId), std::to_string(beforeId), std::to_string(limit)}, &result)) {
            return messages;
        }
        for (int row = PQntuples(result) - 1; row >= 0; --row) {
            MessageRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.chatId = std::atoll(value_(result, row, 1).c_str());
            record.senderId = std::atoll(value_(result, row, 2).c_str());
            record.senderName = value_(result, row, 3);
            record.kind = value_(result, row, 4);
            record.body = value_(result, row, 5);
            record.payload = Json::parse(value_(result, row, 6));
            if (record.payload.isNull()) record.payload = Json::object();
            record.createdAt = value_(result, row, 7);
            messages.push_back(record);
        }
        PQclear(result);
        return messages;
    }

    // ------------------------------------------------------- user_memory
    std::vector<MemoryRecord> listMemory(long long userId, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MemoryRecord> entries;
        PGresult* result = nullptr;
        if (!execute("SELECT id, user_id, kind, text, weight, tags, "
                     "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM user_memory WHERE user_id = $1 ORDER BY weight DESC, updated_at DESC LIMIT $2",
                     {std::to_string(userId), std::to_string(limit)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            MemoryRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.userId = std::atoll(value_(result, row, 1).c_str());
            record.kind = value_(result, row, 2);
            record.text = value_(result, row, 3);
            record.weight = std::atof(value_(result, row, 4).c_str());
            record.tags = parseArray(PQgetvalue(result, row, 5));
            record.updatedAt = value_(result, row, 6);
            entries.push_back(record);
        }
        PQclear(result);
        return entries;
    }

    DatabaseError upsertMemory(long long userId,
                               const std::string& kind,
                               const std::string& text,
                               double weight,
                               const std::vector<std::string>& tags) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        std::ostringstream weightText;
        weightText << weight;
        if (!execute(
                "INSERT INTO user_memory (user_id, kind, text, weight, tags) "
                "VALUES ($1, $2, $3, $4, $5::text[]) "
                "ON CONFLICT (user_id, text) DO UPDATE "
                "SET kind = EXCLUDED.kind, weight = LEAST(5.0, user_memory.weight + 0.25), "
                "tags = EXCLUDED.tags, updated_at = now()",
                {std::to_string(userId), kind, text, weightText.str(), arrayLiteral(tags)}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // -------------------------------------------------- user_preferences
    Json getPreferences(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT jsonb_build_object("
                "'diet', diet, 'transport', transport, 'preferred_hours', preferred_hours, "
                "'work_hours', work_hours, 'budget_limit', budget_limit, 'city', city, "
                "'lat', lat, 'lon', lon, 'theme', theme, 'notifications', notifications, "
                "'updated_at', to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'))::text "
                "FROM user_preferences WHERE user_id = $1",
                {std::to_string(userId)}, &result)) {
            return Json::object();
        }
        Json preferences = Json::object();
        if (PQntuples(result) > 0) {
            preferences = Json::parse(PQgetvalue(result, 0, 0));
            if (preferences.isNull()) preferences = Json::object();
        }
        PQclear(result);
        return preferences;
    }

    DatabaseError setPreferences(long long userId, const Json& preferences) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "UPDATE user_preferences SET "
                "diet = COALESCE((SELECT array_agg(x) FROM jsonb_array_elements_text($2::jsonb->'diet') x), diet), "
                "transport = COALESCE($2::jsonb->>'transport', transport), "
                "preferred_hours = COALESCE((SELECT array_agg((x)::int) FROM jsonb_array_elements_text($2::jsonb->'preferred_hours') x), preferred_hours), "
                "work_hours = COALESCE((SELECT array_agg((x)::int) FROM jsonb_array_elements_text($2::jsonb->'work_hours') x), work_hours), "
                "budget_limit = COALESCE(($2::jsonb->>'budget_limit')::numeric, budget_limit), "
                "city = COALESCE($2::jsonb->>'city', city), "
                "lat = COALESCE(($2::jsonb->>'lat')::double precision, lat), "
                "lon = COALESCE(($2::jsonb->>'lon')::double precision, lon), "
                "theme = COALESCE($2::jsonb->>'theme', theme), "
                "notifications = COALESCE($2::jsonb->'notifications', notifications), "
                "updated_at = now() "
                "WHERE user_id = $1",
                {std::to_string(userId), preferences.dump()}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        if (!updated) return DatabaseError::failure("строка настроек не найдена");
        return DatabaseError::success();
    }

    // ------------------------------------------------- tool_permissions
    std::string getToolPermission(long long userId, const std::string& tool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute("SELECT mode FROM tool_permissions WHERE user_id = $1 AND tool = $2",
                     {std::to_string(userId), tool}, &result)) {
            return "";
        }
        std::string mode;
        if (PQntuples(result) > 0) mode = value_(result, 0, 0);
        PQclear(result);
        return mode;
    }

    std::vector<ToolPermissionRecord> listToolPermissions(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ToolPermissionRecord> entries;
        PGresult* result = nullptr;
        if (!execute(
                "SELECT tool, mode, to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                "FROM tool_permissions WHERE user_id = $1 ORDER BY tool",
                {std::to_string(userId)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            ToolPermissionRecord record;
            record.userId = userId;
            record.tool = value_(result, row, 0);
            record.mode = value_(result, row, 1);
            record.updatedAt = value_(result, row, 2);
            entries.push_back(record);
        }
        PQclear(result);
        return entries;
    }

    DatabaseError setToolPermission(long long userId,
                                    const std::string& tool,
                                    const std::string& mode) override {
        if (mode != "allow" && mode != "ask" && mode != "deny") {
            return DatabaseError::failure("недопустимый режим разрешения");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO tool_permissions (user_id, tool, mode) VALUES ($1, $2, $3) "
                "ON CONFLICT (user_id, tool) DO UPDATE SET mode = EXCLUDED.mode, updated_at = now()",
                {std::to_string(userId), tool, mode}, &result)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ---------------------------------------------------- pending_actions
    long long createPendingAction(const PendingActionRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        const std::string status = record.status.empty() ? std::string("pending") : record.status;
        if (!execute(
                "INSERT INTO pending_actions (user_id, chat_id, tool, args, summary, status) "
                "VALUES ($1, NULLIF($2, '0')::bigint, $3, $4::jsonb, $5, $6) RETURNING id",
                {std::to_string(record.userId), std::to_string(record.chatId), record.tool,
                 record.args.isObject() ? record.args.dump() : std::string("{}"), record.summary, status},
                &result)) {
            PQclear(result);
            return 0;
        }
        long long id = 0;
        if (PQntuples(result) > 0) id = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return id;
    }

    std::optional<PendingActionRecord> findPendingAction(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(pendingActionSelect() + " WHERE id = $1", {std::to_string(id)}, &result)) {
            return std::nullopt;
        }
        std::optional<PendingActionRecord> record;
        if (PQntuples(result) > 0) record = readPendingAction(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<PendingActionRecord> listPendingActions(long long userId,
                                                        const std::string& status,
                                                        int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingActionRecord> entries;
        PGresult* result = nullptr;
        bool ok = false;
        if (status.empty()) {
            ok = execute(pendingActionSelect() + " WHERE user_id = $1 ORDER BY id DESC LIMIT $2",
                         {std::to_string(userId), std::to_string(limit)}, &result);
        } else {
            ok = execute(pendingActionSelect() + " WHERE user_id = $1 AND status = $2 ORDER BY id DESC LIMIT $3",
                         {std::to_string(userId), status, std::to_string(limit)}, &result);
        }
        if (!ok) return entries;
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readPendingAction(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError resolvePendingAction(long long id,
                                       const std::string& status,
                                       const Json& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE pending_actions SET status = $2, result = $3::jsonb, resolved_at = now() "
                "WHERE id = $1",
                {std::to_string(id), status, result.isObject() ? result.dump() : std::string("{}")}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("отложенное действие не найдено");
        return DatabaseError::success();
    }

    // -------------------------------------------------------------- tasks
    DatabaseError createTask(const TaskRecord& record, long long& outId) override {
        if (record.title.empty()) return DatabaseError::failure("пустой заголовок задачи");
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        const std::string status = record.status.empty() ? std::string("pending") : record.status;
        if (!execute(
                "INSERT INTO tasks (user_id, chat_id, title, notes, status, priority, due_at, remind_at) "
                "VALUES ($1, NULLIF($2, '0')::bigint, $3, $4, $5, $6, "
                "NULLIF($7, '')::timestamptz, NULLIF($8, '')::timestamptz) RETURNING id",
                {std::to_string(record.userId), std::to_string(record.chatId), record.title, record.notes,
                 status, std::to_string(record.priority), record.dueAt, record.remindAt},
                &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError_);
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TaskRecord> findTask(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(taskSelect() + " WHERE id = $1", {std::to_string(id)}, &result)) return std::nullopt;
        std::optional<TaskRecord> record;
        if (PQntuples(result) > 0) record = readTask(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<TaskRecord> listTasks(long long userId, const std::string& status, int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TaskRecord> entries;
        PGresult* result = nullptr;
        bool ok = false;
        if (status.empty()) {
            ok = execute(taskSelect() + " WHERE user_id = $1 ORDER BY id DESC LIMIT $2",
                         {std::to_string(userId), std::to_string(limit)}, &result);
        } else {
            ok = execute(taskSelect() + " WHERE user_id = $1 AND status = $2 ORDER BY id DESC LIMIT $3",
                         {std::to_string(userId), status, std::to_string(limit)}, &result);
        }
        if (!ok) return entries;
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readTask(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError setTaskStatus(long long id, const std::string& status) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE tasks SET status = $2, "
                "completed_at = CASE WHEN $2 = 'done' THEN now() ELSE completed_at END WHERE id = $1",
                {std::to_string(id), status}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("задача не найдена");
        return DatabaseError::success();
    }

    DatabaseError deleteTask(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute("DELETE FROM tasks WHERE id = $1", {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool deleted = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!deleted) return DatabaseError::failure("задача не найдена");
        return DatabaseError::success();
    }

    std::vector<TaskRecord> listDueTasks(int limit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TaskRecord> entries;
        PGresult* result = nullptr;
        if (!execute(taskSelect() +
                         " WHERE status = 'pending' AND reminded_at IS NULL "
                         "AND remind_at IS NOT NULL AND remind_at <= now() ORDER BY remind_at LIMIT $1",
                     {std::to_string(limit)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readTask(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError markTaskReminded(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute("UPDATE tasks SET reminded_at = now() WHERE id = $1", {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("задача не найдена");
        return DatabaseError::success();
    }

    // ------------------------------------------- integration_connections
    DatabaseError upsertIntegrationConnection(const IntegrationConnectionRecord& record,
                                              long long& outId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        const std::string status = record.status.empty() ? std::string("active") : record.status;
        if (!execute(
                "INSERT INTO integration_connections "
                "(user_id, provider, account, scope, token_encrypted, status, last_error) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7) "
                "ON CONFLICT (user_id, provider) DO UPDATE SET "
                "account = EXCLUDED.account, scope = EXCLUDED.scope, "
                "token_encrypted = EXCLUDED.token_encrypted, status = EXCLUDED.status, "
                "last_error = EXCLUDED.last_error RETURNING id",
                {std::to_string(record.userId), record.provider, record.account, record.scope,
                 record.tokenEncrypted, status, record.lastError},
                &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError_);
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<IntegrationConnectionRecord> findIntegrationConnection(
        long long userId, const std::string& provider) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(integrationSelect() + " WHERE user_id = $1 AND provider = $2",
                     {std::to_string(userId), provider}, &result)) {
            return std::nullopt;
        }
        std::optional<IntegrationConnectionRecord> record;
        if (PQntuples(result) > 0) record = readIntegration(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<IntegrationConnectionRecord> listIntegrationConnections(long long userId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<IntegrationConnectionRecord> entries;
        PGresult* result = nullptr;
        if (!execute(integrationSelect() + " WHERE user_id = $1 ORDER BY provider",
                     {std::to_string(userId)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readIntegration(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError updateIntegrationToken(long long id,
                                         const std::string& tokenEncrypted,
                                         const std::string& status,
                                         const std::string& lastError) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE integration_connections SET token_encrypted = $2, status = $3, last_error = $4 "
                "WHERE id = $1",
                {std::to_string(id), tokenEncrypted, status, lastError}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    DatabaseError touchIntegrationUsed(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute("UPDATE integration_connections SET last_used_at = now() WHERE id = $1",
                     {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    DatabaseError deleteIntegrationConnection(long long id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute("DELETE FROM integration_connections WHERE id = $1", {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool deleted = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!deleted) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    // --------------------------------------------- integration_oauth_states
    DatabaseError createOauthState(const OauthStateRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute(
                "INSERT INTO integration_oauth_states "
                "(state, user_id, provider, verifier, redirect_uri, expires_at) "
                "VALUES ($1, $2, $3, $4, $5, $6::timestamptz)",
                {record.state, std::to_string(record.userId), record.provider, record.verifier,
                 record.redirectUri, record.expiresAt},
                &res)) {
            return DatabaseError::failure(lastError_);
        }
        PQclear(res);
        return DatabaseError::success();
    }

    std::optional<OauthStateRecord> findOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT state, user_id, provider, verifier, redirect_uri, "
                "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "to_char(used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM integration_oauth_states WHERE state = $1",
                {state}, &result)) {
            return std::nullopt;
        }
        std::optional<OauthStateRecord> record;
        if (PQntuples(result) > 0) {
            OauthStateRecord value;
            value.state = value_(result, 0, 0);
            value.userId = std::atoll(value_(result, 0, 1).c_str());
            value.provider = value_(result, 0, 2);
            value.verifier = value_(result, 0, 3);
            value.redirectUri = value_(result, 0, 4);
            value.expiresAt = value_(result, 0, 5);
            value.usedAt = value_(result, 0, 6);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError useOauthState(const std::string& state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        PGresult* res = nullptr;
        if (!execute("UPDATE integration_oauth_states SET used_at = now() WHERE state = $1", {state}, &res)) {
            return DatabaseError::failure(lastError_);
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("состояние OAuth не найдено");
        return DatabaseError::success();
    }

private:
    static std::string integrationSelect() {
        return "SELECT id, user_id, provider, account, scope, token_encrypted, status, last_error, "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
               "to_char(last_used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
               "FROM integration_connections";
    }

    IntegrationConnectionRecord readIntegration(PGresult* result, int row) const {
        IntegrationConnectionRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.provider = value_(result, row, 2);
        record.account = value_(result, row, 3);
        record.scope = value_(result, row, 4);
        record.tokenEncrypted = value_(result, row, 5);
        record.status = value_(result, row, 6);
        record.lastError = value_(result, row, 7);
        record.createdAt = value_(result, row, 8);
        record.lastUsedAt = value_(result, row, 9);
        return record;
    }

    static std::string taskSelect() {
        const char* ts = "to_char(%s AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";
        char due[96], remind[96], reminded[96], created[96], completed[96];
        std::snprintf(due, sizeof(due), ts, "due_at");
        std::snprintf(remind, sizeof(remind), ts, "remind_at");
        std::snprintf(reminded, sizeof(reminded), ts, "reminded_at");
        std::snprintf(created, sizeof(created), ts, "created_at");
        std::snprintf(completed, sizeof(completed), ts, "completed_at");
        return std::string("SELECT id, user_id, COALESCE(chat_id, 0), title, notes, status, priority, ") +
               due + ", " + remind + ", " + reminded + ", " + created + ", " + completed + " FROM tasks";
    }

    TaskRecord readTask(PGresult* result, int row) const {
        TaskRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.chatId = std::atoll(value_(result, row, 2).c_str());
        record.title = value_(result, row, 3);
        record.notes = value_(result, row, 4);
        record.status = value_(result, row, 5);
        record.priority = std::atoi(value_(result, row, 6).c_str());
        record.dueAt = value_(result, row, 7);
        record.remindAt = value_(result, row, 8);
        record.remindedAt = value_(result, row, 9);
        record.createdAt = value_(result, row, 10);
        record.completedAt = value_(result, row, 11);
        return record;
    }

    static std::string pendingActionSelect() {
        return "SELECT id, user_id, COALESCE(chat_id, 0), tool, args::text, summary, status, result::text, "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
               "to_char(resolved_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
               "FROM pending_actions";
    }

    PendingActionRecord readPendingAction(PGresult* result, int row) const {
        PendingActionRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.chatId = std::atoll(value_(result, row, 2).c_str());
        record.tool = value_(result, row, 3);
        Json args = Json::parse(value_(result, row, 4));
        record.args = args.isObject() ? args : Json::object();
        record.summary = value_(result, row, 5);
        record.status = value_(result, row, 6);
        Json res = Json::parse(value_(result, row, 7));
        record.result = res.isObject() ? res : Json::object();
        record.createdAt = value_(result, row, 8);
        record.resolvedAt = value_(result, row, 9);
        return record;
    }

    static std::string value_(PGresult* result, int row, int column) {
        if (PQgetisnull(result, row, column)) return "";
        return PQgetvalue(result, row, column);
    }

    UserRecord readUser(PGresult* result, int row) const {
        UserRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.email = value_(result, row, 1);
        record.passwordHash = value_(result, row, 2);
        record.displayName = value_(result, row, 3);
        record.avatarUrl = value_(result, row, 4);
        record.timezone = value_(result, row, 5);
        record.active = value_(result, row, 6) == "t";
        record.createdAt = value_(result, row, 7);
        record.lastSeenAt = value_(result, row, 8);
        record.emailVerified = value_(result, row, 9) == "t";
        return record;
    }

    SessionRecord readSession(PGresult* result, int row) const {
        SessionRecord record;
        record.id = value_(result, row, 0);
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.jwtId = value_(result, row, 2);
        record.device = value_(result, row, 3);
        record.remoteAddr = value_(result, row, 4);
        record.createdAt = value_(result, row, 5);
        record.expiresAt = value_(result, row, 6);
        record.revokedAt = value_(result, row, 7);
        return record;
    }

    TrustedDeviceRecord readTrustedDevice(PGresult* result, int row) const {
        TrustedDeviceRecord record;
        record.id = value_(result, row, 0);
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.deviceHash = value_(result, row, 2);
        record.device = value_(result, row, 3);
        record.remoteAddr = value_(result, row, 4);
        record.createdAt = value_(result, row, 5);
        record.expiresAt = value_(result, row, 6);
        record.lastUsedAt = value_(result, row, 7);
        record.revokedAt = value_(result, row, 8);
        return record;
    }

    // Выполняет параметризованный запрос. Память результата освобождает вызывающий.
    bool execute(const std::string& sql, const std::vector<std::string>& args, PGresult** out) {
        lastError_.clear();
        if (connection_ == nullptr) {
            lastError_ = "нет соединения с PostgreSQL";
            return false;
        }
        std::vector<const char*> values;
        values.reserve(args.size());
        for (const auto& arg : args) values.push_back(arg.c_str());

        PGresult* result = PQexecParams(connection_, sql.c_str(), static_cast<int>(values.size()), nullptr,
                                        values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
        const ExecStatusType status = PQresultStatus(result);
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            lastError_ = PQresultErrorMessage(result);
            PQclear(result);
            AURA_LOG(log::Level::Error, "db") << "SQL ошибка: " << lastError_;
            return false;
        }
        if (out != nullptr) {
            *out = result;
        } else {
            PQclear(result);
        }
        return true;
    }

    const std::string url_;
    mutable std::mutex mutex_;
    PGconn* connection_ = nullptr;
    std::string lastError_;
};

}  // namespace

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url) {
    return std::make_unique<PostgresDatabase>(url);
}

#else  // !AURA_WITH_LIBPQ

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url) {
    (void)url;
    AURA_LOG(log::Level::Warn, "db")
        << "сервер собран без libpq (нужен -DAURA_WITH_LIBPQ=ON), PostgreSQL недоступен";
    return nullptr;
}

#endif  // AURA_WITH_LIBPQ

}  // namespace aura
