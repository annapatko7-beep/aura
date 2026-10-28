// selfvoy/authmanager.cpp
#include "selfvoy/authmanager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>

#include "selfvoy/crypto.h"
#include "selfvoy/jwt.h"
#include "selfvoy/log.h"
#include "selfvoy/protocol.h"
#include "selfvoy/session.h"
#include "selfvoy/totp.h"

namespace selfvoy {

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

// Хэш одноразового кода, привязанный к пользователю и назначению: шестизначные
// коды разных людей могут совпасть, а поиск идёт по (purpose, token_hash).
std::string scopedCodeHash(long long userId, const std::string& purpose, const std::string& code) {
    return codeHash(std::to_string(userId) + ":" + purpose + ":" + code);
}

// Равномерный шестизначный код 100000–999999 (отбраковка против смещения по модулю).
std::string sixDigitCode() {
    constexpr std::uint32_t kRange = 900000;
    constexpr std::uint32_t kLimit = 0xFFFFFFFFu - (0xFFFFFFFFu % kRange);
    while (true) {
        const std::string bytes = crypto::randomBytes(4);
        const std::uint32_t value =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) << 24) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]));
        if (value < kLimit) return std::to_string(100000 + value % kRange);
    }
}

// UUID с дефисами и 32 hex — один идентификатор; в jti кладём форму без дефисов.
std::string plainUuid(const std::string& value) {
    std::string out;
    for (const char ch : value) {
        if (ch != '-') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

// Следующая кодовая точка UTF-8 (строгий разбор: без overlong и суррогатов).
bool nextCodePoint(const std::string& text, std::size_t& pos, std::uint32_t& out) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(pos);
    std::size_t length = 0;
    std::uint32_t minimum = 0;
    if (lead < 0x80) {
        out = lead;
        ++pos;
        return true;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2, minimum = 0x80, out = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3, minimum = 0x800, out = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4, minimum = 0x10000, out = lead & 0x07;
    } else {
        return false;
    }
    if (pos + length > text.size()) return false;
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(pos + i) & 0xC0) != 0x80) return false;
        out = (out << 6) | (byte(pos + i) & 0x3F);
    }
    if (out < minimum || out > 0x10FFFF || (out >= 0xD800 && out <= 0xDFFF)) return false;
    pos += length;
    return true;
}

// Буква для правила пароля: латиница, Latin-1 и расширенная латиница,
// греческий, кириллица (клиенты принимают «пароль12» — сервер тоже).
bool isLetter(std::uint32_t code) {
    if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) return true;
    if (code >= 0xC0 && code <= 0x24F) return code != 0xD7 && code != 0xF7;  // кроме × и ÷
    if (code >= 0x386 && code <= 0x3FF) return code != 0x387;                 // греческий (кроме ·)
    if (code >= 0x400 && code <= 0x52F) return !(code >= 0x482 && code <= 0x489);  // кириллица
    return false;
}

// Минимальная проверка адреса: одна «@», непустые части, без пробелов и
// управляющих символов, разумная длина (адрес попадает в заголовок письма).
bool plausibleEmail(const std::string& email) {
    if (email.size() < 3 || email.size() > 254) return false;
    const std::size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= email.size()) return false;
    if (email.find('@', at + 1) != std::string::npos) return false;
    for (const unsigned char ch : email) {
        if (ch <= 0x20 || ch == 0x7F || ch == '<' || ch == '>' || ch == '"' || ch == ',' || ch == ';') return false;
    }
    return true;
}

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
    // Байтовый лимит — до разбора: Argon2 не должен получать мегабайтные строки.
    if (password.size() > 256) return "пароль слишком длинный (до 256 байт)";
    std::size_t characters = 0;
    bool hasLetter = false;
    bool hasDigit = false;
    std::size_t pos = 0;
    while (pos < password.size()) {
        std::uint32_t code = 0;
        if (!nextCodePoint(password, pos, code)) return "пароль содержит некорректные символы (не UTF-8)";
        ++characters;
        if (code >= '0' && code <= '9') hasDigit = true;
        if (isLetter(code)) hasLetter = true;
    }
    if (characters < 8) return "пароль должен быть не короче 8 символов";
    if (!hasLetter || !hasDigit) return "пароль должен содержать буквы и цифры";
    return "";
}

std::string AuthManager::familyOfJwt(const std::string& jwtId) {
    const std::size_t dot = jwtId.find('.');
    if (dot == std::string::npos || dot == 0) return "";
    return jwtId.substr(0, dot);
}

std::string AuthManager::issueToken(long long userId, const std::string& email, std::string& jwtId,
                                    const std::string& familyId) const {
    jwtId = familyId.empty() ? crypto::randomHex(12) : plainUuid(familyId) + "." + crypto::randomHex(12);
    Json claims = Json::object();
    claims.set("sub", Json(userId));
    claims.set("email", Json(email));
    claims.set("jti", Json(jwtId));
    claims.set("iss", Json("selfvoy"));
    return jwt::sign(claims, config_.jwtSecret, config_.tokenTtlSeconds);
}

void AuthManager::audit(long long userId, const std::string& kind, const Json& detail,
                        const std::string& remoteAddr) {
    const DatabaseError stored = database_.db().insertAudit(userId, kind, detail, remoteAddr);
    if (!stored.ok) {
        SELFVOY_LOG(log::Level::Warn, "auth") << "audit(" << kind << "): " << stored.message;
    }
}

std::string AuthManager::issueAuthCode(const UserRecord& user, const std::string& purpose,
                                       const std::string& remoteAddr) {
    // Шестизначный числовой код для обоих назначений — удобно вводить с
    // телефона; перебор ограничен maxCodeAttempts (код сгорает) и сроком жизни.
    const std::string code = sixDigitCode();

    // Старые коды того же назначения аннулируются: действует последний.
    database_.db().deleteAuthTokens(user.id, purpose);
    clearKey("code:" + purpose + ":" + std::to_string(user.id));

    AuthTokenRecord record;
    record.id = crypto::randomHex(16);
    record.userId = user.id;
    record.purpose = purpose;
    record.tokenHash = scopedCodeHash(user.id, purpose, code);
    record.createdAt = isoIn(0);
    record.expiresAt = isoIn(purpose == "email_verify" ? config_.verifyTtlSeconds
                                                       : config_.resetTtlSeconds);
    const DatabaseError stored = database_.db().createAuthToken(record);
    if (!stored.ok) {
        SELFVOY_LOG(log::Level::Error, "auth") << "не удалось сохранить код " << purpose << ": "
                                            << stored.message;
        return "";
    }

    const bool sent = purpose == "email_verify"
                          ? mail_->sendVerificationCode(user.email, code)
                          : mail_->sendPasswordReset(user.email, code);
    if (!sent) {
        SELFVOY_LOG(log::Level::Error, "auth") << "письмо " << purpose << " не отправлено " << user.email;
    }
    audit(user.id, purpose == "email_verify" ? "verification_sent" : "password_reset_requested",
          Json::object(), remoteAddr);
    return code;
}

AuthManager::Result AuthManager::issueAccess(const UserRecord& user,
                                             const std::string& device,
                                             const std::string& remoteAddr,
                                             const std::string& familyId) {
    std::string jwtId;
    const std::string token = issueToken(user.id, user.email, jwtId, familyId);

    SessionRecord session;
    session.id = crypto::randomHex(16);
    session.userId = user.id;
    session.jwtId = jwtId;
    session.device = device.empty() ? "qt-client" : device;
    session.remoteAddr = remoteAddr;
    session.expiresAt = isoIn(config_.tokenTtlSeconds);
    const DatabaseError stored = database_.db().createSession(session);
    if (!stored.ok) {
        SELFVOY_LOG(log::Level::Warn, "auth") << "не удалось сохранить сессию: " << stored.message;
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
    // Новый refresh-токен — начало нового семейства; jti сессии ссылается на него.
    const std::string familyId = crypto::randomHex(16);
    Result result = issueAccess(user, device, remoteAddr, familyId);

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
        SELFVOY_LOG(log::Level::Warn, "auth") << "refresh-токен не сохранён: " << stored.message;
    } else {
        result.payload.set("refresh_token", Json(token));
        result.payload.set("refresh_expires_in", Json(static_cast<long long>(config_.refreshTtlSeconds)));
    }
    return result;
}

// --- ограничение частоты попыток -------------------------------------------
// Счётчики живут в памяти процесса (перезапуск их обнуляет). Ключ по IP — без
// порта: переподключение больше не сбрасывает счётчик.

std::string AuthManager::ipKey(const std::string& remoteAddr) {
    const std::string ip = Session::ipOf(remoteAddr);
    return ip.empty() ? std::string() : "ip:" + ip;
}

bool AuthManager::overLimit(const std::string& key, int limit) {
    if (key.empty() || limit <= 0) return false;
    // Несколько экземпляров сервера на общем PostgreSQL: счётчики в БД, иначе
    // перебор шёл бы «по кругу» через разные экземпляры.
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitCount("auth:" + key) >= limit;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    const auto it = attempts_.find(key);
    if (it == attempts_.end()) return false;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.windowStart).count();
    if (age > it->second.windowSec) {
        attempts_.erase(it);
        return false;
    }
    return it->second.count >= limit;
}

int AuthManager::hit(const std::string& key, int windowSec) {
    if (key.empty()) return 0;
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitHit("auth:" + key, std::max(1, windowSec));
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    // Чистка большой таблицы — не чаще раза в секунду или когда она выросла
    // вдвое с прошлой чистки (амортизированно O(1) на событие).
    if (attempts_.size() >= nextPruneAt_ ||
        (attempts_.size() >= kPruneThreshold && now - lastPrune_ >= std::chrono::seconds(1))) {
        pruneLocked(now);
        lastPrune_ = now;
        nextPruneAt_ = std::max(kPruneThreshold, attempts_.size() * 2);
    }
    auto& entry = attempts_[key];
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.windowStart).count();
    if (entry.count == 0 || age > entry.windowSec) {
        entry.count = 1;
        entry.windowSec = std::max(1, windowSec);
        entry.windowStart = now;
    } else {
        ++entry.count;
    }
    return entry.count;
}

void AuthManager::clearKey(const std::string& key) {
    if (database_.db().sharedRateLimits()) {
        database_.db().rateLimitClear("auth:" + key);
        return;
    }
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    attempts_.erase(key);
}

void AuthManager::pruneLocked(std::chrono::steady_clock::time_point now) {
    // Истёкшие окна удаляются; если таблица всё равно огромна (флуд уникальными
    // ключами), выбрасываем самые старые записи — память ограничена.
    for (auto it = attempts_.begin(); it != attempts_.end();) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.windowStart).count();
        it = age > it->second.windowSec ? attempts_.erase(it) : std::next(it);
    }
    if (attempts_.size() < kMaxAttemptEntries) return;
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> byAge;
    byAge.reserve(attempts_.size());
    for (const auto& entry : attempts_) byAge.emplace_back(entry.second.windowStart, entry.first);
    std::sort(byAge.begin(), byAge.end());
    const std::size_t drop = attempts_.size() - kMaxAttemptEntries * 3 / 4;
    for (std::size_t i = 0; i < drop && i < byAge.size(); ++i) attempts_.erase(byAge[i].second);
}

std::size_t AuthManager::attemptsSize() {
    std::lock_guard<std::mutex> lock(attemptsMutex_);
    return attempts_.size();
}

void AuthManager::codeFailure(long long userId, const std::string& purpose, const std::string& ip) {
    hit(ip, config_.loginWindowSec);
    if (userId == 0 || config_.maxCodeAttempts <= 0) return;
    const std::string key = "code:" + purpose + ":" + std::to_string(userId);
    const long long ttl = purpose == "email_verify" ? config_.verifyTtlSeconds : config_.resetTtlSeconds;
    if (hit(key, static_cast<int>(std::min<long long>(ttl, 86400))) >= config_.maxCodeAttempts) {
        // Перебор шестизначного кода: после N неверных попыток код сгорает,
        // нужен новый (resendCode / forgotPassword).
        database_.db().deleteAuthTokens(userId, purpose);
        clearKey(key);
        SELFVOY_LOG(log::Level::Warn, "auth") << "код " << purpose << " user=" << userId
                                           << " аннулирован после серии неверных попыток";
        audit(userId, purpose == "email_verify" ? "email_code_burned" : "reset_code_burned", Json::object(), "");
    }
}

AuthManager::Result AuthManager::registerUser(const std::string& email,
                                              const std::string& password,
                                              const std::string& displayName,
                                              const std::string& device,
                                              const std::string& remoteAddr) {
    (void)device;  // сессия при регистрации не создаётся — сначала подтверждение email
    const std::string normalized = normalizeEmail(email);
    const std::string ip = ipKey(remoteAddr);
    if (overLimit(ip, config_.maxIpAttempts) || overLimit("login:" + normalized, config_.maxLoginAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const std::string registerKey = "reg:" + Session::ipOf(remoteAddr);
    if (overLimit(registerKey, config_.registerPerIpPerHour)) {
        return Result::failure(protocol::code::kForbidden,
                               "слишком много регистраций с этого адреса, попробуйте позже");
    }
    if (!plausibleEmail(normalized)) {
        return Result::failure(protocol::code::kBadRequest, "некорректный email");
    }
    if (const std::string error = passwordError(password); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }
    // Занятый email отсекаем ДО Argon2id: дубликаты не жгут 64 МиБ и CPU.
    if (database_.db().findUserByEmail(normalized)) {
        return Result::failure(protocol::code::kConflict, "пользователь с таким email уже существует");
    }
    const std::string name = trim(displayName).empty() ? normalized.substr(0, normalized.find('@'))
                                                       : trim(displayName);
    if (config_.registerPerIpPerHour > 0) hit(registerKey, 3600);

    const std::string hash = crypto::hashPassword(password);
    if (hash.empty()) return Result::failure(protocol::code::kInternal, "не удалось хэшировать пароль");
    long long userId = 0;
    const DatabaseError created = database_.db().createUser(normalized, hash, name, userId);
    if (!created.ok) {
        return Result::failure(protocol::code::kConflict, "пользователь с таким email уже существует");
    }

    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kInternal, "пользователь не создан");

    const std::string code = issueAuthCode(*user, "email_verify", remoteAddr);
    if (code.empty()) {
        return Result::failure(protocol::code::kInternal, "не удалось выпустить код подтверждения");
    }

    SELFVOY_LOG(log::Level::Info, "auth") << "регистрация " << normalized << " (id=" << userId
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

AuthManager::Result AuthManager::verifyEmail(const std::string& email, const std::string& code,
                                             const std::string& remoteAddr) {
    Result result;
    const std::string normalized = normalizeEmail(email);
    const std::string ip = ipKey(remoteAddr);
    if (overLimit(ip, config_.maxIpAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserByEmail(normalized);
    // Ошибка одинакова для «нет пользователя» и «неверный код».
    if (!user || code.empty()) {
        hit(ip, config_.loginWindowSec);
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    if (user->emailVerified) {
        // Идемпотентно: повторное подтверждение не ошибка. Но без кода — никаких
        // данных пользователя: иначе по одному email отдавались бы имя, id и роль.
        result.ok = true;
        result.payload.set("verified", Json(true));
        return result;
    }

    const auto token = database_.db().findAuthToken("email_verify", scopedCodeHash(user->id, "email_verify", code));
    if (!token || token->userId != user->id || !tokenUsable(token->usedAt, token->expiresAt)) {
        codeFailure(user->id, "email_verify", ip);
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    if (!database_.db().useAuthToken(token->id).ok) {
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    database_.db().deleteAuthTokens(user->id, "email_verify");
    database_.db().setEmailVerified(user->id, true);
    clearKey("code:email_verify:" + std::to_string(user->id));
    audit(user->id, "email_verified", Json::object(), remoteAddr);

    SELFVOY_LOG(log::Level::Info, "auth") << "email подтверждён: " << normalized;
    result.ok = true;
    result.userId = user->id;
    result.payload.set("verified", Json(true));
    result.payload.set("user", Json(user->toJson()));
    return result;
}

AuthManager::Result AuthManager::resendVerification(const std::string& email,
                                                    const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    const std::string mailIp = "mailip:" + Session::ipOf(remoteAddr);
    if (overLimit(mailIp, config_.mailPerIpPerHour)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    hit(mailIp, 3600);  // считаем запросы, чтобы нельзя было флудить письмами

    Result result;
    result.ok = true;  // ответ не раскрывает, существует ли email
    result.payload.set("message", Json("если email зарегистрирован и не подтверждён, код отправлен"));
    const auto user = database_.db().findUserByEmail(normalized);
    if (user && !user->emailVerified) {
        // Лимит на адрес получателя: не больше N писем в час одному человеку,
        // с скольких бы IP ни приходили запросы (ответ при этом тот же).
        const std::string mailKey = "mail:" + normalized;
        if (overLimit(mailKey, config_.mailPerEmailPerHour)) {
            SELFVOY_LOG(log::Level::Warn, "auth") << "resendCode: лимит писем для " << normalized;
            return result;
        }
        hit(mailKey, 3600);
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
    const std::string ip = ipKey(remoteAddr);
    const std::string loginKey = "login:" + normalized;
    if (overLimit(ip, config_.maxIpAttempts) || overLimit(loginKey, config_.maxLoginAttempts)) {
        SELFVOY_LOG(log::Level::Warn, "auth") << "вход заблокирован по частоте: " << remoteAddr;
        audit(0, "login_blocked", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserByEmail(normalized);
    if (!user) {
        // Не раскрываем, существует ли пользователь: ошибка одинаковая.
        hit(ip, config_.loginWindowSec);
        hit(loginKey, config_.loginWindowSec);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    // Сначала пароль, потом статус аккаунта: иначе «аккаунт отключён» по
    // любому паролю раскрывал бы существование и блокировку адреса.
    if (!crypto::verifyPassword(password, user->passwordHash)) {
        hit(ip, config_.loginWindowSec);
        hit(loginKey, config_.loginWindowSec);
        SELFVOY_LOG(log::Level::Warn, "auth") << "неудачный вход " << normalized << " из " << remoteAddr;
        audit(user->id, "login_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    if (!user->active) return Result::failure(protocol::code::kForbidden, "аккаунт отключён");
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
            SELFVOY_LOG(log::Level::Info, "auth") << "хэш пароля " << normalized << " мигрирован на Argon2id";
            audit(user->id, "password_rehashed", Json::object(), remoteAddr);
        }
    }

    // Верный пароль обнуляет только счётчик паролей этого email. Счётчики IP и
    // 2FA не трогаем: иначе чередование «верный пароль / неверный TOTP»
    // позволяло перебирать коды без ограничений.
    clearKey(loginKey);

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

    SELFVOY_LOG(log::Level::Info, "auth") << "вход " << normalized << " из " << remoteAddr;
    Result result = issue(*user, device, remoteAddr);
    audit(user->id, "login_ok", Json::object(), remoteAddr);
    return result;
}

AuthManager::Result AuthManager::loginOAuthUser(const UserRecord& user,
                                               const std::string& device,
                                               const std::string& remoteAddr) {
    if (!user.active) {
        return Result::failure(protocol::code::kForbidden, "аккаунт заблокирован");
    }
    SELFVOY_LOG(log::Level::Info, "auth") << "oauth вход " << user.email << " из " << remoteAddr;
    Result result = issue(user, device, remoteAddr);
    audit(user.id, "oauth_login_ok", Json::object(), remoteAddr);
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

    // Повторное использование отозванного токена = вероятная кража:
    // отозвать всё семейство и все сессии пользователя.
    auto reuseDetected = [&]() {
        SELFVOY_LOG(log::Level::Warn, "auth")
            << "повторное использование refresh-токена (user=" << record->userId
            << ") — семейство и сессии отозваны";
        database_.db().revokeRefreshFamily(record->familyId);
        database_.db().revokeAllSessions(record->userId, "");
        audit(record->userId, "refresh_reuse_detected", Json::object(), remoteAddr);
        Result failure = Result::failure(protocol::code::kUnauthorized, "refresh-токен недействителен");
        failure.userId = record->userId;
        failure.sessionsRevoked = true;
        return failure;
    };

    if (!record->revokedAt.empty()) {
        // Заменённый при ротации токен предъявлен снова — кража. Токен,
        // отозванный выходом/отзывом сессии, просто недействителен (иначе
        // запоздалый refresh после logout выбивал бы все устройства).
        if (!record->replacedBy.empty()) return reuseDetected();
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен отозван");
    }
    if (!tokenUsable("", record->expiresAt)) {
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен истёк");
    }

    const auto user = database_.db().findUserById(record->userId);
    if (!user || !user->active || !user->emailVerified) {
        return Result::failure(protocol::code::kUnauthorized, "refresh-токен недействителен");
    }

    // Ротация: старый токен атомарно помечается заменённым (UPDATE … WHERE
    // revoked_at IS NULL). Из двух одновременных запросов с одним токеном
    // проходит только один, второй считается повторным использованием.
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
    if (!database_.db().revokeRefreshToken(record->id, fresh.id).ok) return reuseDetected();
    const DatabaseError stored = database_.db().createRefreshToken(fresh);
    if (!stored.ok) {
        return Result::failure(protocol::code::kInternal, "не удалось выпустить refresh-токен");
    }

    Result result = issueAccess(*user, device.empty() ? record->device : device, remoteAddr, record->familyId);
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
    const long long subject = verified.claims.getInt("sub");
    const auto session = database_.db().findSessionByJwt(jwtId);
    if (!session) return Result::failure(protocol::code::kUnauthorized, "сессия не найдена или отозвана");
    // Сессия принадлежит тому, кому выдана: подменённый sub при чужом/своём
    // jti больше не превращает токен в доступ к другому аккаунту.
    if (session->userId != subject) {
        SELFVOY_LOG(log::Level::Warn, "auth") << "токен: sub не совпадает с владельцем сессии";
        return Result::failure(protocol::code::kUnauthorized, "токен недействителен");
    }

    const auto user = database_.db().findUserById(subject);
    if (!user) return Result::failure(protocol::code::kUnauthorized, "пользователь не найден");
    if (!user->active) return Result::failure(protocol::code::kUnauthorized, "аккаунт отключён");

    Result result;
    result.ok = true;
    result.userId = user->id;
    result.displayName = user->displayName;
    result.email = user->email;
    result.jwtId = jwtId;
    result.payload.set("user", Json(user->toJson()));
    return result;
}

AuthManager::Result AuthManager::checkSession(long long userId, const std::string& jwtId) {
    const auto session = database_.db().findSessionByJwt(jwtId);
    if (!session || session->userId != userId) {
        return Result::failure(protocol::code::kUnauthorized, "сессия истекла или отозвана — войдите снова");
    }
    const auto user = database_.db().findUserById(userId);
    if (!user || !user->active) return Result::failure(protocol::code::kUnauthorized, "аккаунт отключён");
    Result result;
    result.ok = true;
    result.userId = userId;
    result.jwtId = jwtId;
    return result;
}

AuthManager::Result AuthManager::logout(const std::string& jwtId) {
    database_.db().revokeSession(jwtId);
    // Выход отзывает и refresh-токены этого устройства: иначе после выхода
    // по refresh_token можно было снова получить доступ.
    database_.db().revokeRefreshFamily(familyOfJwt(jwtId));
    Result result;
    result.ok = true;
    result.jwtId = jwtId;
    result.payload.set("revoked", Json(true));
    return result;
}

AuthManager::Result AuthManager::changePassword(long long userId,
                                                const std::string& jwtId,
                                                const std::string& oldPassword,
                                                const std::string& newPassword,
                                                const std::string& remoteAddr) {
    const std::string pwKey = "pw:" + std::to_string(userId);
    if (overLimit(pwKey, config_.maxLoginAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kUnauthorized, "пользователь не найден");
    if (!crypto::verifyPassword(oldPassword, user->passwordHash)) {
        hit(pwKey, config_.loginWindowSec);
        audit(userId, "password_change_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kForbidden, "неверный текущий пароль");
    }
    if (const std::string error = passwordError(newPassword); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }
    const std::string hash = crypto::hashPassword(newPassword);
    if (hash.empty()) return Result::failure(protocol::code::kInternal, "не удалось хэшировать пароль");
    const DatabaseError updated = database_.db().updatePasswordHash(userId, hash);
    if (!updated.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить пароль");
    clearKey(pwKey);

    // Пароль сменился — все сессии и refresh-токены (включая текущие)
    // отзываются, а текущее устройство сразу получает новую пару токенов:
    // иначе оно «вылетало» при следующем auth.refresh.
    std::string device = "qt-client";
    if (const auto current = database_.db().findSessionByJwt(jwtId)) device = current->device;
    database_.db().revokeAllSessions(userId, "");
    database_.db().revokeAllUserRefreshTokens(userId);
    audit(userId, "password_changed", Json::object(), remoteAddr);
    SELFVOY_LOG(log::Level::Info, "auth") << "смена пароля user=" << userId;

    Result result = issue(*user, device, remoteAddr);
    result.sessionsRevoked = true;
    result.payload.set("changed", Json(true));
    return result;
}

AuthManager::Result AuthManager::forgotPassword(const std::string& email,
                                                const std::string& remoteAddr) {
    const std::string normalized = normalizeEmail(email);
    const std::string mailIp = "mailip:" + Session::ipOf(remoteAddr);
    if (overLimit(mailIp, config_.mailPerIpPerHour)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    hit(mailIp, 3600);  // анти-флуд

    Result result;
    result.ok = true;  // ответ не раскрывает, существует ли email
    result.payload.set("message", Json("если email зарегистрирован, код сброса отправлен"));
    const auto user = database_.db().findUserByEmail(normalized);
    if (user) {
        const std::string mailKey = "mail:" + normalized;
        if (overLimit(mailKey, config_.mailPerEmailPerHour)) {
            SELFVOY_LOG(log::Level::Warn, "auth") << "forgotPassword: лимит писем для " << normalized;
            return result;
        }
        hit(mailKey, 3600);
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
    const std::string ip = ipKey(remoteAddr);
    if (overLimit(ip, config_.maxIpAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    // Слабый новый пароль отклоняем до проверки кода: ответ не должен
    // подсказывать, верен ли код, не расходуя попытку.
    if (const std::string error = passwordError(newPassword); !error.empty()) {
        return Result::failure(protocol::code::kBadRequest, error);
    }
    const auto user = database_.db().findUserByEmail(normalized);
    // Ошибка одинакова для «нет пользователя» и «неверный код».
    if (!user || code.empty()) {
        hit(ip, config_.loginWindowSec);
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    const auto token =
        database_.db().findAuthToken("password_reset", scopedCodeHash(user->id, "password_reset", code));
    if (!token || token->userId != user->id || !tokenUsable(token->usedAt, token->expiresAt)) {
        audit(user->id, "password_reset_failed", Json::object(), remoteAddr);
        codeFailure(user->id, "password_reset", ip);
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }
    // Код одноразовый: помечаем использованным до смены пароля (атомарно).
    if (!database_.db().useAuthToken(token->id).ok) {
        return Result::failure(protocol::code::kBadRequest, "код неверен или истёк");
    }

    const std::string hash = crypto::hashPassword(newPassword);
    if (hash.empty()) return Result::failure(protocol::code::kInternal, "не удалось хэшировать пароль");
    const DatabaseError updated = database_.db().updatePasswordHash(user->id, hash);
    if (!updated.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить пароль");

    database_.db().deleteAuthTokens(user->id, "password_reset");
    clearKey("code:password_reset:" + std::to_string(user->id));
    clearKey("login:" + normalized);
    // Сброс пароля обрывает все сессии и refresh-токены.
    database_.db().revokeAllSessions(user->id, "");
    database_.db().revokeAllUserRefreshTokens(user->id);
    audit(user->id, "password_reset", Json::object(), remoteAddr);
    SELFVOY_LOG(log::Level::Info, "auth") << "сброс пароля " << normalized;

    Result result;
    result.ok = true;
    result.userId = user->id;
    result.sessionsRevoked = true;
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
    std::string jwtId;
    for (const auto& session : database_.db().listSessions(userId)) {
        if (session.id == sessionId) jwtId = session.jwtId;
    }
    const DatabaseError revoked = database_.db().revokeSessionById(userId, sessionId);
    if (!revoked.ok) return Result::failure(protocol::code::kNotFound, "сессия не найдена");
    // Вместе с сессией — refresh-токены её устройства (иначе отозванное
    // устройство тут же получало новый доступ через auth.refresh).
    database_.db().revokeRefreshFamily(familyOfJwt(jwtId));
    Json detail = Json::object();
    detail.set("session_id", Json(sessionId));
    audit(userId, "session_revoked", detail, "");

    Result result;
    result.ok = true;
    result.userId = userId;
    result.jwtId = jwtId;
    result.payload.set("revoked", Json(true));
    return result;
}

AuthManager::Result AuthManager::revokeAllSessions(long long userId, const std::string& currentJwtId) {
    const DatabaseError revoked = database_.db().revokeAllSessions(userId, currentJwtId);
    if (!revoked.ok) return Result::failure(protocol::code::kInternal, "не удалось отозвать сессии");
    database_.db().revokeUserRefreshTokensExcept(userId, familyOfJwt(currentJwtId));
    audit(userId, "sessions_revoked_all", Json::object(), "");

    Result result;
    result.ok = true;
    result.userId = userId;
    result.jwtId = currentJwtId;
    result.sessionsRevoked = true;
    result.payload.set("revoked", Json(true));
    return result;
}

// --- Двухфакторная аутентификация (TOTP) ------------------------------------

// AAD TOTP-секретов: старое значение сохранено для совместимости с уже сохранёнными в БД данными.
constexpr const char* kTwoFactorAad = "aura-2fa-secret";

std::string AuthManager::twoFactorKeyBytes() const {
    // В продакшене — отдельный ключ SELFVOY_2FA_KEY; в демо/тестах выводим из
    // jwtSecret, чтобы функция работала без отдельной настройки. SHA-256 даёт
    // ровно 32 байта — ключ AES-256.
    const std::string base = config_.twoFactorKey.empty() ? config_.jwtSecret : config_.twoFactorKey;
    return crypto::sha256(base);
}

bool AuthManager::decryptTwoFactorSecret(const TwoFactorRecord& record, std::string& outRaw) const {
    std::string blob;
    if (!crypto::base64Decode(record.secretEncrypted, blob)) return false;
    return crypto::aes256GcmDecrypt(twoFactorKeyBytes(), blob, kTwoFactorAad, outRaw);
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
    // Включённую 2FA нельзя молча перезаписать новым (выключенным) секретом —
    // это обходило бы проверку пароля в auth.disable2fa.
    if (const auto existing = database_.db().findTwoFactor(userId); existing && existing->enabled) {
        return Result::failure(protocol::code::kBadRequest,
                               "2FA уже включена — чтобы перенастроить, сначала отключите её (auth.disable2fa)");
    }
    const std::string secretB32 = totp::generateSecretBase32();
    std::string secretRaw;
    if (!totp::decodeSecretBase32(secretB32, secretRaw)) {
        return Result::failure(protocol::code::kInternal, "не удалось сгенерировать секрет");
    }
    std::string blob;
    if (!crypto::aes256GcmEncrypt(twoFactorKeyBytes(), secretRaw, kTwoFactorAad, blob)) {
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
    result.payload.set("otpauth_uri", Json(totp::otpauthUri(secretB32, account, "Selfvoy")));
    result.payload.set("issuer", Json("Selfvoy"));
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
    const std::string twoFactorKey = "2fa:" + std::to_string(userId);
    if (overLimit(twoFactorKey, config_.maxTwoFactorAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много неверных кодов, попробуйте позже");
    }
    std::string secretRaw;
    if (!decryptTwoFactorSecret(*record, secretRaw)) {
        return Result::failure(protocol::code::kInternal, "не удалось прочитать секрет");
    }
    std::int64_t matched = 0;
    if (!totp::verify(secretRaw, code, record->lastUsedCounter, matched)) {
        hit(twoFactorKey, config_.twoFactorWindowSec);
        audit(userId, "2fa_confirm_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный код");
    }
    clearKey(twoFactorKey);
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
    const std::string pwKey = "pw:" + std::to_string(userId);
    if (overLimit(pwKey, config_.maxLoginAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserById(userId);
    if (!user) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    if (!crypto::verifyPassword(password, user->passwordHash)) {
        hit(pwKey, config_.loginWindowSec);
        audit(userId, "2fa_disable_failed", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kUnauthorized, "неверный пароль");
    }
    clearKey(pwKey);
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
    const std::string ip = ipKey(remoteAddr);
    const std::string loginKey = "login:" + normalized;
    if (overLimit(ip, config_.maxIpAttempts) || overLimit(loginKey, config_.maxLoginAttempts)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте позже");
    }
    const auto user = database_.db().findUserByEmail(normalized);
    if (!user || !crypto::verifyPassword(password, user->passwordHash)) {
        hit(ip, config_.loginWindowSec);
        hit(loginKey, config_.loginWindowSec);
        return Result::failure(protocol::code::kUnauthorized, "неверный email или пароль");
    }
    if (!user->active) return Result::failure(protocol::code::kForbidden, "аккаунт отключён");
    if (!user->emailVerified) {
        return Result::failure("email_not_verified", "подтвердите email — мы отправили код");
    }
    const auto record = database_.db().findTwoFactor(user->id);
    if (!record || !record->enabled) {
        return Result::failure(protocol::code::kBadRequest, "2FA не включена для этого аккаунта");
    }
    // Отдельный счётчик неверных кодов на аккаунт: его не обнуляет ни верный
    // пароль, ни переподключение — перебор TOTP ограничен N попытками за окно.
    const std::string twoFactorKey = "2fa:" + std::to_string(user->id);
    if (overLimit(twoFactorKey, config_.maxTwoFactorAttempts)) {
        audit(user->id, "2fa_blocked", Json::object(), remoteAddr);
        return Result::failure(protocol::code::kForbidden, "слишком много неверных кодов, попробуйте позже");
    }
    // Резервный код (с дефисом, формат XXXXX-XXXXX) или 6-значный TOTP-код.
    const bool isRecovery = code.find('-') != std::string::npos;
    if (isRecovery) {
        const auto rc = database_.db().findRecoveryCode(user->id, codeHash(code));
        if (!rc) {
            hit(twoFactorKey, config_.twoFactorWindowSec);
            hit(ip, config_.loginWindowSec);
            audit(user->id, "2fa_recovery_failed", Json::object(), remoteAddr);
            return Result::failure(protocol::code::kUnauthorized, "неверный код");
        }
        // Одноразовость: два параллельных входа с одним кодом — пройдёт только тот,
        // кто первым пометил код использованным.
        if (!database_.db().useRecoveryCode(rc->id).ok) {
            hit(twoFactorKey, config_.twoFactorWindowSec);
            return Result::failure(protocol::code::kUnauthorized, "неверный код");
        }
        audit(user->id, "2fa_recovery_used", Json::object(), remoteAddr);
    } else {
        std::string secretRaw;
        if (!decryptTwoFactorSecret(*record, secretRaw)) {
            return Result::failure(protocol::code::kInternal, "не удалось прочитать секрет");
        }
        std::int64_t matched = 0;
        if (!totp::verify(secretRaw, code, record->lastUsedCounter, matched)) {
            hit(twoFactorKey, config_.twoFactorWindowSec);
            hit(ip, config_.loginWindowSec);
            audit(user->id, "2fa_failed", Json::object(), remoteAddr);
            return Result::failure(protocol::code::kUnauthorized, "неверный код");
        }
        database_.db().updateTwoFactorCounter(user->id, matched);
    }
    clearKey(twoFactorKey);
    clearKey(loginKey);
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
    SELFVOY_LOG(log::Level::Info, "auth") << "вход (2FA) " << normalized << " из " << remoteAddr;
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

}  // namespace selfvoy
