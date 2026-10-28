// selfvoy/database_pg.cpp — реализация IDatabase поверх libpq (PostgreSQL).
//
// Компилируется только при SELFVOY_WITH_LIBPQ (CMake находит libpq автоматически).
// Без него makePostgresDatabase() возвращает nullptr, и DatabaseManager
// переключается на встроенное хранилище с предупреждением в лог.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "selfvoy/config.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/json.h"
#include "selfvoy/log.h"

#ifdef SELFVOY_WITH_LIBPQ
#include <libpq-fe.h>
#endif

namespace selfvoy {

#ifdef SELFVOY_WITH_LIBPQ

namespace {

// Колонки тарифа и Telegram (S1a) в выборках users — после role, читает readUser.
#define SELFVOY_PG_USER_GROWTH_COLUMNS                                                            \
    ", to_char(plan_until AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "              \
    "COALESCE(telegram_chat_id, 0), telegram_username "

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

// Соединение с PostgreSQL и текст последней ошибки на нём.
struct PgConn {
    PGconn* pg = nullptr;
    std::string lastError;
};

// Пул соединений: раньше весь процесс ходил в БД через одно соединение под одним
// mutex — запросы всех потоков шли строго по очереди, один медленный запрос
// задерживал всех. Соединение выдаётся на время одного вызова IDatabase.
class PgPool {
public:
    explicit PgPool(std::size_t size) : conns_(size), free_(size) {
        for (std::size_t i = 0; i < size; ++i) free_[i] = i;
    }
    ~PgPool() {
        for (auto& conn : conns_) {
            if (conn.pg != nullptr) PQfinish(conn.pg);
        }
    }
    PgConn& checkout() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return !free_.empty(); });
        const std::size_t index = free_.back();
        free_.pop_back();
        return conns_[index];
    }
    void giveBack(PgConn& conn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(static_cast<std::size_t>(&conn - conns_.data()));
        }
        available_.notify_one();
    }
    PgConn& first() { return conns_.front(); }
    std::size_t size() const { return conns_.size(); }

private:
    std::vector<PgConn> conns_;
    std::vector<std::size_t> free_;
    std::mutex mutex_;
    std::condition_variable available_;
};

// Текущее соединение потока (выдано Lease).
thread_local PgConn* tlsConn = nullptr;

// Соединение из пула на время вызова; вложенные вызовы в одном потоке не берут второе.
class Lease {
public:
    explicit Lease(PgPool& pool) : pool_(pool), previous_(tlsConn) {
        conn_ = &pool_.checkout();
        tlsConn = conn_;
    }
    ~Lease() {
        tlsConn = previous_;
        pool_.giveBack(*conn_);
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

private:
    PgPool& pool_;
    PgConn* previous_;
    PgConn* conn_ = nullptr;
};

std::size_t poolSizeFromEnv() {
    const long value = std::strtol(envOr("SELFVOY_DB_POOL_SIZE", "0").c_str(), nullptr, 10);
    return static_cast<std::size_t>(value >= 1 && value <= 64 ? value : 8);
}

// Ответ счётчиков rate_limits при сбое БД: больше любого лимита (fail closed).
constexpr int kRateLimitFailClosed = 1 << 20;

class PostgresDatabase : public IDatabase {
public:
    explicit PostgresDatabase(std::string url) : url_(std::move(url)), pool_(poolSizeFromEnv()), lockPool_(1) {}

    ~PostgresDatabase() override = default;

    std::string name() const override { return "postgresql"; }

    // Первое соединение — сразу (ошибка подключения видна при старте), остальные
    // соединения пула открываются по мере надобности (ensureConnected).
    bool connect(std::string& error) override {
        Lease lease(pool_);
        PgConn& conn = *tlsConn;
        conn.pg = PQconnectdb(url_.c_str());
        if (PQstatus(conn.pg) != CONNECTION_OK) {
            error = std::string("PostgreSQL: ") + PQerrorMessage(conn.pg);
            PQfinish(conn.pg);
            conn.pg = nullptr;
            return false;
        }
        SELFVOY_LOG(log::Level::Info, "db") << "PostgreSQL: пул до " << pool_.size() << " соединений";
        return prepareSession();
    }

    bool healthy() const override {
        Lease lease(pool_);
        return ensureConnected();
    }

    // ------------------------------------------------------------ users
    DatabaseError createUser(const std::string& email,
                             const std::string& passwordHash,
                             const std::string& displayName,
                             long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        const std::vector<std::string> args{lower(email), passwordHash, displayName};
        // email_verified = false явно: DEFAULT TRUE в схеме — для аккаунтов, созданных
        // до подтверждения email; новый аккаунт подтверждается через auth.verifyEmail.
        if (!execute(
                "INSERT INTO users (email, password_hash, display_name, email_verified) "
                "VALUES (LOWER($1), $2, $3, false) RETURNING id",
                args, &result)) {
            return DatabaseError::failure(lastError());
        }
        outId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<UserRecord> findUserByEmail(const std::string& email) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified, bio, role" SELFVOY_PG_USER_GROWTH_COLUMNS
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified, bio, role" SELFVOY_PG_USER_GROWTH_COLUMNS
                     "FROM users WHERE id = $1",
                     {std::to_string(id)}, &result)) {
            return std::nullopt;
        }
        std::optional<UserRecord> record;
        if (PQntuples(result) > 0) record = readUser(result, 0);
        PQclear(result);
        return record;
    }

    DatabaseError updateProfile(long long userId, const Json& fields) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET "
                     "display_name = COALESCE($2::jsonb->>'display_name', display_name), "
                     "bio = COALESCE($2::jsonb->>'bio', bio), "
                     "avatar_url = COALESCE($2::jsonb->>'avatar_url', avatar_url) "
                     "WHERE id = $1",
                     {std::to_string(userId), fields.dump()}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        if (!updated) return DatabaseError::failure("пользователь не найден");
        return DatabaseError::success();
    }

    std::vector<UserRecord> searchUsers(const std::string& query, int limit) override {
        Lease lease(pool_);
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
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified, bio, role" SELFVOY_PG_USER_GROWTH_COLUMNS
                     "FROM users WHERE lower(display_name) LIKE LOWER($1) ESCAPE '\\' "
                     "ORDER BY id LIMIT $2",
                     {pattern, std::to_string(limit)}, &result)) {
            return users;
        }
        for (int row = 0; row < PQntuples(result); ++row) users.push_back(readUser(result, row));
        PQclear(result);
        return users;
    }

    DatabaseError touchUser(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET last_seen_at = now() WHERE id = $1", {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- sessions
    DatabaseError createSession(const SessionRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO sessions (id, user_id, jwt_id, device, remote_addr, created_at, expires_at) "
                "VALUES ($1::uuid, $2, $3, $4, $5, now(), $6::timestamptz)",
                {record.id, std::to_string(record.userId), record.jwtId, record.device, record.remoteAddr,
                 record.expiresAt},
                &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError revokeSession(const std::string& jwtId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() WHERE jwt_id = $1 AND revoked_at IS NULL",
                     {jwtId}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<SessionRecord> findSessionByJwt(const std::string& jwtId) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET password_hash = $2 WHERE id = $1",
                     {std::to_string(userId), hash}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    DatabaseError setUserActive(long long userId, bool active) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET is_active = $2 WHERE id = $1",
                     {std::to_string(userId), active ? "TRUE" : "FALSE"}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    DatabaseError deleteUser(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM users WHERE id = $1", {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    DatabaseError setEmailVerified(long long userId, bool verified) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET email_verified = $2 WHERE id = $1",
                     {std::to_string(userId), verified ? "TRUE" : "FALSE"}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    std::vector<SessionRecord> listSessions(long long userId) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() "
                     "WHERE id = $1::uuid AND user_id = $2 AND revoked_at IS NULL",
                     {sessionId, std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("сессия не найдена");
    }

    DatabaseError revokeAllSessions(long long userId, const std::string& exceptJwtId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE sessions SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL AND jwt_id <> $2",
                     {std::to_string(userId), exceptJwtId}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ------------------------------------------------------- auth tokens
    DatabaseError createAuthToken(const AuthTokenRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO auth_tokens (id, user_id, purpose, token_hash, created_at, expires_at) "
                     "VALUES ($1::uuid, $2, $3, $4, now(), $5::timestamptz)",
                     {record.id, std::to_string(record.userId), record.purpose, record.tokenHash,
                      record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<AuthTokenRecord> findAuthToken(const std::string& purpose,
                                                 const std::string& tokenHash) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE auth_tokens SET used_at = now() WHERE id = $1::uuid AND used_at IS NULL",
                     {id}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("код не найден");
    }

    DatabaseError deleteAuthTokens(long long userId, const std::string& purpose) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM auth_tokens WHERE user_id = $1 AND purpose = $2",
                     {std::to_string(userId), purpose}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ---------------------------------------------------- refresh tokens
    DatabaseError createRefreshToken(const RefreshTokenRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO refresh_tokens (id, user_id, family_id, token_hash, device, "
                     "remote_addr, created_at, expires_at) "
                     "VALUES ($1::uuid, $2, $3::uuid, $4, $5, $6, now(), $7::timestamptz)",
                     {record.id, std::to_string(record.userId), record.familyId, record.tokenHash,
                      record.device, record.remoteAddr, record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<RefreshTokenRecord> findRefreshToken(const std::string& tokenHash) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now(), replaced_by = NULLIF($2, '')::uuid "
                     "WHERE id = $1::uuid AND revoked_at IS NULL",
                     {id, replacedBy}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("refresh-токен не найден или уже отозван");
    }

    DatabaseError revokeRefreshFamily(const std::string& familyId) override {
        Lease lease(pool_);
        if (familyId.empty()) return DatabaseError::success();
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now() "
                     "WHERE family_id = $1::uuid AND revoked_at IS NULL",
                     {familyId}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError revokeAllUserRefreshTokens(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE refresh_tokens SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError revokeUserRefreshTokensExcept(long long userId, const std::string& keepFamilyId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        // Сравнение в тексте без дефисов: пустой keep не приводится к uuid.
        if (!execute("UPDATE refresh_tokens SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL "
                     "AND replace(family_id::text, '-', '') <> lower(replace($2, '-', ''))",
                     {std::to_string(userId), keepFamilyId}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- audit log
    DatabaseError insertAudit(long long userId,
                              const std::string& kind,
                              const Json& detail,
                              const std::string& remoteAddr) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO audit_logs (user_id, kind, detail, remote_addr) "
                     "VALUES (NULLIF($1, '0')::bigint, $2, $3::jsonb, $4)",
                     {std::to_string(userId), kind,
                      detail.isObject() ? detail.dump() : std::string("{}"), remoteAddr},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<AuditRecord> listAudit(long long userId, int limit) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO two_factor_settings (user_id, secret_encrypted, enabled, last_used_counter, enabled_at) "
                "VALUES ($1, $2, $3, $4, NULLIF($5, '')::timestamptz) "
                "ON CONFLICT (user_id) DO UPDATE SET secret_encrypted = EXCLUDED.secret_encrypted, "
                "enabled = EXCLUDED.enabled, last_used_counter = EXCLUDED.last_used_counter, updated_at = now()",
                {std::to_string(record.userId), record.secretEncrypted,
                 record.enabled ? "true" : "false", std::to_string(record.lastUsedCounter), record.enabledAt},
                &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TwoFactorRecord> findTwoFactor(long long userId) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE two_factor_settings SET enabled = $2, "
                     "enabled_at = CASE WHEN $2 AND enabled_at IS NULL THEN now() ELSE enabled_at END, "
                     "updated_at = now() WHERE user_id = $1",
                     {std::to_string(userId), enabled ? "true" : "false"}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError updateTwoFactorCounter(long long userId, long long counter) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE two_factor_settings SET last_used_counter = $2 WHERE user_id = $1",
                     {std::to_string(userId), std::to_string(counter)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("настройка 2FA не найдена");
    }

    DatabaseError deleteTwoFactor(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM two_factor_settings WHERE user_id = $1",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    DatabaseError replaceRecoveryCodes(long long userId,
                                       const std::vector<RecoveryCodeRecord>& codes) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("DELETE FROM recovery_codes WHERE user_id = $1", {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        for (const auto& code : codes) {
            PGresult* insertResult = nullptr;
            const bool ok = execute("INSERT INTO recovery_codes (id, user_id, code_hash) "
                                    "VALUES ($1::uuid, $2, $3)",
                                    {code.id, std::to_string(userId), code.codeHash}, &insertResult);
            if (insertResult) PQclear(insertResult);
            if (!ok) return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::optional<RecoveryCodeRecord> findRecoveryCode(long long userId,
                                                       const std::string& codeHash) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE recovery_codes SET used_at = now() WHERE id = $1::uuid AND used_at IS NULL",
                     {id}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("резервный код не найден");
    }

    int countUnusedRecoveryCodes(long long userId) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO trusted_devices (id, user_id, device_hash, device, remote_addr, "
                     "expires_at, last_used_at) VALUES ($1::uuid, $2, $3, $4, $5, $6::timestamptz, now()) "
                     "ON CONFLICT (user_id, device_hash) DO UPDATE SET revoked_at = NULL, "
                     "expires_at = EXCLUDED.expires_at, last_used_at = now()",
                     {record.id, std::to_string(record.userId), record.deviceHash, record.device,
                      record.remoteAddr, record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TrustedDeviceRecord> findTrustedDevice(long long userId,
                                                         const std::string& deviceHash) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET last_used_at = now() "
                     "WHERE user_id = $1 AND device_hash = $2 AND revoked_at IS NULL",
                     {std::to_string(userId), deviceHash}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeTrustedDevice(long long userId, const std::string& id) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET revoked_at = now() "
                     "WHERE id = $1::uuid AND user_id = $2 AND revoked_at IS NULL",
                     {id, std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("доверенное устройство не найдено");
    }

    DatabaseError revokeAllTrustedDevices(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE trusted_devices SET revoked_at = now() "
                     "WHERE user_id = $1 AND revoked_at IS NULL",
                     {std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO chats (kind, title, created_by) VALUES ($1, $2, NULLIF($3, 0)) RETURNING id",
                {kind, title, std::to_string(createdBy)}, &result)) {
            return DatabaseError::failure(lastError());
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
            if (!ok) return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::vector<ChatRecord> listChats(long long userId, int limit) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE chat_members SET last_read_at = now() WHERE chat_id = $1 AND user_id = $2",
                     {std::to_string(chatId), std::to_string(userId)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // --------------------------------------------------------- messages
    DatabaseError insertMessage(const MessageRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO messages (chat_id, sender_id, kind, body, payload) "
                "VALUES ($1, NULLIF($2, 0), $3, $4, $5::jsonb) RETURNING id",
                {std::to_string(record.chatId), std::to_string(record.senderId), record.kind, record.body,
                 record.payload.isObject() ? record.payload.dump() : std::string("{}")},
                &result)) {
            return DatabaseError::failure(lastError());
        }
        outId = std::atoll(PQgetvalue(result, 0, 0));
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<MessageRecord> listMessages(long long chatId, long long beforeId, int limit) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        std::vector<MemoryRecord> entries;
        PGresult* result = nullptr;
        if (!execute("SELECT id, user_id, kind, text, weight, tags, "
                     "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM user_memory WHERE user_id = $1 ORDER BY weight DESC, updated_at DESC, id DESC LIMIT $2",
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
        Lease lease(pool_);
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
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // -------------------------------------------------- user_preferences
    Json getPreferences(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT jsonb_build_object("
                "'diet', diet, 'transport', transport, 'preferred_hours', preferred_hours, "
                "'work_hours', work_hours, 'budget_limit', budget_limit, 'city', city, "
                "'lat', lat, 'lon', lon, 'theme', theme, 'notifications', notifications, 'privacy', privacy, "
                "'birthday', birthday, 'allergies', allergies, 'onboarded', onboarded, "
                "'npd_receipts', npd_receipts, "
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
        Lease lease(pool_);
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
                "privacy = COALESCE(privacy || ($2::jsonb->'privacy'), privacy), "
                "birthday = COALESCE($2::jsonb->>'birthday', birthday), "
                "allergies = COALESCE((SELECT array_agg(x) FROM jsonb_array_elements_text($2::jsonb->'allergies') x), allergies), "
                "onboarded = COALESCE(($2::jsonb->>'onboarded')::boolean, onboarded), "
                "npd_receipts = COALESCE(($2::jsonb->>'npd_receipts')::boolean, npd_receipts), "
                "updated_at = now() "
                "WHERE user_id = $1",
                {std::to_string(userId), preferences.dump()}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        if (!updated) return DatabaseError::failure("строка настроек не найдена");
        return DatabaseError::success();
    }

    // ------------------------------------------------- tool_permissions
    std::string getToolPermission(long long userId, const std::string& tool) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO tool_permissions (user_id, tool, mode) VALUES ($1, $2, $3) "
                "ON CONFLICT (user_id, tool) DO UPDATE SET mode = EXCLUDED.mode, updated_at = now()",
                {std::to_string(userId), tool, mode}, &result)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(result);
        return DatabaseError::success();
    }

    // ---------------------------------------------------- pending_actions
    long long createPendingAction(const PendingActionRecord& record) override {
        Lease lease(pool_);
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

    bool claimPendingAction(long long id, long long userId, const std::string& status) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE pending_actions SET status = $3, resolved_at = now() "
                     "WHERE id = $1 AND user_id = $2 AND status = 'pending'",
                     {std::to_string(id), std::to_string(userId), status}, &result)) {
            return false;
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched;
    }

    std::optional<PendingActionRecord> findPendingAction(long long id) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE pending_actions SET status = $2, result = $3::jsonb, resolved_at = now() "
                "WHERE id = $1",
                {std::to_string(id), status, result.isObject() ? result.dump() : std::string("{}")}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("отложенное действие не найдено");
        return DatabaseError::success();
    }

    // -------------------------------------------------------------- tasks
    DatabaseError createTask(const TaskRecord& record, long long& outId) override {
        if (record.title.empty()) return DatabaseError::failure("пустой заголовок задачи");
        Lease lease(pool_);
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
            return DatabaseError::failure(lastError());
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TaskRecord> findTask(long long id) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(taskSelect() + " WHERE id = $1", {std::to_string(id)}, &result)) return std::nullopt;
        std::optional<TaskRecord> record;
        if (PQntuples(result) > 0) record = readTask(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<TaskRecord> listTasks(long long userId, const std::string& status, int limit) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE tasks SET status = $2, "
                "completed_at = CASE WHEN $2 = 'done' THEN now() ELSE completed_at END WHERE id = $1",
                {std::to_string(id), status}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("задача не найдена");
        return DatabaseError::success();
    }

    DatabaseError deleteTask(long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("DELETE FROM tasks WHERE id = $1", {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool deleted = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!deleted) return DatabaseError::failure("задача не найдена");
        return DatabaseError::success();
    }

    std::vector<TaskRecord> listDueTasks(int limit, long long userId) override {
        Lease lease(pool_);
        std::vector<TaskRecord> entries;
        PGresult* result = nullptr;
        if (!execute(taskSelect() +
                         " WHERE status = 'pending' AND reminded_at IS NULL "
                         "AND remind_at IS NOT NULL AND remind_at <= now() "
                         "AND ($2::bigint = 0 OR user_id = $2::bigint) ORDER BY remind_at LIMIT $1",
                     {std::to_string(limit), std::to_string(userId)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readTask(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError markTaskReminded(long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE tasks SET reminded_at = now() WHERE id = $1 AND reminded_at IS NULL",
                     {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("задача не найдена или напоминание уже отправлено");
        return DatabaseError::success();
    }

    // ------------------------------------------- integration_connections
    DatabaseError upsertIntegrationConnection(const IntegrationConnectionRecord& record,
                                              long long& outId) override {
        Lease lease(pool_);
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
            return DatabaseError::failure(lastError());
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<IntegrationConnectionRecord> findIntegrationConnection(
        long long userId, const std::string& provider) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "UPDATE integration_connections SET token_encrypted = $2, status = $3, last_error = $4 "
                "WHERE id = $1",
                {std::to_string(id), tokenEncrypted, status, lastError}, &res)) {
            return DatabaseError::failure(this->lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    DatabaseError touchIntegrationUsed(long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE integration_connections SET last_used_at = now() WHERE id = $1",
                     {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    DatabaseError deleteIntegrationConnection(long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("DELETE FROM integration_connections WHERE id = $1", {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool deleted = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!deleted) return DatabaseError::failure("подключение не найдено");
        return DatabaseError::success();
    }

    // --------------------------------------------- integration_oauth_states
    DatabaseError createOauthState(const OauthStateRecord& record) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "INSERT INTO integration_oauth_states "
                "(state, user_id, provider, verifier, redirect_uri, expires_at) "
                "VALUES ($1, $2, $3, $4, $5, $6::timestamptz)",
                {record.state, std::to_string(record.userId), record.provider, record.verifier,
                 record.redirectUri, record.expiresAt},
                &res)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(res);
        return DatabaseError::success();
    }

    std::optional<OauthStateRecord> findOauthState(const std::string& state) override {
        Lease lease(pool_);
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
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE integration_oauth_states SET used_at = now() WHERE state = $1 AND used_at IS NULL",
                     {state}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("состояние OAuth не найдено");
        return DatabaseError::success();
    }

    // -------------------------------------------------- auth_oauth
    DatabaseError linkOauthAccount(const UserOauthAccountRecord& record) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "INSERT INTO user_oauth_accounts (user_id, provider, provider_user_id, email) "
                "VALUES ($1, $2, $3, $4)",
                {std::to_string(record.userId), record.provider, record.providerUserId, record.email},
                &res)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(res);
        return DatabaseError::success();
    }

    std::optional<UserOauthAccountRecord> findOauthAccount(const std::string& provider,
                                                           const std::string& providerUserId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT id, user_id, provider, provider_user_id, email, "
                "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM user_oauth_accounts WHERE provider = $1 AND provider_user_id = $2",
                {provider, providerUserId}, &result)) {
            return std::nullopt;
        }
        std::optional<UserOauthAccountRecord> record;
        if (PQntuples(result) > 0) {
            UserOauthAccountRecord value;
            value.id = std::atoll(value_(result, 0, 0).c_str());
            value.userId = std::atoll(value_(result, 0, 1).c_str());
            value.provider = value_(result, 0, 2);
            value.providerUserId = value_(result, 0, 3);
            value.email = value_(result, 0, 4);
            value.createdAt = value_(result, 0, 5);
            record = value;
        }
        PQclear(result);
        return record;
    }

    std::vector<UserOauthAccountRecord> listUserOauthAccounts(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT id, user_id, provider, provider_user_id, email, "
                "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM user_oauth_accounts WHERE user_id = $1 ORDER BY id ASC",
                {std::to_string(userId)}, &result)) {
            return {};
        }
        std::vector<UserOauthAccountRecord> list;
        const int count = PQntuples(result);
        for (int i = 0; i < count; ++i) {
            UserOauthAccountRecord value;
            value.id = std::atoll(value_(result, i, 0).c_str());
            value.userId = std::atoll(value_(result, i, 1).c_str());
            value.provider = value_(result, i, 2);
            value.providerUserId = value_(result, i, 3);
            value.email = value_(result, i, 4);
            value.createdAt = value_(result, i, 5);
            list.push_back(value);
        }
        PQclear(result);
        return list;
    }

    DatabaseError createAuthOauthState(const AuthOauthStateRecord& record) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute(
                "INSERT INTO auth_oauth_states "
                "(state, provider, verifier, redirect_uri, expires_at) "
                "VALUES ($1, $2, $3, $4, $5::timestamptz)",
                {record.state, record.provider, record.verifier, record.redirectUri, record.expiresAt},
                &res)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(res);
        return DatabaseError::success();
    }

    std::optional<AuthOauthStateRecord> findAuthOauthState(const std::string& state) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "SELECT state, provider, verifier, redirect_uri, "
                "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "to_char(used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM auth_oauth_states WHERE state = $1",
                {state}, &result)) {
            return std::nullopt;
        }
        std::optional<AuthOauthStateRecord> record;
        if (PQntuples(result) > 0) {
            AuthOauthStateRecord value;
            value.state = value_(result, 0, 0);
            value.provider = value_(result, 0, 1);
            value.verifier = value_(result, 0, 2);
            value.redirectUri = value_(result, 0, 3);
            value.expiresAt = value_(result, 0, 4);
            value.usedAt = value_(result, 0, 5);
            record = value;
        }
        PQclear(result);
        return record;
    }

    DatabaseError useAuthOauthState(const std::string& state) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE auth_oauth_states SET used_at = now() WHERE state = $1 AND used_at IS NULL",
                     {state}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("состояние OAuth не найдено");
        return DatabaseError::success();
    }

    // ------------------------------------------------------------ notifications
    DatabaseError createNotification(const NotificationRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO notifications (user_id, kind, title, body, payload) "
                "VALUES ($1, $2, $3, $4, $5::jsonb) RETURNING id",
                {std::to_string(record.userId), record.kind, record.title, record.body,
                 record.payload.dump()},
                &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<NotificationRecord> listNotifications(long long userId,
                                                      bool unreadOnly,
                                                      int limit) override {
        Lease lease(pool_);
        std::vector<NotificationRecord> entries;
        PGresult* result = nullptr;
        const std::string query = notificationSelect() + " WHERE user_id = $1" +
                                  (unreadOnly ? " AND read_at IS NULL" : "") +
                                  " ORDER BY id DESC LIMIT $2";
        if (!execute(query, {std::to_string(userId), std::to_string(limit > 0 ? limit : 50)},
                     &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readNotification(result, row));
        PQclear(result);
        return entries;
    }

    long long unreadNotificationCount(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        long long count = 0;
        if (execute("SELECT count(*) FROM notifications WHERE user_id = $1 AND read_at IS NULL",
                    {std::to_string(userId)}, &result) &&
            PQntuples(result) > 0) {
            count = std::atoll(value_(result, 0, 0).c_str());
        }
        PQclear(result);
        return count;
    }

    DatabaseError markNotificationRead(long long userId, long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE notifications SET read_at = now() WHERE id = $1 AND user_id = $2",
                     {std::to_string(id), std::to_string(userId)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("уведомление не найдено");
        return DatabaseError::success();
    }

    DatabaseError markAllNotificationsRead(long long userId) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE notifications SET read_at = now() WHERE user_id = $1 AND read_at IS NULL",
                     {std::to_string(userId)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        PQclear(res);
        return DatabaseError::success();
    }

    // ------------------------------------------------------------- push_devices
    DatabaseError registerPushDevice(long long userId,
                                     const std::string& platform,
                                     const std::string& token,
                                     long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(
                "INSERT INTO push_devices (user_id, platform, token) VALUES ($1, $2, $3) "
                "ON CONFLICT (user_id, token) DO UPDATE SET "
                "platform = EXCLUDED.platform, enabled = TRUE RETURNING id",
                {std::to_string(userId), platform, token}, &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        if (PQntuples(result) > 0) outId = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);
        return DatabaseError::success();
    }

    std::vector<PushDeviceRecord> listPushDevices(long long userId) override {
        Lease lease(pool_);
        std::vector<PushDeviceRecord> entries;
        PGresult* result = nullptr;
        if (!execute(
                "SELECT id, user_id, platform, token, enabled, "
                "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "to_char(last_used_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM push_devices WHERE user_id = $1 ORDER BY id",
                {std::to_string(userId)}, &result)) {
            return entries;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            PushDeviceRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.userId = std::atoll(value_(result, row, 1).c_str());
            record.platform = value_(result, row, 2);
            record.token = value_(result, row, 3);
            record.enabled = value_(result, row, 4) == "t";
            record.createdAt = value_(result, row, 5);
            record.lastUsedAt = value_(result, row, 6);
            entries.push_back(record);
        }
        PQclear(result);
        return entries;
    }

    DatabaseError touchPushDevice(long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("UPDATE push_devices SET last_used_at = now() WHERE id = $1",
                     {std::to_string(id)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool updated = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!updated) return DatabaseError::failure("устройство не найдено");
        return DatabaseError::success();
    }

    DatabaseError deletePushDevice(long long userId, long long id) override {
        Lease lease(pool_);
        PGresult* res = nullptr;
        if (!execute("DELETE FROM push_devices WHERE id = $1 AND user_id = $2",
                     {std::to_string(id), std::to_string(userId)}, &res)) {
            return DatabaseError::failure(lastError());
        }
        const bool deleted = std::atoi(PQcmdTuples(res)) > 0;
        PQclear(res);
        if (!deleted) return DatabaseError::failure("устройство не найдено");
        return DatabaseError::success();
    }

    // ------------------------------------------------------------ admin
    DatabaseError setUserRole(long long userId, const std::string& role) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET role = $2 WHERE id = $1", {std::to_string(userId), role}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    std::vector<UserRecord> adminListUsers(const std::string& query,
                                           const std::string& role,
                                           const std::string& status,
                                           int limit,
                                           int offset,
                                           long long& total) override {
        Lease lease(pool_);
        std::vector<UserRecord> users;
        total = 0;
        // Экранируем LIKE-маски, как в searchUsers.
        std::string escaped;
        for (const char ch : query) {
            if (ch == '\\' || ch == '%' || ch == '_') escaped.push_back('\\');
            escaped.push_back(ch);
        }
        const std::string where =
            "WHERE ($1::text = '' OR lower(email) LIKE lower($2::text) ESCAPE '\\' "
            "OR lower(display_name) LIKE lower($2::text) ESCAPE '\\') "
            "AND ($3::text = '' OR role = $3::text) "
            "AND ($4::text = '' OR ($4::text = 'active' AND is_active) "
            "OR ($4::text = 'disabled' AND NOT is_active) "
            "OR ($4::text = 'unverified' AND NOT email_verified)) ";
        const std::vector<std::string> filters{query, "%" + escaped + "%", role, status};
        PGresult* result = nullptr;
        if (!execute("SELECT count(*) FROM users " + where, filters, &result)) return users;
        total = std::atoll(value_(result, 0, 0).c_str());
        PQclear(result);

        std::vector<std::string> args = filters;
        args.push_back(std::to_string(limit));
        args.push_back(std::to_string(offset));
        if (!execute("SELECT id, email, password_hash, display_name, avatar_url, timezone, is_active, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
                     "to_char(last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), email_verified, bio, role" SELFVOY_PG_USER_GROWTH_COLUMNS
                     "FROM users " + where + "ORDER BY id DESC LIMIT $5 OFFSET $6",
                     args, &result)) {
            return users;
        }
        for (int row = 0; row < PQntuples(result); ++row) users.push_back(readUser(result, row));
        PQclear(result);
        return users;
    }

    Json adminStats() override {
        Lease lease(pool_);
        Json stats = Json::object();
        PGresult* result = nullptr;
        if (!execute("SELECT count(*), "
                     "count(*) FILTER (WHERE email_verified), "
                     "count(*) FILTER (WHERE NOT is_active), "
                     "(SELECT count(*) FROM two_factor_settings WHERE enabled), "
                     "count(*) FILTER (WHERE created_at > now() - interval '7 days'), "
                     "count(*) FILTER (WHERE role = 'moderator'), "
                     "count(*) FILTER (WHERE role = 'admin'), "
                     "(SELECT count(*) FROM sessions WHERE revoked_at IS NULL AND expires_at > now()) "
                     "FROM users",
                     {}, &result)) {
            return stats;
        }
        const char* keys[] = {"users_total", "users_verified", "users_disabled", "users_2fa",
                              "users_new_7d", "moderators", "admins", "sessions_active"};
        for (int column = 0; column < 8; ++column) {
            stats.set(keys[column], Json(std::atoll(value_(result, 0, column).c_str())));
        }
        PQclear(result);
        return stats;
    }

    std::vector<AuditRecord> listAuditPage(long long userId, long long beforeId, int limit) override {
        Lease lease(pool_);
        std::vector<AuditRecord> logs;
        PGresult* result = nullptr;
        if (!execute("SELECT id, COALESCE(user_id, 0), kind, detail::text, remote_addr, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
                     "FROM audit_logs WHERE ($1::bigint = 0 OR user_id = $1::bigint) "
                     "AND ($2::bigint = 0 OR id < $2::bigint) ORDER BY id DESC LIMIT $3",
                     {std::to_string(userId), std::to_string(beforeId), std::to_string(limit)}, &result)) {
            return logs;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            AuditRecord record;
            record.id = std::atoll(value_(result, row, 0).c_str());
            record.userId = std::atoll(value_(result, row, 1).c_str());
            record.kind = value_(result, row, 2);
            const Json detail = Json::parse(value_(result, row, 3));
            record.detail = detail.isNull() ? Json::object() : detail;
            record.remoteAddr = value_(result, row, 4);
            record.createdAt = value_(result, row, 5);
            logs.push_back(record);
        }
        PQclear(result);
        return logs;
    }

    // ------------------------------------------------------- meeting_invites
    DatabaseError createInvite(const InviteRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO meeting_invites (owner_id, token_hash, topic, note, duration_minutes, "
                     "slots, guest_name, expires_at) "
                     "VALUES ($1, $2, $3, $4, $5::int, $6::jsonb, $7, $8::timestamptz) RETURNING id",
                     {std::to_string(record.ownerId), record.tokenHash, record.topic, record.note,
                      std::to_string(record.durationMinutes),
                      record.slots.isArray() ? record.slots.dump() : std::string("[]"), record.guestName,
                      record.expiresAt},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<InviteRecord> findInvite(long long id) override {
        return findInviteWhere("id = $1", std::to_string(id));
    }

    std::optional<InviteRecord> findInviteByToken(const std::string& tokenHash) override {
        if (tokenHash.empty()) return std::nullopt;
        return findInviteWhere("token_hash = $1", tokenHash);
    }

    std::vector<InviteRecord> listInvites(long long ownerId, int limit) override {
        Lease lease(pool_);
        std::vector<InviteRecord> invites;
        PGresult* result = nullptr;
        if (!execute(inviteSelect() + " WHERE owner_id = $1 ORDER BY id DESC LIMIT $2",
                     {std::to_string(ownerId), std::to_string(limit > 0 ? limit : 50)}, &result)) {
            return invites;
        }
        for (int row = 0; row < PQntuples(result); ++row) invites.push_back(readInvite(result, row));
        PQclear(result);
        return invites;
    }

    int countOpenInvites(long long ownerId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT count(*) FROM meeting_invites WHERE owner_id = $1 AND status = 'open' "
                     "AND expires_at > now()",
                     {std::to_string(ownerId)}, &result)) {
            return 0;
        }
        const int count = PQntuples(result) > 0 ? std::atoi(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return count;
    }

    bool closeInvite(long long id, const std::string& status, const InviteRecord& response) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        const bool keepGuest = status == "cancelled";
        if (!execute("UPDATE meeting_invites SET status = $2, responded_at = now(), "
                     "guest_name = CASE WHEN $3::bool THEN guest_name ELSE $4 END, "
                     "guest_email = CASE WHEN $3::bool THEN guest_email ELSE $5 END, "
                     "guest_comment = CASE WHEN $3::bool THEN guest_comment ELSE $6 END, "
                     "chosen = CASE WHEN $3::bool THEN chosen ELSE $7::jsonb END "
                     "WHERE id = $1 AND status = 'open'",
                     {std::to_string(id), status, keepGuest ? "true" : "false", response.guestName,
                      response.guestEmail, response.guestComment,
                      response.chosen.isObject() ? response.chosen.dump() : std::string("{}")},
                     &result)) {
            return false;
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched;
    }

    // ------------------------------------------------------------- ics feed
    std::string icsTokenHash(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT COALESCE(ics_token_hash, '') FROM users WHERE id = $1", {std::to_string(userId)},
                     &result)) {
            return "";
        }
        const std::string hash = PQntuples(result) > 0 ? value_(result, 0, 0) : std::string();
        PQclear(result);
        return hash;
    }

    DatabaseError setIcsTokenHash(long long userId, const std::string& tokenHash) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET ics_token_hash = NULLIF($2, '') WHERE id = $1",
                     {std::to_string(userId), tokenHash}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    long long findUserByIcsToken(const std::string& tokenHash) override {
        Lease lease(pool_);
        if (tokenHash.empty()) return 0;
        PGresult* result = nullptr;
        if (!execute("SELECT id FROM users WHERE ics_token_hash = $1", {tokenHash}, &result)) return 0;
        const long long id = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return id;
    }

private:
    // ----------------------------------------------------------- rate_limits
    bool sharedRateLimits() const override { return true; }

    int rateLimitHit(const std::string& key, int windowSec) override {
        Lease lease(pool_);
        // Истёкшие окна чистим изредка и порциями, чтобы таблица не росла от уникальных
        // ключей: по индексу window_start, без полного прохода таблицы.
        if (++rateHits_ % 1000 == 0) {
            execute("DELETE FROM rate_limits WHERE ctid IN (SELECT ctid FROM rate_limits "
                    "WHERE window_start < now() - interval '1 day' "
                    "AND window_start + make_interval(secs => window_sec) < now() LIMIT 5000)",
                    {}, nullptr);
        }
        PGresult* result = nullptr;
        if (!execute("INSERT INTO rate_limits (key, count, window_start, window_sec) VALUES ($1, 1, now(), $2::int) "
                     "ON CONFLICT (key) DO UPDATE SET "
                     "count = CASE WHEN rate_limits.window_start + make_interval(secs => rate_limits.window_sec) "
                     "< now() THEN 1 ELSE rate_limits.count + 1 END, "
                     "window_start = CASE WHEN rate_limits.window_start + make_interval(secs => rate_limits.window_sec) "
                     "< now() THEN now() ELSE rate_limits.window_start END, "
                     "window_sec = CASE WHEN rate_limits.window_start + make_interval(secs => rate_limits.window_sec) "
                     "< now() THEN EXCLUDED.window_sec ELSE rate_limits.window_sec END "
                     "RETURNING count",
                     {key, std::to_string(windowSec > 0 ? windowSec : 1)}, &result)) {
            // Fail closed: при сбое БД (нет прав на rate_limits, таймаут) защита от
            // перебора паролей и кодов не должна молча отключаться.
            SELFVOY_LOG(log::Level::Error, "db") << "rate_limits недоступна, считаем лимит исчерпанным";
            return kRateLimitFailClosed;
        }
        const int count = PQntuples(result) > 0 ? std::atoi(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return count;
    }

    int rateLimitCount(const std::string& key) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT count FROM rate_limits WHERE key = $1 "
                     "AND window_start + make_interval(secs => window_sec) >= now()",
                     {key}, &result)) {
            SELFVOY_LOG(log::Level::Error, "db") << "rate_limits недоступна, считаем лимит исчерпанным";
            return kRateLimitFailClosed;
        }
        const int count = PQntuples(result) > 0 ? std::atoi(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return count;
    }

    void rateLimitClear(const std::string& key) override {
        Lease lease(pool_);
        execute("DELETE FROM rate_limits WHERE key = $1", {key}, nullptr);
    }

    // Advisory lock на соединении сервера: ключ — пространство Selfvoy (старшие биты) + id владельца.
    // Константу 0x4155 при переименовании не меняем: иначе старый и новый сервер не увидят чужие блокировки.
    static std::string scheduleLockKey(long long ownerId) {
        return std::to_string((0x4155LL << 40) | (ownerId & ((1LL << 40) - 1)));
    }

    // Advisory lock живёт в сессии PostgreSQL: взять и отпустить его нужно на одном
    // и том же соединении — для этого отдельный «пул» из одного соединения.
    bool tryLockSchedule(long long ownerId) override {
        Lease lease(lockPool_);
        PGresult* result = nullptr;
        if (!execute("SELECT pg_try_advisory_lock($1::bigint)", {scheduleLockKey(ownerId)}, &result)) return false;
        const bool locked = PQntuples(result) > 0 && value_(result, 0, 0) == "t";
        PQclear(result);
        return locked;
    }

    void unlockSchedule(long long ownerId) override {
        Lease lease(lockPool_);
        execute("SELECT pg_advisory_unlock($1::bigint)", {scheduleLockKey(ownerId)}, nullptr);
    }

    // --------------------------------------- рост (S1a): состояние, тариф, Telegram
    std::string getState(const std::string& key) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT value FROM server_state WHERE key = $1", {key}, &result)) return "";
        const std::string value = PQntuples(result) > 0 ? value_(result, 0, 0) : std::string();
        PQclear(result);
        return value;
    }

    DatabaseError setState(const std::string& key, const std::string& value) override {
        Lease lease(pool_);
        if (!execute("INSERT INTO server_state (key, value, updated_at) VALUES ($1, $2, now()) "
                     "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now()",
                     {key, value}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    // Ведущий экземпляр: advisory lock живёт в сессии соединения блокировок. Сначала —
    // держим ли мы его уже (pg_locks этой сессии): повторный pg_try_advisory_lock
    // увеличил бы счётчик захватов, и один unlock его бы не отпустил.
    bool tryLeaderLock(long long key) override {
        Lease lease(lockPool_);
        PGresult* result = nullptr;
        if (!execute("SELECT CASE WHEN EXISTS (SELECT 1 FROM pg_locks WHERE locktype = 'advisory' "
                     "AND pid = pg_backend_pid() AND granted AND objsubid = 1 "
                     "AND ((classid::bigint << 32) | objid::bigint) = $1::bigint) "
                     "THEN true ELSE pg_try_advisory_lock($1::bigint) END",
                     {std::to_string(key)}, &result)) {
            return false;
        }
        const bool locked = PQntuples(result) > 0 && value_(result, 0, 0) == "t";
        PQclear(result);
        return locked;
    }

    void releaseLeaderLock(long long key) override {
        Lease lease(lockPool_);
        execute("SELECT pg_advisory_unlock($1::bigint)", {std::to_string(key)}, nullptr);
    }

    DatabaseError setUserPlanDays(long long userId, int days) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET plan_until = CASE WHEN $2::int <= 0 THEN NULL "
                     "ELSE now() + make_interval(days => $2::int) END WHERE id = $1",
                     {std::to_string(userId), std::to_string(days)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    DatabaseError extendUserPlanDays(long long userId, int days) override {
        if (days <= 0) return DatabaseError::failure("срок продления — больше 0 дней");
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE users SET plan_until = greatest(now(), plan_until) + make_interval(days => $2::int) "
                     "WHERE id = $1",
                     {std::to_string(userId), std::to_string(days)}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
    }

    // Отвязка у прежнего аккаунта и привязка к новому — одной транзакцией на одном
    // соединении (уникальный индекс по telegram_chat_id).
    DatabaseError setUserTelegram(long long userId, long long chatId, const std::string& username) override {
        Lease lease(pool_);
        if (chatId == 0) {
            PGresult* result = nullptr;
            if (!execute("UPDATE users SET telegram_chat_id = NULL, telegram_username = '' WHERE id = $1",
                         {std::to_string(userId)}, &result)) {
                return DatabaseError::failure(lastError());
            }
            const bool touched = std::atoi(PQcmdTuples(result)) > 0;
            PQclear(result);
            return touched ? DatabaseError::success() : DatabaseError::failure("пользователь не найден");
        }
        if (!execute("BEGIN", {}, nullptr)) return DatabaseError::failure(lastError());
        PGresult* result = nullptr;
        const bool ok =
            execute("UPDATE users SET telegram_chat_id = NULL, telegram_username = '' "
                    "WHERE telegram_chat_id = $1::bigint AND id <> $2",
                    {std::to_string(chatId), std::to_string(userId)}, nullptr) &&
            execute("UPDATE users SET telegram_chat_id = $2::bigint, telegram_username = $3 WHERE id = $1",
                    {std::to_string(userId), std::to_string(chatId), username}, &result);
        const bool touched = ok && std::atoi(PQcmdTuples(result)) > 0;
        if (result) PQclear(result);
        if (!touched) {
            const std::string message = ok ? std::string("пользователь не найден") : lastError();
            execute("ROLLBACK", {}, nullptr);
            return DatabaseError::failure(message);
        }
        if (!execute("COMMIT", {}, nullptr)) {
            execute("ROLLBACK", {}, nullptr);
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    bool linkBookingTelegram(long long bookingId, long long chatId, int maxChats) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        // Уже привязан — true; иначе вставка, только пока чатов меньше maxChats.
        if (!execute("WITH existing AS (SELECT 1 FROM booking_telegram WHERE booking_id = $1 AND chat_id = $2::bigint), "
                     "inserted AS (INSERT INTO booking_telegram (booking_id, chat_id) "
                     "SELECT $1, $2::bigint WHERE NOT EXISTS (SELECT 1 FROM existing) "
                     "AND (SELECT count(*) FROM booking_telegram WHERE booking_id = $1) < $3::int "
                     "ON CONFLICT DO NOTHING RETURNING 1) "
                     "SELECT (SELECT count(*) FROM existing) + (SELECT count(*) FROM inserted)",
                     {std::to_string(bookingId), std::to_string(chatId), std::to_string(maxChats)}, &result)) {
            return false;
        }
        const bool linked = PQntuples(result) > 0 && std::atoll(value_(result, 0, 0).c_str()) > 0;
        PQclear(result);
        return linked;
    }

    std::vector<long long> bookingTelegramChats(long long bookingId) override {
        Lease lease(pool_);
        std::vector<long long> chats;
        PGresult* result = nullptr;
        if (!execute("SELECT chat_id FROM booking_telegram WHERE booking_id = $1 ORDER BY linked_at",
                     {std::to_string(bookingId)}, &result)) {
            return chats;
        }
        for (int row = 0; row < PQntuples(result); ++row) chats.push_back(std::atoll(value_(result, row, 0).c_str()));
        PQclear(result);
        return chats;
    }

    DatabaseError unlinkTelegramChat(long long chatId) override {
        Lease lease(pool_);
        if (chatId == 0) return DatabaseError::success();
        const std::string chat = std::to_string(chatId);
        if (!execute("UPDATE users SET telegram_chat_id = NULL, telegram_username = '' WHERE telegram_chat_id = $1::bigint",
                     {chat}, nullptr) ||
            !execute("DELETE FROM booking_telegram WHERE chat_id = $1::bigint", {chat}, nullptr) ||
            !execute("UPDATE waitlist_entries SET telegram_chat_id = NULL WHERE telegram_chat_id = $1::bigint", {chat},
                     nullptr) ||
            !execute("DELETE FROM telegram_guest_chats WHERE chat_id = $1::bigint", {chat}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    // ------------------------------------------- F7: ИИ-администратор в Telegram
    DatabaseError saveTelegramGuestChat(const TelegramGuestChatRecord& record) override {
        if (record.chatId == 0 || record.pageId <= 0) return DatabaseError::failure("чат без страницы");
        Lease lease(pool_);
        if (!execute("INSERT INTO telegram_guest_chats (chat_id, page_id, owner_id, guest_name, username, history) "
                     "VALUES ($1::bigint, $2, $3, $4, $5, $6::jsonb) "
                     "ON CONFLICT (chat_id) DO UPDATE SET page_id = EXCLUDED.page_id, owner_id = EXCLUDED.owner_id, "
                     "guest_name = EXCLUDED.guest_name, username = EXCLUDED.username, history = EXCLUDED.history, "
                     "updated_at = now()",
                     {std::to_string(record.chatId), std::to_string(record.pageId), std::to_string(record.ownerId),
                      record.guestName, record.username,
                      record.history.isArray() ? record.history.dump() : std::string("[]")},
                     nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::optional<TelegramGuestChatRecord> findTelegramGuestChat(long long chatId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT chat_id, page_id, owner_id, guest_name, username, history::text, "
                     "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                     "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                     "FROM telegram_guest_chats WHERE chat_id = $1::bigint",
                     {std::to_string(chatId)}, &result)) {
            return std::nullopt;
        }
        std::optional<TelegramGuestChatRecord> record;
        if (PQntuples(result) > 0) {
            TelegramGuestChatRecord row;
            row.chatId = std::atoll(value_(result, 0, 0).c_str());
            row.pageId = std::atoll(value_(result, 0, 1).c_str());
            row.ownerId = std::atoll(value_(result, 0, 2).c_str());
            row.guestName = value_(result, 0, 3);
            row.username = value_(result, 0, 4);
            const Json history = Json::parse(value_(result, 0, 5));
            if (history.isArray()) row.history = history;
            row.createdAt = value_(result, 0, 6);
            row.updatedAt = value_(result, 0, 7);
            record = row;
        }
        PQclear(result);
        return record;
    }

    static std::string questionSelect() {
        return "SELECT id, owner_id, page_id, chat_id, guest_name, username, text, status, answer, "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "COALESCE(to_char(answered_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
               "FROM telegram_questions";
    }

    TelegramQuestionRecord readQuestion(PGresult* result, int row) const {
        TelegramQuestionRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.ownerId = std::atoll(value_(result, row, 1).c_str());
        record.pageId = std::atoll(value_(result, row, 2).c_str());
        record.chatId = std::atoll(value_(result, row, 3).c_str());
        record.guestName = value_(result, row, 4);
        record.username = value_(result, row, 5);
        record.text = value_(result, row, 6);
        record.status = value_(result, row, 7);
        record.answer = value_(result, row, 8);
        record.createdAt = value_(result, row, 9);
        record.answeredAt = value_(result, row, 10);
        return record;
    }

    DatabaseError createTelegramQuestion(const TelegramQuestionRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO telegram_questions (owner_id, page_id, chat_id, guest_name, username, text) "
                     "VALUES ($1, $2, $3::bigint, $4, $5, $6) RETURNING id",
                     {std::to_string(record.ownerId), std::to_string(record.pageId), std::to_string(record.chatId),
                      record.guestName, record.username, record.text},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<TelegramQuestionRecord> findTelegramQuestion(long long id) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(questionSelect() + " WHERE id = $1", {std::to_string(id)}, &result)) return std::nullopt;
        std::optional<TelegramQuestionRecord> record;
        if (PQntuples(result) > 0) record = readQuestion(result, 0);
        PQclear(result);
        return record;
    }

    std::vector<TelegramQuestionRecord> listTelegramQuestions(long long ownerId, const std::string& status,
                                                              int limit) override {
        Lease lease(pool_);
        std::vector<TelegramQuestionRecord> list;
        PGresult* result = nullptr;
        if (!execute(questionSelect() + " WHERE owner_id = $1 AND ($2 = '' OR status = $2) ORDER BY id DESC LIMIT $3",
                     {std::to_string(ownerId), status, std::to_string(limit > 0 ? limit : 50)}, &result)) {
            return list;
        }
        for (int row = 0; row < PQntuples(result); ++row) list.push_back(readQuestion(result, row));
        PQclear(result);
        return list;
    }

    bool answerTelegramQuestion(long long id, const std::string& answer) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE telegram_questions SET status = 'answered', answer = $2, answered_at = now() "
                     "WHERE id = $1",
                     {std::to_string(id), answer}, &result)) {
            return false;
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched;
    }

    // F7: поле страницы ai_admin — отдельным запросом после основной вставки/обновления.
    DatabaseError writePageAiAdmin(long long pageId, bool aiAdmin) {
        if (!execute("UPDATE booking_pages SET ai_admin = $2::bool WHERE id = $1",
                     {std::to_string(pageId), aiAdmin ? "true" : "false"}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    // --------------------------------------------------------- booking_pages
    DatabaseError createBookingPage(const BookingPageRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO booking_pages (owner_id, slug, title, description, duration_minutes, days_ahead, "
                     "max_per_day, notice_hours, max_series, weekends, status, guest_reminders, hide_branding, "
                     "price, currency, prepay, deposit, payment_url, payment_note, pay_within_minutes, cancel_hours, "
                     "late_cancel, policy_text, packages, waitlist, waitlist_hold_minutes, rebook_after_days) "
                     "VALUES ($1, $2, $3, $4, $5::int, $6::int, $7::int, $8::int, $9::int, $10::bool, $11, "
                     "$12::bool, $13::bool, $14::bigint, $15, $16, $17::bigint, $18, $19, $20::int, $21::int, $22, $23, "
                     "$24::jsonb, $25::bool, $26::int, $27::int) "
                     "RETURNING id",
                     {std::to_string(record.ownerId), record.slug, record.title, record.description,
                      std::to_string(record.durationMinutes), std::to_string(record.daysAhead),
                      std::to_string(record.maxPerDay), std::to_string(record.noticeHours),
                      std::to_string(record.maxSeries), record.weekends ? "true" : "false", record.status,
                      record.guestReminders ? "true" : "false", record.hideBranding ? "true" : "false",
                      std::to_string(record.price), record.currency, record.prepay, std::to_string(record.deposit),
                      record.paymentUrl, record.paymentNote, std::to_string(record.payWithinMinutes),
                      std::to_string(record.cancelHours), record.lateCancel, record.policyText,
                      record.packages.isArray() ? record.packages.dump() : std::string("[]"),
                      record.waitlist ? "true" : "false", std::to_string(record.waitlistHoldMinutes),
                      std::to_string(record.rebookAfterDays)},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        if (record.aiAdmin && outId > 0) return writePageAiAdmin(outId, true);
        return DatabaseError::success();
    }

    std::optional<BookingPageRecord> findBookingPage(long long id) override {
        return findBookingPageWhere("id = $1", std::to_string(id));
    }

    std::optional<BookingPageRecord> findBookingPageBySlug(const std::string& slug) override {
        if (slug.empty()) return std::nullopt;
        return findBookingPageWhere("slug = $1", slug);
    }

    std::vector<BookingPageRecord> listBookingPages(long long ownerId) override {
        Lease lease(pool_);
        std::vector<BookingPageRecord> pages;
        PGresult* result = nullptr;
        if (!execute(bookingPageSelect() + " WHERE owner_id = $1 AND status <> 'archived' ORDER BY id DESC",
                     {std::to_string(ownerId)}, &result)) {
            return pages;
        }
        for (int row = 0; row < PQntuples(result); ++row) pages.push_back(readBookingPage(result, row));
        PQclear(result);
        return pages;
    }

    DatabaseError updateBookingPage(const BookingPageRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE booking_pages SET title = $2, description = $3, duration_minutes = $4::int, "
                     "days_ahead = $5::int, max_per_day = $6::int, notice_hours = $7::int, max_series = $8::int, "
                     "weekends = $9::bool, status = $10, guest_reminders = $11::bool, hide_branding = $12::bool, "
                     "price = $13::bigint, currency = $14, prepay = $15, deposit = $16::bigint, payment_url = $17, "
                     "payment_note = $18, pay_within_minutes = $19::int, cancel_hours = $20::int, late_cancel = $21, "
                     "policy_text = $22, packages = $23::jsonb, waitlist = $24::bool, waitlist_hold_minutes = $25::int, "
                     "rebook_after_days = $26::int, updated_at = now() WHERE id = $1",
                     {std::to_string(record.id), record.title, record.description,
                      std::to_string(record.durationMinutes), std::to_string(record.daysAhead),
                      std::to_string(record.maxPerDay), std::to_string(record.noticeHours),
                      std::to_string(record.maxSeries), record.weekends ? "true" : "false", record.status,
                      record.guestReminders ? "true" : "false", record.hideBranding ? "true" : "false",
                      std::to_string(record.price), record.currency, record.prepay, std::to_string(record.deposit),
                      record.paymentUrl, record.paymentNote, std::to_string(record.payWithinMinutes),
                      std::to_string(record.cancelHours), record.lateCancel, record.policyText,
                      record.packages.isArray() ? record.packages.dump() : std::string("[]"),
                      record.waitlist ? "true" : "false", std::to_string(record.waitlistHoldMinutes),
                      std::to_string(record.rebookAfterDays)},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        if (!touched) return DatabaseError::failure("страница записи не найдена");
        return writePageAiAdmin(record.id, record.aiAdmin);
    }

    static std::string bookingPageSelect() {
        return "SELECT id, owner_id, slug, title, description, duration_minutes, days_ahead, max_per_day, "
               "notice_hours, max_series, weekends, status, "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "guest_reminders, hide_branding, price, currency, prepay, deposit, payment_url, payment_note, "
               "pay_within_minutes, cancel_hours, late_cancel, policy_text, packages::text, "
               "waitlist, waitlist_hold_minutes, rebook_after_days, "  // F8: 27–29
               "ai_admin "                                               // F7: 30
               "FROM booking_pages";
    }

    std::optional<BookingPageRecord> findBookingPageWhere(const std::string& condition, const std::string& value) {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(bookingPageSelect() + " WHERE " + condition, {value}, &result)) return std::nullopt;
        std::optional<BookingPageRecord> record;
        if (PQntuples(result) > 0) record = readBookingPage(result, 0);
        PQclear(result);
        return record;
    }

    BookingPageRecord readBookingPage(PGresult* result, int row) const {
        BookingPageRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.ownerId = std::atoll(value_(result, row, 1).c_str());
        record.slug = value_(result, row, 2);
        record.title = value_(result, row, 3);
        record.description = value_(result, row, 4);
        record.durationMinutes = std::atoi(value_(result, row, 5).c_str());
        record.daysAhead = std::atoi(value_(result, row, 6).c_str());
        record.maxPerDay = std::atoi(value_(result, row, 7).c_str());
        record.noticeHours = std::atoi(value_(result, row, 8).c_str());
        record.maxSeries = std::atoi(value_(result, row, 9).c_str());
        record.weekends = value_(result, row, 10) == "t";
        record.status = value_(result, row, 11);
        record.createdAt = value_(result, row, 12);
        record.updatedAt = value_(result, row, 13);
        record.guestReminders = value_(result, row, 14) != "f";
        record.hideBranding = value_(result, row, 15) == "t";
        record.price = std::atoll(value_(result, row, 16).c_str());
        record.currency = value_(result, row, 17);
        record.prepay = value_(result, row, 18);
        record.deposit = std::atoll(value_(result, row, 19).c_str());
        record.paymentUrl = value_(result, row, 20);
        record.paymentNote = value_(result, row, 21);
        record.payWithinMinutes = std::atoi(value_(result, row, 22).c_str());
        record.cancelHours = std::atoi(value_(result, row, 23).c_str());
        record.lateCancel = value_(result, row, 24);
        record.policyText = value_(result, row, 25);
        const Json packages = Json::parse(value_(result, row, 26));
        if (packages.isArray()) record.packages = packages;
        if (PQnfields(result) > 29) {
            record.waitlist = value_(result, row, 27) == "t";
            record.waitlistHoldMinutes = std::atoi(value_(result, row, 28).c_str());
            record.rebookAfterDays = std::atoi(value_(result, row, 29).c_str());
        }
        if (PQnfields(result) > 30) record.aiAdmin = value_(result, row, 30) == "t";  // F7
        return record;
    }

    // ------------------------------------------------------ booking_reminders
    static std::string reminderColumns() {
        return "id, booking_id, owner_id, "
               "to_char(occurrence_start AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), kind, "
               "to_char(due_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "COALESCE(to_char(sent_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')";
    }

    BookingReminderRecord readReminder(PGresult* result, int row) const {
        BookingReminderRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.bookingId = std::atoll(value_(result, row, 1).c_str());
        record.ownerId = std::atoll(value_(result, row, 2).c_str());
        record.occurrenceStart = value_(result, row, 3);
        record.kind = value_(result, row, 4);
        record.dueAt = value_(result, row, 5);
        record.sentAt = value_(result, row, 6);
        return record;
    }

    DatabaseError addBookingReminders(const std::vector<BookingReminderRecord>& reminders) override {
        Lease lease(pool_);
        for (const auto& reminder : reminders) {
            if (!execute("INSERT INTO booking_reminders (booking_id, owner_id, occurrence_start, kind, due_at) "
                         "VALUES ($1, $2, $3::timestamptz, $4, $5::timestamptz) "
                         "ON CONFLICT (booking_id, occurrence_start, kind) DO NOTHING",
                         {std::to_string(reminder.bookingId), std::to_string(reminder.ownerId),
                          reminder.occurrenceStart, reminder.kind, reminder.dueAt},
                         nullptr)) {
                return DatabaseError::failure(lastError());
            }
        }
        return DatabaseError::success();
    }

    DatabaseError deleteBookingReminders(long long bookingId, const std::string& occurrenceStart) override {
        Lease lease(pool_);
        if (!execute("DELETE FROM booking_reminders WHERE booking_id = $1 AND sent_at IS NULL "
                     "AND ($2::text = '' OR occurrence_start = NULLIF($2::text, '')::timestamptz)",
                     {std::to_string(bookingId), occurrenceStart}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::vector<BookingReminderRecord> listBookingReminders(long long bookingId) override {
        Lease lease(pool_);
        std::vector<BookingReminderRecord> reminders;
        PGresult* result = nullptr;
        if (!execute("SELECT " + reminderColumns() + " FROM booking_reminders WHERE booking_id = $1 ORDER BY due_at, id",
                     {std::to_string(bookingId)}, &result)) {
            return reminders;
        }
        for (int row = 0; row < PQntuples(result); ++row) reminders.push_back(readReminder(result, row));
        PQclear(result);
        return reminders;
    }

    // Захват одним UPDATE: строки, уже взятые другим экземпляром, пропускаются (SKIP LOCKED).
    std::vector<BookingReminderRecord> claimDueReminders(const std::string& nowIso, int limit) override {
        Lease lease(pool_);
        std::vector<BookingReminderRecord> reminders;
        PGresult* result = nullptr;
        if (!execute("UPDATE booking_reminders SET sent_at = now() WHERE id IN ("
                     "SELECT id FROM booking_reminders WHERE sent_at IS NULL AND due_at <= $1::timestamptz "
                     "ORDER BY due_at LIMIT $2 FOR UPDATE SKIP LOCKED) RETURNING " + reminderColumns(),
                     {nowIso, std::to_string(limit > 0 ? limit : 100)}, &result)) {
            return reminders;
        }
        for (int row = 0; row < PQntuples(result); ++row) reminders.push_back(readReminder(result, row));
        PQclear(result);
        std::sort(reminders.begin(), reminders.end(),
                  [](const BookingReminderRecord& a, const BookingReminderRecord& b) { return a.dueAt < b.dueAt; });
        return reminders;
    }

    // -------------------------------------------------------------- bookings
    DatabaseError createBooking(const BookingRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO bookings (page_id, owner_id, manage_hash, guest_name, guest_email, guest_comment, "
                     "occurrences, status, cancelled_by, reschedules, ends_at, manage_token_enc, payment, cc_email, "
                     "package_id, price, currency) "
                     "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8, $9, $10::int, NULLIF($11, '')::timestamptz, "
                     "NULLIF($12, ''), NULLIF($13, '')::jsonb, $14, NULLIF($15::bigint, 0), $16::int, $17) "
                     "RETURNING id",
                     {std::to_string(record.pageId), std::to_string(record.ownerId), record.manageHash,
                      record.guestName, record.guestEmail, record.guestComment,
                      record.occurrences.isArray() ? record.occurrences.dump() : std::string("[]"), record.status,
                      record.cancelledBy, std::to_string(record.reschedules), bookingEndsAt(record.occurrences),
                      record.manageTokenEnc, record.payment.isObject() ? record.payment.dump() : std::string(),
                      record.ccEmail, std::to_string(record.packageId), std::to_string(record.price),
                      record.currency.empty() ? std::string("RUB") : record.currency},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<BookingRecord> findBooking(long long id) override {
        return findBookingWhere("id = $1", std::to_string(id));
    }

    std::optional<BookingRecord> findBookingByToken(const std::string& manageHash) override {
        if (manageHash.empty()) return std::nullopt;
        return findBookingWhere("manage_hash = $1", manageHash);
    }

    std::vector<BookingRecord> listBookings(long long ownerId, long long pageId, bool activeOnly,
                                            int limit) override {
        Lease lease(pool_);
        std::vector<BookingRecord> bookings;
        PGresult* result = nullptr;
        if (!execute(bookingSelect() +
                         " WHERE owner_id = $1 AND ($2::bigint = 0 OR page_id = $2::bigint) "
                         "AND (NOT $3::bool OR status = 'active') ORDER BY id DESC LIMIT $4",
                     {std::to_string(ownerId), std::to_string(pageId), activeOnly ? "true" : "false",
                      std::to_string(limit > 0 ? limit : 100)},
                     &result)) {
            return bookings;
        }
        for (int row = 0; row < PQntuples(result); ++row) bookings.push_back(readBooking(result, row));
        PQclear(result);
        return bookings;
    }

    std::vector<BookingRecord> listUpcomingBookings(long long ownerId, long long pageId, int limit) override {
        Lease lease(pool_);
        std::vector<BookingRecord> bookings;
        PGresult* result = nullptr;
        if (!execute(bookingSelect() +
                         " WHERE owner_id = $1 AND ($2::bigint = 0 OR page_id = $2::bigint) "
                         "AND status = 'active' AND ends_at > now() ORDER BY id DESC LIMIT $3",
                     {std::to_string(ownerId), std::to_string(pageId), std::to_string(limit > 0 ? limit : 100)},
                     &result)) {
            return bookings;
        }
        for (int row = 0; row < PQntuples(result); ++row) bookings.push_back(readBooking(result, row));
        PQclear(result);
        return bookings;
    }

    DatabaseError updateBooking(const BookingRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE bookings SET occurrences = $2::jsonb, status = $3, cancelled_by = $4, "
                     "reschedules = $5::int, manage_hash = COALESCE(NULLIF($6, ''), manage_hash), "
                     "ends_at = NULLIF($7, '')::timestamptz, "
                     "manage_token_enc = COALESCE(NULLIF($8, ''), manage_token_enc), "
                     "payment = NULLIF($9, '')::jsonb, updated_at = now() WHERE id = $1",
                     {std::to_string(record.id),
                      record.occurrences.isArray() ? record.occurrences.dump() : std::string("[]"), record.status,
                      record.cancelledBy, std::to_string(record.reschedules), record.manageHash,
                      bookingEndsAt(record.occurrences), record.manageTokenEnc,
                      record.payment.isObject() ? record.payment.dump() : std::string()},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("запись не найдена");
    }

    static std::string bookingSelect() {
        return "SELECT id, page_id, owner_id, manage_hash, guest_name, guest_email, guest_comment, "
               "occurrences::text, status, cancelled_by, reschedules, "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "COALESCE(manage_token_enc, ''), COALESCE(payment::text, ''), cc_email, COALESCE(package_id, 0), "
               "price, currency, "
               "COALESCE(to_char(rebook_sent_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
               "rebook_status "
               "FROM bookings";
    }

    std::vector<BookingRecord> listAwaitingPayments(int limit) override {
        Lease lease(pool_);
        std::vector<BookingRecord> bookings;
        PGresult* result = nullptr;
        if (!execute(bookingSelect() + " WHERE payment->>'status' = 'pending' ORDER BY payment->>'due_at', id LIMIT $1",
                     {std::to_string(limit > 0 ? limit : 100)}, &result)) {
            return bookings;
        }
        for (int row = 0; row < PQntuples(result); ++row) bookings.push_back(readBooking(result, row));
        PQclear(result);
        return bookings;
    }

    // ------------------------------------------------------- payment_accounts
    std::optional<PaymentAccountRecord> findPaymentAccount(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT user_id, provider, shop_id, secret_enc, "
                     "to_char(connected_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                     "FROM payment_accounts WHERE user_id = $1",
                     {std::to_string(userId)}, &result)) {
            return std::nullopt;
        }
        std::optional<PaymentAccountRecord> record;
        if (PQntuples(result) > 0) {
            PaymentAccountRecord item;
            item.userId = std::atoll(value_(result, 0, 0).c_str());
            item.provider = value_(result, 0, 1);
            item.shopId = value_(result, 0, 2);
            item.secretEnc = value_(result, 0, 3);
            item.connectedAt = value_(result, 0, 4);
            record = item;
        }
        PQclear(result);
        return record;
    }

    DatabaseError setPaymentAccount(const PaymentAccountRecord& record) override {
        Lease lease(pool_);
        if (!execute("INSERT INTO payment_accounts (user_id, provider, shop_id, secret_enc) VALUES ($1, $2, $3, $4) "
                     "ON CONFLICT (user_id) DO UPDATE SET provider = EXCLUDED.provider, shop_id = EXCLUDED.shop_id, "
                     "secret_enc = EXCLUDED.secret_enc, connected_at = now()",
                     {std::to_string(record.userId), record.provider, record.shopId, record.secretEnc}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    DatabaseError deletePaymentAccount(long long userId) override {
        Lease lease(pool_);
        if (!execute("DELETE FROM payment_accounts WHERE user_id = $1", {std::to_string(userId)}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::optional<BookingRecord> findBookingWhere(const std::string& condition, const std::string& value) {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(bookingSelect() + " WHERE " + condition, {value}, &result)) return std::nullopt;
        std::optional<BookingRecord> record;
        if (PQntuples(result) > 0) record = readBooking(result, 0);
        PQclear(result);
        return record;
    }

    BookingRecord readBooking(PGresult* result, int row) const {
        BookingRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.pageId = std::atoll(value_(result, row, 1).c_str());
        record.ownerId = std::atoll(value_(result, row, 2).c_str());
        record.manageHash = value_(result, row, 3);
        record.guestName = value_(result, row, 4);
        record.guestEmail = value_(result, row, 5);
        record.guestComment = value_(result, row, 6);
        const Json occurrences = Json::parse(value_(result, row, 7));
        record.occurrences = occurrences.isArray() ? occurrences : Json::array();
        record.status = value_(result, row, 8);
        record.cancelledBy = value_(result, row, 9);
        record.reschedules = std::atoi(value_(result, row, 10).c_str());
        record.createdAt = value_(result, row, 11);
        record.updatedAt = value_(result, row, 12);
        if (PQnfields(result) > 13) record.manageTokenEnc = value_(result, row, 13);
        if (PQnfields(result) > 14 && !value_(result, row, 14).empty()) {
            const Json payment = Json::parse(value_(result, row, 14));
            if (payment.isObject()) record.payment = payment;
        }
        if (PQnfields(result) > 16) {
            record.ccEmail = value_(result, row, 15);
            record.packageId = std::atoll(value_(result, row, 16).c_str());
        }
        if (PQnfields(result) > 18) {
            record.price = std::atoll(value_(result, row, 17).c_str());
            const std::string currency = value_(result, row, 18);
            if (!currency.empty()) record.currency = currency;
        }
        if (PQnfields(result) > 20) {
            record.rebookSentAt = value_(result, row, 19);
            record.rebookStatus = value_(result, row, 20);
        }
        return record;
    }

    std::vector<BookingRecord> listBookingsByPackage(long long packageId, int limit) override {
        Lease lease(pool_);
        std::vector<BookingRecord> bookings;
        PGresult* result = nullptr;
        if (packageId <= 0 ||
            !execute(bookingSelect() + " WHERE package_id = $1 ORDER BY id LIMIT $2",
                     {std::to_string(packageId), std::to_string(limit > 0 ? limit : 100)}, &result)) {
            return bookings;
        }
        for (int row = 0; row < PQntuples(result); ++row) bookings.push_back(readBooking(result, row));
        PQclear(result);
        return bookings;
    }

    // ------------------------------------------------------ package_purchases
    static std::string packageSelect() {
        return "SELECT id, owner_id, page_id, offer_id, title, sessions_total, sessions_used, valid_days, price, "
               "currency, COALESCE(to_char(valid_until AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
               "status, COALESCE(payment::text, ''), guest_name, guest_email, cc_email, token_hash, "
               "COALESCE(token_enc, ''), "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
               "FROM package_purchases";
    }

    PackagePurchaseRecord readPackage(PGresult* result, int row) const {
        PackagePurchaseRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.ownerId = std::atoll(value_(result, row, 1).c_str());
        record.pageId = std::atoll(value_(result, row, 2).c_str());
        record.offerId = std::atoll(value_(result, row, 3).c_str());
        record.title = value_(result, row, 4);
        record.sessionsTotal = std::atoi(value_(result, row, 5).c_str());
        record.sessionsUsed = std::atoi(value_(result, row, 6).c_str());
        record.validDays = std::atoi(value_(result, row, 7).c_str());
        record.price = std::atoll(value_(result, row, 8).c_str());
        record.currency = value_(result, row, 9);
        record.validUntil = value_(result, row, 10);
        record.status = value_(result, row, 11);
        if (!value_(result, row, 12).empty()) {
            const Json payment = Json::parse(value_(result, row, 12));
            if (payment.isObject()) record.payment = payment;
        }
        record.guestName = value_(result, row, 13);
        record.guestEmail = value_(result, row, 14);
        record.ccEmail = value_(result, row, 15);
        record.tokenHash = value_(result, row, 16);
        record.tokenEnc = value_(result, row, 17);
        record.createdAt = value_(result, row, 18);
        record.updatedAt = value_(result, row, 19);
        return record;
    }

    std::vector<PackagePurchaseRecord> packagesWhere(const std::string& tail, const std::vector<std::string>& args) {
        Lease lease(pool_);
        std::vector<PackagePurchaseRecord> packages;
        PGresult* result = nullptr;
        if (!execute(packageSelect() + tail, args, &result)) return packages;
        for (int row = 0; row < PQntuples(result); ++row) packages.push_back(readPackage(result, row));
        PQclear(result);
        return packages;
    }

    DatabaseError createPackagePurchase(const PackagePurchaseRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO package_purchases (owner_id, page_id, offer_id, title, sessions_total, sessions_used, "
                     "valid_days, price, currency, valid_until, status, payment, guest_name, guest_email, cc_email, "
                     "token_hash, token_enc) "
                     "VALUES ($1, $2, $3::bigint, $4, $5::int, $6::int, $7::int, $8::bigint, $9, "
                     "NULLIF($10, '')::timestamptz, $11, NULLIF($12, '')::jsonb, $13, $14, $15, $16, NULLIF($17, '')) "
                     "RETURNING id",
                     {std::to_string(record.ownerId), std::to_string(record.pageId), std::to_string(record.offerId),
                      record.title, std::to_string(record.sessionsTotal), std::to_string(record.sessionsUsed),
                      std::to_string(record.validDays), std::to_string(record.price), record.currency,
                      record.validUntil, record.status,
                      record.payment.isObject() ? record.payment.dump() : std::string(), record.guestName,
                      record.guestEmail, record.ccEmail, record.tokenHash, record.tokenEnc},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<PackagePurchaseRecord> findPackagePurchase(long long id) override {
        auto found = packagesWhere(" WHERE id = $1", {std::to_string(id)});
        if (found.empty()) return std::nullopt;
        return found.front();
    }

    std::optional<PackagePurchaseRecord> findPackagePurchaseByToken(const std::string& tokenHash) override {
        if (tokenHash.empty()) return std::nullopt;
        auto found = packagesWhere(" WHERE token_hash = $1", {tokenHash});
        if (found.empty()) return std::nullopt;
        return found.front();
    }

    std::vector<PackagePurchaseRecord> listPackagePurchases(long long ownerId, long long pageId,
                                                           const std::string& status, int limit) override {
        return packagesWhere(" WHERE owner_id = $1 AND ($2::bigint = 0 OR page_id = $2::bigint) "
                             "AND ($3::text = '' OR status = $3::text) ORDER BY id DESC LIMIT $4",
                             {std::to_string(ownerId), std::to_string(pageId), status,
                              std::to_string(limit > 0 ? limit : 100)});
    }

    DatabaseError updatePackagePurchase(const PackagePurchaseRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE package_purchases SET sessions_total = $2::int, sessions_used = $3::int, "
                     "valid_until = NULLIF($4, '')::timestamptz, status = $5, payment = NULLIF($6, '')::jsonb, "
                     "updated_at = now() WHERE id = $1",
                     {std::to_string(record.id), std::to_string(record.sessionsTotal),
                      std::to_string(record.sessionsUsed), record.validUntil, record.status,
                      record.payment.isObject() ? record.payment.dump() : std::string()},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("пакет не найден");
    }

    std::vector<PackagePurchaseRecord> listPendingPackagePurchases(int limit) override {
        return packagesWhere(" WHERE status = 'pending' ORDER BY payment->>'due_at', id LIMIT $1",
                             {std::to_string(limit > 0 ? limit : 100)});
    }

    std::vector<PackagePurchaseRecord> listExpiredPackagePurchases(const std::string& nowIso, int limit) override {
        return packagesWhere(" WHERE status = 'active' AND valid_until <= $1::timestamptz ORDER BY valid_until, id "
                             "LIMIT $2",
                             {nowIso, std::to_string(limit > 0 ? limit : 100)});
    }

    // ------------------------------------------------------ waitlist_entries (F8)
    static std::string waitlistSelect() {
        const std::string iso = "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'";
        return "SELECT id, owner_id, page_id, guest_name, guest_email, days::text, part_of_day, comment, status, "
               "token_hash, COALESCE(token_enc, ''), COALESCE(telegram_chat_id, 0), "
               "COALESCE(to_char(offer_start AT TIME ZONE 'UTC', " + iso + "), ''), "
               "COALESCE(to_char(offer_end AT TIME ZONE 'UTC', " + iso + "), ''), offer_day, offer_local, "
               "COALESCE(to_char(offer_expires_at AT TIME ZONE 'UTC', " + iso + "), ''), offer_status, "
               "COALESCE(booking_id, 0), "
               "to_char(queued_at AT TIME ZONE 'UTC', " + iso + "), "
               "to_char(expires_at AT TIME ZONE 'UTC', " + iso + "), "
               "to_char(created_at AT TIME ZONE 'UTC', " + iso + "), "
               "to_char(updated_at AT TIME ZONE 'UTC', " + iso + ") "
               "FROM waitlist_entries";
    }

    WaitlistEntryRecord readWaitlist(PGresult* result, int row) const {
        WaitlistEntryRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.ownerId = std::atoll(value_(result, row, 1).c_str());
        record.pageId = std::atoll(value_(result, row, 2).c_str());
        record.guestName = value_(result, row, 3);
        record.guestEmail = value_(result, row, 4);
        const Json days = Json::parse(value_(result, row, 5));
        record.days = days.isArray() ? days : Json::array();
        record.partOfDay = value_(result, row, 6);
        record.comment = value_(result, row, 7);
        record.status = value_(result, row, 8);
        record.tokenHash = value_(result, row, 9);
        record.tokenEnc = value_(result, row, 10);
        record.telegramChatId = std::atoll(value_(result, row, 11).c_str());
        record.offerStart = value_(result, row, 12);
        record.offerEnd = value_(result, row, 13);
        record.offerDay = value_(result, row, 14);
        record.offerLocal = value_(result, row, 15);
        record.offerExpiresAt = value_(result, row, 16);
        record.offerStatus = value_(result, row, 17);
        record.bookingId = std::atoll(value_(result, row, 18).c_str());
        record.queuedAt = value_(result, row, 19);
        record.expiresAt = value_(result, row, 20);
        record.createdAt = value_(result, row, 21);
        record.updatedAt = value_(result, row, 22);
        return record;
    }

    std::vector<WaitlistEntryRecord> waitlistWhere(const std::string& tail, const std::vector<std::string>& args) {
        Lease lease(pool_);
        std::vector<WaitlistEntryRecord> entries;
        PGresult* result = nullptr;
        if (!execute(waitlistSelect() + tail, args, &result)) return entries;
        for (int row = 0; row < PQntuples(result); ++row) entries.push_back(readWaitlist(result, row));
        PQclear(result);
        return entries;
    }

    DatabaseError createWaitlistEntry(const WaitlistEntryRecord& record, long long& outId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO waitlist_entries (owner_id, page_id, guest_name, guest_email, days, part_of_day, "
                     "comment, status, token_hash, token_enc, queued_at, expires_at) "
                     "VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7, $8, $9, NULLIF($10, ''), "
                     "COALESCE(NULLIF($11, '')::timestamptz, now()), $12::timestamptz) RETURNING id",
                     {std::to_string(record.ownerId), std::to_string(record.pageId), record.guestName,
                      record.guestEmail, record.days.isArray() ? record.days.dump() : std::string("[]"),
                      record.partOfDay, record.comment, record.status, record.tokenHash, record.tokenEnc,
                      record.queuedAt, record.expiresAt},
                     &result)) {
            PQclear(result);
            return DatabaseError::failure(lastError());
        }
        outId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return DatabaseError::success();
    }

    std::optional<WaitlistEntryRecord> findWaitlistEntry(long long id) override {
        auto found = waitlistWhere(" WHERE id = $1", {std::to_string(id)});
        if (found.empty()) return std::nullopt;
        return found.front();
    }

    std::optional<WaitlistEntryRecord> findWaitlistEntryByToken(const std::string& tokenHash) override {
        if (tokenHash.empty()) return std::nullopt;
        auto found = waitlistWhere(" WHERE token_hash = $1", {tokenHash});
        if (found.empty()) return std::nullopt;
        return found.front();
    }

    std::optional<WaitlistEntryRecord> findActiveWaitlistEntry(long long pageId, const std::string& emailLower) override {
        auto found = waitlistWhere(" WHERE page_id = $1 AND lower(guest_email) = $2 AND status IN ('waiting', 'offered') "
                                   "ORDER BY id DESC LIMIT 1",
                                   {std::to_string(pageId), emailLower});
        if (found.empty()) return std::nullopt;
        return found.front();
    }

    std::vector<WaitlistEntryRecord> listWaitlistEntries(long long ownerId, long long pageId, const std::string& status,
                                                         int limit) override {
        return waitlistWhere(" WHERE owner_id = $1 AND ($2::bigint = 0 OR page_id = $2::bigint) "
                             "AND ($3::text = '' OR status = $3::text) ORDER BY id DESC LIMIT $4",
                             {std::to_string(ownerId), std::to_string(pageId), status,
                              std::to_string(limit > 0 ? limit : 100)});
    }

    std::vector<WaitlistEntryRecord> listWaitingEntries(long long pageId, const std::string& nowIso, int limit) override {
        return waitlistWhere(" WHERE page_id = $1 AND status = 'waiting' AND expires_at > $2::timestamptz "
                             "ORDER BY queued_at, id LIMIT $3",
                             {std::to_string(pageId), nowIso, std::to_string(limit > 0 ? limit : 100)});
    }

    int countActiveWaitlistEntries(long long pageId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT count(*) FROM waitlist_entries WHERE page_id = $1 AND status IN ('waiting', 'offered')",
                     {std::to_string(pageId)}, &result)) {
            return 0;
        }
        const int count = PQntuples(result) > 0 ? std::atoi(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return count;
    }

    DatabaseError updateWaitlistEntry(const WaitlistEntryRecord& record) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE waitlist_entries SET guest_name = $2, days = $3::jsonb, part_of_day = $4, comment = $5, "
                     "status = $6, offer_start = NULLIF($7, '')::timestamptz, offer_end = NULLIF($8, '')::timestamptz, "
                     "offer_day = $9, offer_local = $10, offer_expires_at = NULLIF($11, '')::timestamptz, "
                     "offer_status = $12, booking_id = NULLIF($13::bigint, 0), "
                     "queued_at = COALESCE(NULLIF($14, '')::timestamptz, queued_at), "
                     "expires_at = COALESCE(NULLIF($15, '')::timestamptz, expires_at), updated_at = now() "
                     "WHERE id = $1",
                     {std::to_string(record.id), record.guestName,
                      record.days.isArray() ? record.days.dump() : std::string("[]"), record.partOfDay,
                      record.comment, record.status, record.offerStart, record.offerEnd, record.offerDay,
                      record.offerLocal, record.offerExpiresAt, record.offerStatus, std::to_string(record.bookingId),
                      record.queuedAt, record.expiresAt},
                     &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool touched = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return touched ? DatabaseError::success() : DatabaseError::failure("запись листа ожидания не найдена");
    }

    DatabaseError setWaitlistTelegram(long long entryId, long long chatId) override {
        Lease lease(pool_);
        if (!execute("UPDATE waitlist_entries SET telegram_chat_id = NULLIF($2::bigint, 0) WHERE id = $1",
                     {std::to_string(entryId), std::to_string(chatId)}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    std::vector<WaitlistEntryRecord> listWaitlistHolds(long long ownerId, const std::string& nowIso) override {
        return waitlistWhere(" WHERE owner_id = $1 AND status = 'offered' AND offer_expires_at > $2::timestamptz "
                             "AND offer_start IS NOT NULL AND offer_end IS NOT NULL ORDER BY offer_start LIMIT 1000",
                             {std::to_string(ownerId), nowIso});
    }

    std::vector<WaitlistEntryRecord> listDueWaitlist(const std::string& nowIso, int limit) override {
        return waitlistWhere(" WHERE (status = 'offered' AND offer_expires_at <= $1::timestamptz) "
                             "OR (status = 'waiting' AND expires_at <= $1::timestamptz) "
                             "ORDER BY COALESCE(offer_expires_at, expires_at), id LIMIT $2",
                             {nowIso, std::to_string(limit > 0 ? limit : 100)});
    }

    // ------------------------------------------------------- повторный приём (F8)
    // Захват одним UPDATE: строки, уже взятые другим экземпляром, пропускаются (SKIP LOCKED).
    std::vector<BookingRecord> claimDueRebooks(const std::string& nowIso, int limit) override {
        std::vector<std::string> ids;
        {
            Lease lease(pool_);
            PGresult* result = nullptr;
            if (!execute("UPDATE bookings SET rebook_sent_at = $1::timestamptz, rebook_status = 'claimed' "
                         "WHERE id IN (SELECT b.id FROM bookings b JOIN booking_pages p ON p.id = b.page_id "
                         "WHERE b.status = 'active' AND b.rebook_sent_at IS NULL AND b.ends_at IS NOT NULL "
                         "AND p.rebook_after_days > 0 "
                         "AND b.ends_at + make_interval(days => p.rebook_after_days) <= $1::timestamptz "
                         "ORDER BY b.ends_at LIMIT $2 FOR UPDATE OF b SKIP LOCKED) RETURNING id",
                         {nowIso, std::to_string(limit > 0 ? limit : 100)}, &result)) {
                return {};
            }
            for (int row = 0; row < PQntuples(result); ++row) ids.push_back(value_(result, row, 0));
            PQclear(result);
        }
        std::vector<BookingRecord> bookings;
        for (const auto& id : ids) {
            if (auto booking = findBookingWhere("id = $1", id)) bookings.push_back(std::move(*booking));
        }
        std::sort(bookings.begin(), bookings.end(), [](const BookingRecord& a, const BookingRecord& b) {
            return bookingEndsAt(a.occurrences) < bookingEndsAt(b.occurrences);
        });
        return bookings;
    }

    DatabaseError setRebookStatus(long long bookingId, const std::string& status) override {
        Lease lease(pool_);
        if (!execute("UPDATE bookings SET rebook_status = $2 WHERE id = $1", {std::to_string(bookingId), status},
                     nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    DatabaseError cancelPendingRebooks(long long pageId, const std::string& nowIso) override {
        Lease lease(pool_);
        if (!execute("UPDATE bookings SET rebook_sent_at = $2::timestamptz, rebook_status = 'cancelled' "
                     "WHERE page_id = $1 AND status = 'active' AND rebook_sent_at IS NULL AND ends_at <= $2::timestamptz",
                     {std::to_string(pageId), nowIso}, nullptr)) {
            return DatabaseError::failure(lastError());
        }
        return DatabaseError::success();
    }

    bool hasLaterBooking(long long pageId, const std::string& emailLower, long long exceptBookingId,
                         const std::string& endsAtIso) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (emailLower.empty() ||
            !execute("SELECT 1 FROM bookings WHERE page_id = $1 AND lower(guest_email) = $2 AND id <> $3 "
                     "AND status = 'active' AND ends_at > $4::timestamptz LIMIT 1",
                     {std::to_string(pageId), emailLower, std::to_string(exceptBookingId), endsAtIso}, &result)) {
            return false;
        }
        const bool found = PQntuples(result) > 0;
        PQclear(result);
        return found;
    }

    // ----------------------------------------------------------- чеки (F10)
    std::vector<BookingRecord> listReceiptPendingBookings(long long ownerId, int limit) override {
        Lease lease(pool_);
        std::vector<BookingRecord> bookings;
        PGresult* result = nullptr;
        if (!execute(bookingSelect() +
                         " WHERE owner_id = $1 AND payment->>'npd' = 'true' AND payment->>'status' = 'paid' "
                         "AND COALESCE(payment->>'receipt_at', '') = '' ORDER BY id DESC LIMIT $2",
                     {std::to_string(ownerId), std::to_string(limit > 0 ? limit : 100)}, &result)) {
            return bookings;
        }
        for (int row = 0; row < PQntuples(result); ++row) bookings.push_back(readBooking(result, row));
        PQclear(result);
        return bookings;
    }

    std::vector<PackagePurchaseRecord> listReceiptPendingPackages(long long ownerId, int limit) override {
        return packagesWhere(" WHERE owner_id = $1 AND payment->>'npd' = 'true' AND payment->>'status' = 'paid' "
                             "AND COALESCE(payment->>'receipt_at', '') = '' ORDER BY id DESC LIMIT $2",
                             {std::to_string(ownerId), std::to_string(limit > 0 ? limit : 100)});
    }

    // ------------------------------------------------------------- referrals
    std::string referralCode(long long userId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("SELECT code FROM referral_codes WHERE user_id = $1", {std::to_string(userId)}, &result)) {
            return "";
        }
        const std::string code = PQntuples(result) > 0 ? value_(result, 0, 0) : std::string();
        PQclear(result);
        return code;
    }

    bool setReferralCode(long long userId, const std::string& code) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("INSERT INTO referral_codes (user_id, code) VALUES ($1, $2) ON CONFLICT DO NOTHING",
                     {std::to_string(userId), code}, &result)) {
            return false;
        }
        const bool inserted = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return inserted;
    }

    long long findReferrer(const std::string& code) override {
        Lease lease(pool_);
        if (code.empty()) return 0;
        PGresult* result = nullptr;
        if (!execute("SELECT user_id FROM referral_codes WHERE code = $1", {code}, &result)) return 0;
        const long long id = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return id;
    }

    DatabaseError createReferral(long long referrerId, long long referredId, const std::string& source) override {
        Lease lease(pool_);
        if (referrerId == referredId) return DatabaseError::failure("нельзя пригласить себя");
        PGresult* result = nullptr;
        if (!execute("INSERT INTO referrals (referrer_id, referred_id, source) VALUES ($1, $2, $3) "
                     "ON CONFLICT (referred_id) DO NOTHING",
                     {std::to_string(referrerId), std::to_string(referredId), source}, &result)) {
            return DatabaseError::failure(lastError());
        }
        const bool inserted = std::atoi(PQcmdTuples(result)) > 0;
        PQclear(result);
        return inserted ? DatabaseError::success() : DatabaseError::failure("уже приглашён");
    }

    long long markReferralVerified(long long referredId) override {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute("UPDATE referrals SET verified_at = now() WHERE referred_id = $1 AND verified_at IS NULL "
                     "RETURNING referrer_id",
                     {std::to_string(referredId)}, &result)) {
            return 0;
        }
        const long long id = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        return id;
    }

    // Одна транзакция: строка referrals и оба пользователя заблокированы (пользователи —
    // по возрастанию id, без взаимной блокировки), счётчик наград пригласившего
    // читается под блокировкой его строки — два подтверждения не превысят cap.
    long long rewardReferral(long long referredId, int days, int cap, bool& referrerRewarded) override {
        referrerRewarded = false;
        if (days <= 0) return 0;
        Lease lease(pool_);
        if (!execute("BEGIN", {}, nullptr)) return 0;
        const auto fail = [this]() {
            execute("ROLLBACK", {}, nullptr);
            return 0LL;
        };
        PGresult* result = nullptr;
        if (!execute("SELECT referrer_id FROM referrals WHERE referred_id = $1 AND verified_at IS NOT NULL "
                     "AND rewarded_at IS NULL FOR UPDATE",
                     {std::to_string(referredId)}, &result)) {
            return fail();
        }
        const long long referrerId = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        if (referrerId <= 0) return fail();
        const std::string referrer = std::to_string(referrerId);
        const std::string referred = std::to_string(referredId);
        if (!execute("SELECT id FROM users WHERE id IN ($1, $2) ORDER BY id FOR UPDATE", {referrer, referred},
                     nullptr)) {
            return fail();
        }
        result = nullptr;
        if (!execute("SELECT count(*) FROM referrals WHERE referrer_id = $1 AND rewarded_at IS NOT NULL", {referrer},
                     &result)) {
            return fail();
        }
        const long long rewarded = PQntuples(result) > 0 ? std::atoll(value_(result, 0, 0).c_str()) : 0;
        PQclear(result);
        const std::string extend =
            "UPDATE users SET plan_until = greatest(now(), plan_until) + make_interval(days => $2::int) WHERE id = $1";
        if (!execute("UPDATE referrals SET rewarded_at = now() WHERE referred_id = $1", {referred}, nullptr) ||
            !execute(extend, {referred, std::to_string(days)}, nullptr)) {
            return fail();
        }
        const bool toReferrer = rewarded < cap;
        if (toReferrer && !execute(extend, {referrer, std::to_string(days)}, nullptr)) return fail();
        if (!execute("COMMIT", {}, nullptr)) return fail();
        referrerRewarded = toReferrer;
        return referrerId;
    }

    std::vector<ReferralRecord> listReferrals(long long referrerId, int limit) override {
        Lease lease(pool_);
        std::vector<ReferralRecord> referrals;
        PGresult* result = nullptr;
        if (!execute("SELECT r.referred_id, u.display_name, r.source, "
                     "to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                     "to_char(r.verified_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                     "to_char(r.rewarded_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                     "FROM referrals r JOIN users u ON u.id = r.referred_id "
                     "WHERE r.referrer_id = $1 ORDER BY r.id DESC LIMIT $2",
                     {std::to_string(referrerId), std::to_string(limit > 0 ? limit : 50)}, &result)) {
            return referrals;
        }
        for (int row = 0; row < PQntuples(result); ++row) {
            ReferralRecord record;
            record.referrerId = referrerId;
            record.referredId = std::atoll(value_(result, row, 0).c_str());
            record.displayName = value_(result, row, 1);
            record.source = value_(result, row, 2);
            record.createdAt = value_(result, row, 3);
            record.verifiedAt = value_(result, row, 4);
            record.rewardedAt = value_(result, row, 5);
            referrals.push_back(record);
        }
        PQclear(result);
        return referrals;
    }

    static std::string inviteSelect() {
        return "SELECT id, owner_id, token_hash, topic, note, duration_minutes, slots::text, status, "
               "guest_name, guest_email, guest_comment, chosen::text, "
               "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(responded_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
               "FROM meeting_invites";
    }

    std::optional<InviteRecord> findInviteWhere(const std::string& condition, const std::string& value) {
        Lease lease(pool_);
        PGresult* result = nullptr;
        if (!execute(inviteSelect() + " WHERE " + condition, {value}, &result)) return std::nullopt;
        std::optional<InviteRecord> record;
        if (PQntuples(result) > 0) record = readInvite(result, 0);
        PQclear(result);
        return record;
    }

    InviteRecord readInvite(PGresult* result, int row) const {
        InviteRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.ownerId = std::atoll(value_(result, row, 1).c_str());
        record.tokenHash = value_(result, row, 2);
        record.topic = value_(result, row, 3);
        record.note = value_(result, row, 4);
        record.durationMinutes = std::atoi(value_(result, row, 5).c_str());
        const Json slots = Json::parse(value_(result, row, 6));
        record.slots = slots.isArray() ? slots : Json::array();
        record.status = value_(result, row, 7);
        record.guestName = value_(result, row, 8);
        record.guestEmail = value_(result, row, 9);
        record.guestComment = value_(result, row, 10);
        const Json chosen = Json::parse(value_(result, row, 11));
        record.chosen = chosen.isObject() ? chosen : Json::object();
        record.expiresAt = value_(result, row, 12);
        record.createdAt = value_(result, row, 13);
        record.respondedAt = value_(result, row, 14);
        return record;
    }

    static std::string notificationSelect() {
        return "SELECT id, user_id, kind, title, body, payload::text, "
               "to_char(read_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
               "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
               "FROM notifications";
    }

    NotificationRecord readNotification(PGresult* result, int row) const {
        NotificationRecord record;
        record.id = std::atoll(value_(result, row, 0).c_str());
        record.userId = std::atoll(value_(result, row, 1).c_str());
        record.kind = value_(result, row, 2);
        record.title = value_(result, row, 3);
        record.body = value_(result, row, 4);
        record.payload = Json::parse(value_(result, row, 5), nullptr);
        record.readAt = value_(result, row, 6);
        record.createdAt = value_(result, row, 7);
        return record;
    }

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
        record.bio = value_(result, row, 10);
        if (PQnfields(result) > 11) record.role = value_(result, row, 11);
        if (record.role.empty()) record.role = "user";
        if (PQnfields(result) > 14) {
            record.planUntil = value_(result, row, 12);
            record.telegramChatId = std::atoll(value_(result, row, 13).c_str());
            record.telegramUsername = value_(result, row, 14);
        }
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
    // Параметры сессии после (пере)подключения.
    bool prepareSession() const {
        PGresult* result = PQexec(tlsConn->pg, "SET TIME ZONE 'UTC'");
        const bool ok = PQresultStatus(result) == PGRES_COMMAND_OK;
        PQclear(result);
        return ok;
    }

    // Соединение потеряно (рестарт PostgreSQL, обрыв сети) — переподключаемся.
    // Без этого сервер после перезапуска БД отвечал ошибками до своего рестарта.
    bool ensureConnected() const {
        PGconn*& connection = tlsConn->pg;
        if (connection == nullptr) {
            connection = PQconnectdb(url_.c_str());
        } else if (PQstatus(connection) == CONNECTION_OK) {
            return true;
        } else {
            SELFVOY_LOG(log::Level::Warn, "db") << "соединение с PostgreSQL потеряно, переподключаюсь";
            PQreset(connection);
        }
        if (PQstatus(connection) != CONNECTION_OK) {
            SELFVOY_LOG(log::Level::Error, "db") << "PostgreSQL недоступен: " << PQerrorMessage(connection);
            return false;
        }
        SELFVOY_LOG(log::Level::Info, "db") << "соединение с PostgreSQL восстановлено";
        return prepareSession();
    }

    // Наружу (клиенту) уходит только обобщённое описание по SQLSTATE; текст
    // ошибки PostgreSQL (имена таблиц, ограничений, значения) — только в лог.
    static std::string publicError(const char* sqlState) {
        const std::string state = sqlState ? sqlState : "";
        if (state == "23505") return "запись уже существует";
        if (state == "23503") return "связанная запись не найдена";
        if (state == "23514" || state == "23502") return "недопустимое значение";
        if (state.rfind("22", 0) == 0) return "некорректный формат значения";
        return "ошибка базы данных";
    }

    bool execute(const std::string& sql, const std::vector<std::string>& args, PGresult** out) {
        std::string& lastError_ = tlsConn->lastError;
        lastError_.clear();
        std::vector<const char*> values;
        values.reserve(args.size());
        for (const auto& arg : args) values.push_back(arg.c_str());

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensureConnected()) {
                lastError_ = "база данных недоступна";
                return false;
            }
            PGresult* result = PQexecParams(tlsConn->pg, sql.c_str(), static_cast<int>(values.size()), nullptr,
                                            values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
            const ExecStatusType status = PQresultStatus(result);
            if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) {
                if (out != nullptr) {
                    *out = result;
                } else {
                    PQclear(result);
                }
                return true;
            }
            const char* sqlState = result ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
            SELFVOY_LOG(log::Level::Error, "db") << "SQL ошибка: " << PQresultErrorMessage(result);
            lastError_ = publicError(sqlState);
            PQclear(result);
            // Обрыв соединения посреди запроса: один повтор после переподключения.
            if (PQstatus(tlsConn->pg) == CONNECTION_OK) return false;
        }
        return false;
    }

    // Текст ошибки последнего запроса на соединении текущего вызова.
    const std::string& lastError() const { return tlsConn->lastError; }

    const std::string url_;
    mutable PgPool pool_;
    mutable PgPool lockPool_;
    std::atomic<unsigned long long> rateHits_{0};  // вызовы rateLimitHit — для редкой чистки
};

}  // namespace

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url) {
    return std::make_unique<PostgresDatabase>(url);
}

#else  // !SELFVOY_WITH_LIBPQ

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url) {
    (void)url;
    SELFVOY_LOG(log::Level::Warn, "db")
        << "сервер собран без libpq (нужен -DSELFVOY_WITH_LIBPQ=ON), PostgreSQL недоступен";
    return nullptr;
}

#endif  // SELFVOY_WITH_LIBPQ

}  // namespace selfvoy
