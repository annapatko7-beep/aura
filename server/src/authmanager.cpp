// aura/authmanager.cpp
#include "aura/authmanager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>

#include "aura/crypto.h"
#include "aura/jwt.h"
#include "aura/log.h"
#include "aura/protocol.h"
#include "aura/totp.h"

namespace aura {

namespace {

std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string isoIn(long long seconds) {
    const auto moment = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    const std::time_t time = std::chrono::system_clock::to_time_t(moment);
    std::tm tm{};
    gmtime_r(&time, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

// SHA-256 от кода/токена в hex — в БД хранится только хэш.
std::string codeHash(const std::string& code) { return crypto::toHex(crypto::sha256(code)); }

// Код не старше срока и ещё не использован (ISO-строки одного формата
// сравниваются лексикографически).
bool tokenUsable(const std::string& usedAt, const std::string& expiresAt) {
    return usedAt.empty() && expiresAt > isoIn(0);
}

}  // namespace

AuthManager::AuthManager(DatabaseManager& database, const Config& config)
    : database_(database), config_(config), mail_(makeMailProvider(config)) {}

std::string AuthManager::normalizeEmail(const std::string& email) {
    std::string normalized = trim(email);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

std::string AuthManager::passwordError(const std::string& password) {
    if (password.size() < 8) return "пароль должен быть не короче 8 символов";
    const bool hasLetter = std::any_of(password.begin(), password.end(),
                                       [](unsigned char ch) { return std::isalpha(ch) != 0; });
    const bool hasDigit = std::any_of(password.begin(), password.end(),
                                      [](unsigned char ch) { return std::isdigit(ch) != 0; });
    if (!hasLetter || !hasDigit) return "пароль должен содержать буквы и цифры";
    if (password.size() > 256) return "пароль слишком длинный";
    return "";
}

std::string AuthManager::issueToken(long long userId, const std::string& email, std::string& jwtId) const {
    jwtId = crypto::randomHex(12);
    Json claims = Json::object();
    claims.set("sub", Json(userId));
    claims.set("email", Json(email));
    claims.set("jti", Json(jwtId));
    claims.set("iss", Json("aura"));
    return jwt::sign(claims, config_.jwtSecret, config_.tokenTtlSeconds);
}

void AuthManager::audit(long long userId, const std::string& kind, const Json& detail,
                        const std::string& remoteAddr) {
    const DatabaseError stored = database_.db().insertAudit(userId, kind, detail, remoteAddr);
    if (!stored.ok) {
        AURA_LOG(log::Level::Warn, "auth") << "audit(" << kind << "): " << stored.message;
    }
}

std::string AuthManager::issueAuthCode(const UserRecord& user, const std::string& purpose,
                                       const std::string& remoteAddr) {
    std::string code;
    if (purpose == "email_verify") {
        // Шестизначный числовой код — удобно вводить с телефона.
        const auto bytes = crypto::randomBytes(4);
        const std::uint32_t value =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) << 24) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]));
        code = std::to_string(100000 + value % 900000);
    } else {
        code = crypto::randomHex(16);  // длинный токен для сброса пароля
    }

    // Старые коды того же назначения аннулируются: действует последний.
    database_.db().deleteAuthTokens(user.id, purpose);

    AuthTokenRecord record;
    record.id = crypto::randomHex(16);
    record.userId = user.id;
    record.purpose = purpose;
    record.tokenHash = codeHash(code);
    record.createdAt = isoIn(0);
    record.expiresAt = isoIn(purpose == "email_verify" ? config_.verifyTtlSeconds
                                                       : config_.resetTtlSeconds);
    const DatabaseError stored = database_.db().createAuthToken(record);
    if (!stored.ok) {
        AURA_LOG(log::Level::Error, "auth") << "не удалось сохранить код " << purpose << ": "
                                            << stored.message;
        return "";
    }

    const bool sent = purpose == "email_verify"
                          ? mail_->sendVerificationCode(user.email, code)
                          : mail_->sendPasswordReset(user.email, code);
    if (!sent) {
        AURA_LOG(log::Level::Error, "auth") << "письмо " << purpose << " не отправлено " << user.email;
    }
    audit(user.id, purpose == "email_verify" ? "verification_sent" : "password_reset_requested",
          Json::object(), remoteAddr);
    return code;
}

AuthManager::Result AuthManager::issueAccess(const UserRecord& user,
                                             const std::string& device,
                                             const std::string& remoteAddr) {
    std::string jwtId;
    const std::string token = issueToken(user.id, user.email, jwtId);

    SessionRecord session;
    session.id = crypto::randomHex(16);
    session.userId = user.id;
    session.jwtId = jwtId;
    session.device = device.empty() ? "qt-client" : device;
    session.remoteAddr = remoteAddr;
    session.expiresAt = isoIn(config_.tokenTtlSeconds);
    const DatabaseError stored = database_.db().createSession(session);
    if (!stored.ok) {
        AURA_LOG(log::Level::Warn, "auth") << "не удалось сохранить сессию: " << stored.message;
    }
    database_.db().touchUser(user.id);

    Result result;
    result.ok = true;
    result.userId = user.id;
    result.displayName = user.displayName;
    result.email = user.email;
    result.jwtId = jwtId;
    result.payload.set("token", Json(token));
    result.payload.set("token_type", Json("Bearer"));
    result.payload.set("expires_in", Json(static_cast<long long>(config_.tokenTtlSeconds)));
    result.payload.set("session_id", Json(session.id));
    result.payload.set("user", Json(user.toJson()));
    return result;
}

AuthManager::Result AuthManager::issue(const UserRecord& user,
                                       const std::string& device,
                                       const std::string& remoteAddr) {
    Result result = issueAccess(user, device, remoteAddr);

    // Новый refresh-токен — начало нового семейства.
    const std::string familyId = crypto::randomHex(16);
    const std::string token = crypto::randomHex(32);
    RefreshTokenRecord record;
    record.id = crypto::randomHex(16);
    record.userId = user.id;
    record.familyId = familyId;
    record.tokenHash = codeHash(token);
    record.device = device;
    record.remoteAddr = remoteAddr;
    record.createdAt = isoIn(0);
    record.expiresAt = isoIn(config_.refreshTtlSeconds);
    const DatabaseError stored = database_.db().createRefreshToken(record);
    if (!stored.ok) {
        AURA_LOG(log::Level::Warn, "auth") << "refresh-токен не сохранён: " << stored.message;
    } else {
        result.payload.set("refresh_token", Json(token));
        result.payload.set("refresh_expires_in", Json(static_cast<long long>(config_.refreshTtlSeconds)));
    }
    return result;
}

// --- ограничение частоты попыток -------------------------------------------
// Считаем провалы по двум ключам сразу: по адресу (перебор одного аккаунта)
// и по email (перебор по многим адресам с одного места).

bool AuthManager::throttled(const std::string& key) {
    if (key.empty() || config_.maxLoginAttempts <= 0) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    const auto it = attempts_.find(key);
    if (it == attempts_.end()) return false;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.windowStart).count();
    if (age > config_.loginWindowSec) {
        attempts_.erase(it);
        return false;
    }
    return it->second.count >= config_.maxLoginAttempts;
}

void AuthManager::registerFailure(const std::string& remoteAddr, const std::string& email) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    for (const auto& key : {remoteAddr, email}) {
        if (key.empty()) continue;
        auto& entry = attempts_[key];
        const auto age =
            std::chrono::duration_cast<std::chrono::seconds>(now - entry.windowStart).count();
        if (entry.count == 0 || age > config_.loginWindowSec) {
            entry.count = 1;
            entry.windowStart = now;
        } else {
            ++entry.count;
        }
    }
}

void AuthManager::clearFailures(const std::string& remoteAddr, const std::string& email) {
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    attempts_.erase(remoteAddr);
    attempts_.erase(email);
}

AuthManager::Result AuthManager::registerUser(const std::string& email,
                                              const std::string& password,
                                              const std::string& displayName,
                                              const std::string& device,
                                              const std::string& remoteAddr) {
    (void)device;  // сессия при регистрации не создаётся — сначала подтверждение email
    const std::string normalized = normalizeEmail(email);
    if (throttled(remoteAddr) || throttled(normalized)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    if (normalized.empty() || normalized.find('@') == std::string::npos) {
        return Result::failure(protocol::code::kBadRequest, "некорректный email");
    }
    if (const std::string error = passwordError(password); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }
    const std::string name = trim(displayName).empty() ? normalized.substr(0, normalized.find('@'))
                                                       : trim(displayName);

    long long userId = 0;
    const DatabaseError created =
        database_.db().createUser(normalized, crypto::hashPassword(password), name, userId);
    if (!created.ok) return Result::failure(protocol::code::kConflict, created.message);

    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kInternal, "пользователь не создан");

    const std::string code = issueAuthCode(*user, "email_verify", remoteAddr);
    if (code.empty()) {
        return Result::failure(protocol::code::kInternal, "не удалось выпустить код подтверждения");
    }

    AURA_LOG(log::Level::Info, "auth") << "регистрация " << normalized << " (id=" << userId
                                       << "), ждём подтверждения email";
    audit(userId, "register", Json::object(), remoteAddr);

    Result result;
    result.ok = true;
    result.userId = userId;
    result.displayName = user->displayName;
    result.email = user->email;
    result.payload.set("requires_verification", Json(true));
    result.payload.set("user", Json(user->toJson()));
    result.payload.set("message", Json("мы отправили код подтверждения на " + user->email));
    if (mail_->exposesCodes()) result.payload.set("email_code", Json(code));  // режим dev
    return result;
}

AuthManager::Result AuthManager::verifyEmail(const std::string& email, const std::string& code) {
    Result result;
    const std::string normalized = normalizeEmail(email);
    const auto user = database_.db().findUserByEmail(normalized);
    // Ошибка одинакова для «нет пользователя» и «неверный код».
    if (!user || code.empty()) {
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    if (user->emailVerified) {
        result.ok = true;  // идемпотентно: повторное подтверждение не ошибка
        result.payload.set("verified", Json(true));
        result.payload.set("user", Json(user->toJson()));
        return result;
    }

    const auto token = database_.db().findAuthToken("email_verify", codeHash(code));
    if (!token || token->userId != user->id || !tokenUsable(token->usedAt, token->expiresAt)) {
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }

    database_.db().useAuthToken(token->id);
    database_.db().deleteAuthTokens(user->id, "email_verify");
    database_.db().setEmailVerified(user->id, true);
    audit(user->id, "email_verified", Json::object(), "");

    AURA_LOG(log::Level::Info, "auth") << "email подтверждён: " << normalized;
    result.ok = true;
    result.userId = user->id;
    result.payload.set("verified", Json(true));
    result.payload.set("user", Json(user->toJson()));
    return result;
}

AuthManager::Result AuthManager::resendVerification(const std::string& email,
                                                    const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    if (throttled(remoteAddr)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    registerFailure(remoteAddr, "");  // считаем запросы, чтобы нельзя было флудить письмами

    Result result;
    result.ok = true;  // ответ не раскрывает, существует ли email
    result.payload.set("message", Json("если email зарегистрирован и не подтверждён, код отправлен"));
    const auto user = database_.db().findUserByEmail(normalized);
    if (user && !user->emailVerified) {
        const std::string code = issueAuthCode(*user, "email_verify", remoteAddr);
        if (!code.empty() && mail_->exposesCodes()) result.payload.set("email_code", Json(code));
    }
    return result;
}

AuthManager::Result AuthManager::login(const std::string& email,
                                       const std::string& password,
                                       const std::string& device,
                                       const std::string& deviceId,
                                       const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    if (throttled(remoteAddr) || throttled(normalized)) {
        AURA_LOG(log::Level::Warn, "auth") << "вход заблокирован по частоте: " << remoteAddr;
        audit(0, "login_blocked", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserByEmail(normalized);
    if (!user) {
        // Не раскрываем, существует ли пользователь: ошибка одинаковая.
        registerFailure(remoteAddr, normalized);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    if (!user->active) return Result::failure(protocol::code::kForbidden, "аккаунт отключён");
    if (!crypto::verifyPassword(password, user->passwordHash)) {
        registerFailure(remoteAddr, normalized);
        AURA_LOG(log::Level::Warn, "auth") << "неудачный вход " << normalized << " из " << remoteAddr;
        audit(user->id, "login_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    if (!user->emailVerified) {
        // Пароль верный, но email не подтверждён — отдельный код ошибки,
        // чтобы клиент показал экран ввода кода.
        return Result::failure("email_not_verified", "подтвердите email — мы отправили код");
    }

    // Прозрачная миграция: успешный вход со старым PBKDF2-хэшем
    // пересчитывает его в Argon2id.
    if (crypto::passwordNeedsRehash(user->passwordHash)) {
        const std::string upgraded = crypto::hashPassword(password);
        if (!upgraded.empty() && database_.db().updatePasswordHash(user->id, upgraded).ok) {
            AURA_LOG(log::Level::Info, "auth") << "хэш пароля " << normalized << " мигрирован на Argon2id";
            audit(user->id, "password_rehashed", Json::object(), remoteAddr);
        }
    }

    clearFailures(remoteAddr, normalized);

    // Порядок: пароль → TOTP → сессия. Если 2FA включена, а устройство не
    // доверенное — просим код (клиент вызовет auth.login2fa).
    const auto twoFactor = database_.db().findTwoFactor(user->id);
    if (twoFactor && twoFactor->enabled) {
        bool trusted = false;
        if (!deviceId.empty()) {
            const auto td = database_.db().findTrustedDevice(user->id, codeHash(deviceId));
            if (td) {
                trusted = true;
                database_.db().touchTrustedDevice(user->id, codeHash(deviceId));
            }
        }
        if (!trusted) {
            audit(user->id, "login_2fa_required", Json::object(), remoteAddr);
            return Result::failure("requires_2fa", "введите код из приложения-аутентификатора");
        }
        audit(user->id, "login_trusted_device", Json::object(), remoteAddr);
    }

    AURA_LOG(log::Level::Info, "auth") << "вход " << normalized << " из " << remoteAddr;
    Result result = issue(*user, device, remoteAddr);
    audit(user->id, "login_ok", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::refresh(const std::string& refreshToken,
                                         const std::string& device,
                                         const std::string& remoteAddr) {
    if (refreshToken.empty()) {
        return Result::failure(protocol::code::kUnauthorized, "нет refresh-токена");
    }
    const auto record = database_.db().findRefreshToken(codeHash(refreshToken));
    if (!record) {
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен недействителен");
    }

    if (!record->revokedAt.empty()) {
        // Повторное использование отозванного токена = вероятная кража:
        // отозвать всё семейство и все сессии пользователя.
        AURA_LOG(log::Level::Warn, "auth")
            << "повторное использование refresh-токена (user=" << record->userId
            << ") — семейство и сессии отозваны";
        database_.db().revokeRefreshFamily(record->familyId);
        database_.db().revokeAllSessions(record->userId, "");
        audit(record->userId, "refresh_reuse_detected", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен недействителен");
    }
    if (!tokenUsable("", record->expiresAt)) {
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен истёк");
    }

    const auto user = database_.db().findUserById(record->userId);
    if (!user || !user->active || !user->emailVerified) {
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен недействителен");
    }

    // Ротация: новый токен в том же семействе, старый помечается заменённым.
    const std::string newToken = crypto::randomHex(32);
    RefreshTokenRecord fresh;
    fresh.id = crypto::randomHex(16);
    fresh.userId = user->id;
    fresh.familyId = record->familyId;
    fresh.tokenHash = codeHash(newToken);
    fresh.device = device.empty() ? record->device : device;
    fresh.remoteAddr = remoteAddr;
    fresh.createdAt = isoIn(0);
    fresh.expiresAt = isoIn(config_.refreshTtlSeconds);
    const DatabaseError stored = database_.db().createRefreshToken(fresh);
    if (!stored.ok) {
        return Result::failure(protocol::code::kInternal, "не удалось выпустить refresh-токен");
    }
    database_.db().revokeRefreshToken(record->id, fresh.id);

    Result result = issueAccess(*user, device.empty() ? record->device : device, remoteAddr);
    result.payload.set("refresh_token", Json(newToken));
    result.payload.set("refresh_expires_in", Json(static_cast<long long>(config_.refreshTtlSeconds)));
    audit(user->id, "refresh_rotated", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::verifyToken(const std::string& token) {
    if (token.empty()) return Result::failure(protocol::code::kUnauthorized, "нет токена");

    std::string raw = token;
    const std::string prefix = "Bearer ";
    if (raw.size() > prefix.size() && raw.compare(0, prefix.size(), prefix) == 0) {
        raw = raw.substr(prefix.size());
    }

    const jwt::Token verified = jwt::verify(raw, config_.jwtSecret);
    if (!verified.valid) return Result::failure(protocol::code::kUnauthorized, verified.error);

    const std::string jwtId = verified.claims.getString("jti");
    const auto session = database_.db().findSessionByJwt(jwtId);
    if (!session) return Result::failure(protocol::code::kUnauthorized, "сессия не найдена или отозвана");

    const auto user = database_.db().findUserById(verified.claims.getInt("sub"));
    if (!user) return Result::failure(protocol::code::kUnauthorized, "пользователь не найден");

    Result result;
    result.ok = true;
    result.userId = user->id;
    result.displayName = user->displayName;
    result.email = user->email;
    result.jwtId = jwtId;
    result.payload.set("user", Json(user->toJson()));
    return result;
}

AuthManager::Result AuthManager::logout(const std::string& jwtId) {
    database_.db().revokeSession(jwtId);
    Result result;
    result.ok = true;
    result.payload.set("revoked", Json(true));
    return result;
}

AuthManager::Result AuthManager::changePassword(long long userId,
                                                const std::string& jwtId,
                                                const std::string& oldPassword,
                                                const std::string& newPassword) {
    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kUnauthorized, "пользователь не найден");
    if (!crypto::verifyPassword(oldPassword, user->passwordHash)) {
        audit(userId, "password_change_failed", Json::object(), "");
        return Result::failure(protocol::code::kForbidden, "неверный текущий пароль");
    }
    if (const std::string error = passwordError(newPassword); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }
    const std::string hash = crypto::hashPassword(newPassword);
    if (hash.empty()) return Result::failure(protocol::code::kInternal, "не удалось хэшировать пароль");
    const DatabaseError updated = database_.db().updatePasswordHash(userId, hash);
    if (!updated.ok) return Result::failure(protocol::code::kInternal, updated.message);

    // Все прочие сессии и refresh-токены отзываются: пароль сменился —
    // остальные устройства должны войти заново.
    database_.db().revokeAllSessions(userId, jwtId);
    database_.db().revokeAllUserRefreshTokens(userId);
    audit(userId, "password_changed", Json::object(), "");
    AURA_LOG(log::Level::Info, "auth") << "смена пароля user=" << userId;

    Result result;
    result.ok = true;
    result.payload.set("changed", Json(true));
    return result;
}

AuthManager::Result AuthManager::forgotPassword(const std::string& email,
                                                const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    if (throttled(remoteAddr)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    registerFailure(remoteAddr, "");  // анти-флуд

    Result result;
    result.ok = true;  // ответ не раскрывает, существует ли email
    result.payload.set("message", Json("если email зарегистрирован, код сброса отправлен"));
    const auto user = database_.db().findUserByEmail(normalized);
    if (user) {
        const std::string code = issueAuthCode(*user, "password_reset", remoteAddr);
        if (!code.empty() && mail_->exposesCodes()) result.payload.set("reset_code", Json(code));
    }
    return result;
}

AuthManager::Result AuthManager::resetPassword(const std::string& email,
                                               const std::string& code,
                                               const std::string& newPassword,
                                               const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    const auto user = database_.db().findUserByEmail(normalized);
    // Ошибка одинакова для «нет пользователя» и «неверный код».
    if (!user || code.empty()) {
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    const auto token = database_.db().findAuthToken("password_reset", codeHash(code));
    if (!token || token->userId != user->id || !tokenUsable(token->usedAt, token->expiresAt)) {
        audit(user->id, "password_reset_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    if (const std::string error = passwordError(newPassword); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }

    const std::string hash = crypto::hashPassword(newPassword);
    if (hash.empty()) return Result::failure(protocol::code::kInternal, "не удалось хэшировать пароль");
    const DatabaseError updated = database_.db().updatePasswordHash(user->id, hash);
    if (!updated.ok) return Result::failure(protocol::code::kInternal, updated.message);

    database_.db().useAuthToken(token->id);
    database_.db().deleteAuthTokens(user->id, "password_reset");
    // Сброс пароля обрывает все сессии и refresh-токены.
    database_.db().revokeAllSessions(user->id, "");
    database_.db().revokeAllUserRefreshTokens(user->id);
    audit(user->id, "password_reset", Json::object(), remoteAddr);
    AURA_LOG(log::Level::Info, "auth") << "сброс пароля " << normalized;

    Result result;
    result.ok = true;
    result.userId = user->id;
    result.payload.set("reset", Json(true));
    return result;
}

AuthManager::Result AuthManager::listSessions(long long userId, const std::string& currentJwtId) {
    Json sessions = Json::array();
    for (const auto& session : database_.db().listSessions(userId)) {
        Json item = Json::object();
        item.set("id", Json(session.id));
        item.set("device", Json(session.device));
        item.set("remote_addr", Json(session.remoteAddr));
        item.set("created_at", Json(session.createdAt));
        item.set("expires_at", Json(session.expiresAt));
        item.set("current", Json(session.jwtId == currentJwtId));
        sessions.push(item);
    }
    Result result;
    result.ok = true;
    result.payload.set("sessions", sessions);
    return result;
}

AuthManager::Result AuthManager::revokeSession(long long userId, const std::string& sessionId) {
    if (sessionId.empty()) {
        return Result::failure(protocol::code::kBadRequest, "нужен session_id");
    }
    const DatabaseError revoked = database_.db().revokeSessionById(userId, sessionId);
    if (!revoked.ok) return Result::failure(protocol::code::kNotFound, revoked.message);
    Json detail = Json::object();
    detail.set("session_id", Json(sessionId));
    audit(userId, "session_revoked", detail, "");

    Result result;
    result.ok = true;
    result.payload.set("revoked", Json(true));
    return result;
}

AuthManager::Result AuthManager::revokeAllSessions(long long userId, const std::string& currentJwtId) {
    const DatabaseError revoked = database_.db().revokeAllSessions(userId, currentJwtId);
    if (!revoked.ok) return Result::failure(protocol::code::kInternal, revoked.message);
    audit(userId, "sessions_revoked_all", Json::object(), "");

    Result result;
    result.ok = true;
    result.payload.set("revoked", Json(true));
    return result;
}

// --- Двухфакторная аутентификация (TOTP) ------------------------------------

std::string AuthManager::twoFactorKeyBytes() const {
    // В продакшене — отдельный ключ AURA_2FA_KEY; в демо/тестах выводим из
    // jwtSecret, чтобы функция работала без отдельной настройки. SHA-256 даёт
    // ровно 32 байта — ключ AES-256.
    const std::string base = config_.twoFactorKey.empty() ? config_.jwtSecret : config_.twoFactorKey;
    return crypto::sha256(base);
}

bool AuthManager::decryptTwoFactorSecret(const TwoFactorRecord& record, std::string& outRaw) const {
    std::string blob;
    if (!crypto::base64Decode(record.secretEncrypted, blob)) return false;
    return crypto::aes256GcmDecrypt(twoFactorKeyBytes(), blob, "aura-2fa-secret", outRaw);
}

std::vector<std::string> AuthManager::generateRecoveryCodes(long long userId) {
    // Алфавит без визуально похожих символов (нет 0/O, 1/I/L).
    static const char* alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // 31 символ
    const std::string randomness = crypto::randomBytes(100);
    std::vector<RecoveryCodeRecord> records;
    std::vector<std::string> plaintext;
    records.reserve(10);
    plaintext.reserve(10);
    std::size_t pos = 0;
    for (int i = 0; i < 10; ++i) {
        std::string code;  // формат XXXXX-XXXXX
        for (int j = 0; j < 10; ++j) {
            if (j == 5) code += '-';
            code += alphabet[static_cast<unsigned char>(randomness[pos++]) % 31];
        }
        plaintext.push_back(code);
        RecoveryCodeRecord rec;
        rec.id = crypto::randomHex(16);
        rec.userId = userId;
        rec.codeHash = codeHash(code);
        rec.createdAt = isoIn(0);
        records.push_back(rec);
    }
    database_.db().replaceRecoveryCodes(userId, records);
    return plaintext;
}

AuthManager::Result AuthManager::setup2fa(long long userId, const std::string& remoteAddr) {
    const std::string secretB32 = totp::generateSecretBase32();
    std::string secretRaw;
    if (!totp::decodeSecretBase32(secretB32, secretRaw)) {
        return Result::failure(protocol::code::kInternal, "не удалось сгенерировать секрет");
    }
    std::string blob;
    if (!crypto::aes256GcmEncrypt(twoFactorKeyBytes(), secretRaw, "aura-2fa-secret", blob)) {
        return Result::failure(protocol::code::kInternal, "не удалось зашифровать секрет");
    }
    TwoFactorRecord record;
    record.userId = userId;
    record.secretEncrypted = crypto::base64Encode(blob);
    record.enabled = false;  // включится только после confirm2fa
    record.lastUsedCounter = 0;
    record.createdAt = isoIn(0);
    const DatabaseError stored = database_.db().upsertTwoFactor(record);
    if (!stored.ok) return Result::failure(protocol::code::kInternal, stored.message);

    const auto user = database_.db().findUserById(userId);
    const std::string account = user ? user->email : "";
    Result result;
    result.ok = true;
    result.userId = userId;
    result.payload.set("secret", Json(secretB32));
    result.payload.set("otpauth_uri", Json(totp::otpauthUri(secretB32, account, "Aura")));
    result.payload.set("issuer", Json("Aura"));
    result.payload.set("algorithm", Json("SHA1"));
    result.payload.set("digits", Json(6));
    result.payload.set("period", Json(30));
    audit(userId, "2fa_setup_started", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::confirm2fa(long long userId, const std::string& code,
                                            const std::string& remoteAddr) {
    const auto record = database_.db().findTwoFactor(userId);
    if (!record) return Result::failure(protocol::code::kBadRequest, "2FA не настроена");
    if (record->enabled) return Result::failure(protocol::code::kBadRequest, "2FA уже включена");
    std::string secretRaw;
    if (!decryptTwoFactorSecret(*record, secretRaw)) {
        return Result::failure(protocol::code::kInternal, "не удалось прочитать секрет");
    }
    std::int64_t matched = 0;
    if (!totp::verify(secretRaw, code, record->lastUsedCounter, matched)) {
        audit(userId, "2fa_confirm_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный код");
    }
    database_.db().updateTwoFactorCounter(userId, matched);
    database_.db().setTwoFactorEnabled(userId, true);
    const std::vector<std::string> codes = generateRecoveryCodes(userId);
    Result result;
    result.ok = true;
    result.userId = userId;
    Json arr = Json::array();
    for (const auto& c : codes) arr.push(Json(c));
    result.payload.set("recovery_codes", arr);
    audit(userId, "2fa_enabled", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::disable2fa(long long userId, const std::string& password,
                                            const std::string& remoteAddr) {
    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    if (!crypto::verifyPassword(password, user->passwordHash)) {
        audit(userId, "2fa_disable_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный пароль");
    }
    database_.db().deleteTwoFactor(userId);
    database_.db().replaceRecoveryCodes(userId, {});
    database_.db().revokeAllTrustedDevices(userId);
    audit(userId, "2fa_disabled", Json::object(), remoteAddr);
    Result result;
    result.ok = true;
    result.userId = userId;
    return result;
}

AuthManager::Result AuthManager::twoFactorStatus(long long userId) {
    const auto record = database_.db().findTwoFactor(userId);
    Result result;
    result.ok = true;
    result.userId = userId;
    const bool enabled = record.has_value() && record->enabled;
    result.payload.set("enabled", Json(enabled));
    result.payload.set("pending", Json(record.has_value() && !record->enabled));
    result.payload.set("recovery_codes_left",
                       Json(enabled ? database_.db().countUnusedRecoveryCodes(userId) : 0));
    return result;
}

AuthManager::Result AuthManager::login2fa(const std::string& email, const std::string& password,
                                          const std::string& code, bool trustDevice,
                                          const std::string& deviceId, const std::string& device,
                                          const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    if (throttled(remoteAddr) || throttled(normalized)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserByEmail(normalized);
    if (!user || !user->active || !crypto::verifyPassword(password, user->passwordHash)) {
        registerFailure(remoteAddr, normalized);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    if (!user->emailVerified) {
        return Result::failure("email_not_verified", "подтвердите email — мы отправили код");
    }
    const auto record = database_.db().findTwoFactor(user->id);
    if (!record || !record->enabled) {
        return Result::failure(protocol::code::kBadRequest, "2FA не включена для этого аккаунта");
    }
    // Резервный код (с дефисом, формат XXXXX-XXXXX) или 6-значный TOTP-код.
    const bool isRecovery = code.find('-') != std::string::npos;
    if (isRecovery) {
        const auto rc = database_.db().findRecoveryCode(user->id, codeHash(code));
        if (!rc) {
            registerFailure(remoteAddr, normalized);
            audit(user->id, "2fa_recovery_failed", Json::object(), remoteAddr);
            return Result::failure(protocol::code::kUnauthorized, "неверный код");
        }
        database_.db().useRecoveryCode(rc->id);
        audit(user->id, "2fa_recovery_used", Json::object(), remoteAddr);
    } else {
        std::string secretRaw;
        if (!decryptTwoFactorSecret(*record, secretRaw)) {
            return Result::failure(protocol::code::kInternal, "не удалось прочитать секрет");
        }
        std::int64_t matched = 0;
        if (!totp::verify(secretRaw, code, record->lastUsedCounter, matched)) {
            registerFailure(remoteAddr, normalized);
            audit(user->id, "2fa_failed", Json::object(), remoteAddr);
            return Result::failure(protocol::code::kUnauthorized, "неверный код");
        }
        database_.db().updateTwoFactorCounter(user->id, matched);
    }
    clearFailures(remoteAddr, normalized);
    if (trustDevice && !deviceId.empty()) {
        TrustedDeviceRecord td;
        td.id = crypto::randomHex(16);
        td.userId = user->id;
        td.deviceHash = codeHash(deviceId);
        td.device = device.empty() ? "qt-client" : device;
        td.remoteAddr = remoteAddr;
        td.createdAt = isoIn(0);
        td.expiresAt = isoIn(90LL * 24 * 3600);  // 90 дней
        database_.db().addTrustedDevice(td);
    }
    AURA_LOG(log::Level::Info, "auth") << "вход (2FA) " << normalized << " из " << remoteAddr;
    Result result = issue(*user, device, remoteAddr);
    audit(user->id, "login_ok", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::listTrustedDevices(long long userId) {
    Result result;
    result.ok = true;
    result.userId = userId;
    Json arr = Json::array();
    for (const auto& td : database_.db().listTrustedDevices(userId)) {
        Json item = Json::object();
        item.set("id", Json(td.id));
        item.set("device", Json(td.device));
        item.set("remote_addr", Json(td.remoteAddr));
        item.set("created_at", Json(td.createdAt));
        item.set("expires_at", Json(td.expiresAt));
        item.set("last_used_at", Json(td.lastUsedAt));
        arr.push(item);
    }
    result.payload.set("devices", arr);
    return result;
}

AuthManager::Result AuthManager::revokeTrustedDevice(long long userId, const std::string& id,
                                                     const std::string& remoteAddr) {
    const DatabaseError r = database_.db().revokeTrustedDevice(userId, id);
    if (!r.ok) return Result::failure(protocol::code::kNotFound, r.message);
    audit(userId, "2fa_device_revoked", Json::object(), remoteAddr);
    Result result;
    result.ok = true;
    result.userId = userId;
    return result;
}

AuthManager::Result AuthManager::revokeAllTrustedDevices(long long userId, const std::string& remoteAddr) {
    database_.db().revokeAllTrustedDevices(userId);
    audit(userId, "2fa_devices_revoked_all", Json::object(), remoteAddr);
    Result result;
    result.ok = true;
    result.userId = userId;
    return result;
}

}  // namespace aura
