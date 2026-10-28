// selfvoy/authmanager.h — регистрация, вход, JWT, refresh-токены, управление сессиями.
//
// Поток аккаунта (auth v3):
//   регистрация → код подтверждения email → вход → access(JWT, короткий срок)
//   + refresh-токен (ротация по семействам, детект повторного использования).
// Пароли: Argon2id; старые PBKDF2-хэши прозрачно пересчитываются при входе.
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/json.h"
#include "selfvoy/mailprovider.h"

namespace selfvoy {

class AuthManager {
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
        // true — операция отозвала сессии пользователя (кроме jwtId, если он
        // задан): серверу нужно «выбить» открытые соединения (H8).
        bool sessionsRevoked = false;

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    AuthManager(DatabaseManager& database, const Config& config);

    // Регистрация: создаёт неподтверждённого пользователя и отправляет код.
    // Сессию НЕ выдаёт — вход возможен после verifyEmail.
    Result registerUser(const std::string& email,
                        const std::string& password,
                        const std::string& displayName,
                        const std::string& device,
                        const std::string& remoteAddr);

    // Подтверждение email одноразовым кодом (5 неверных попыток — код сгорает).
    Result verifyEmail(const std::string& email, const std::string& code,
                       const std::string& remoteAddr = "");
    // Повторная отправка кода (ответ не раскрывает существование email).
    Result resendVerification(const std::string& email, const std::string& remoteAddr);

    Result login(const std::string& email,
                 const std::string& password,
                 const std::string& device,
                 const std::string& deviceId,
                 const std::string& remoteAddr);

    // Вход авторизованного через OAuth пользователя (без ввода пароля).
    Result loginOAuthUser(const UserRecord& user,
                          const std::string& device,
                          const std::string& remoteAddr);

    // Обмен refresh-токена на новую пару access+refresh (ротация).
    // Повторное использование отозванного токена отзывает всё семейство.
    Result refresh(const std::string& refreshToken,
                   const std::string& device,
                   const std::string& remoteAddr);

    // Проверяет JWT, живую сессию в таблице Sessions (её владелец должен
    // совпадать с sub токена) и что аккаунт не отключён.
    Result verifyToken(const std::string& token);

    // Перепроверка уже авторизованного соединения: сессия не отозвана и не
    // истекла, пользователь активен (вызывается сервером периодически).
    Result checkSession(long long userId, const std::string& jwtId);

    // Выход: отзывает сессию и refresh-токены её семейства.
    Result logout(const std::string& jwtId);

    // Смена пароля: проверяет старый, отзывает ВСЕ сессии и refresh-токены и
    // выдаёт текущему устройству новую пару token+refresh_token (result.jwtId —
    // новая сессия, ей серверу нужно переавторизовать соединение).
    Result changePassword(long long userId,
                          const std::string& jwtId,
                          const std::string& oldPassword,
                          const std::string& newPassword,
                          const std::string& remoteAddr = "");

    // Запрос кода сброса пароля (ответ всегда «ок» — без раскрытия email).
    Result forgotPassword(const std::string& email, const std::string& remoteAddr);
    // Сброс пароля по коду: отзывает все сессии и refresh-токены.
    Result resetPassword(const std::string& email,
                         const std::string& code,
                         const std::string& newPassword,
                         const std::string& remoteAddr);

    // Управление сессиями. Отзыв сессии отзывает и refresh-токены её
    // семейства (result.jwtId — jti отозванной сессии); revokeAll оставляет
    // живыми только текущую сессию и её refresh-семейство.
    Result listSessions(long long userId, const std::string& currentJwtId);
    Result revokeSession(long long userId, const std::string& sessionId);
    Result revokeAllSessions(long long userId, const std::string& currentJwtId);

    // --- Двухфакторная аутентификация (TOTP, RFC 6238) -----------------------
    // Порядок: пароль → TOTP → сессия. Секрет хранится зашифрованным (AES-256-GCM).

    // Генерирует секрет и возвращает его (base32) + otpauth-URI для ввода в
    // приложение-аутентификатор. 2FA остаётся выключенной до confirm2fa.
    Result setup2fa(long long userId, const std::string& remoteAddr);
    // Проверяет одноразовый код, включает 2FA и выдаёт резервные коды (одноразово).
    Result confirm2fa(long long userId, const std::string& code, const std::string& remoteAddr);
    // Отключение 2FA — только после повторной проверки пароля.
    Result disable2fa(long long userId, const std::string& password, const std::string& remoteAddr);
    // Статус: включена ли 2FA, есть ли незавершённая настройка, сколько кодов осталось.
    Result twoFactorStatus(long long userId);
    // Завершение входа: пароль + TOTP-код (или резервный код). trustDevice помечает
    // устройство доверенным на 90 дней.
    Result login2fa(const std::string& email, const std::string& password, const std::string& code,
                    bool trustDevice, const std::string& deviceId, const std::string& device,
                    const std::string& remoteAddr);
    // Доверенные устройства: список / отзыв одного / отзыв всех.
    Result listTrustedDevices(long long userId);
    Result revokeTrustedDevice(long long userId, const std::string& id, const std::string& remoteAddr);
    Result revokeAllTrustedDevices(long long userId, const std::string& remoteAddr);

    // Вспомогательные проверки (открыты для тестов).
    static std::string normalizeEmail(const std::string& email);
    // Пароль: 8+ символов (кодовых точек UTF-8), хотя бы одна буква (латиница,
    // Latin-1/расширенная латиница, греческий, кириллица) и цифра, до 256 байт.
    static std::string passwordError(const std::string& password);
    // jti = <family_id refresh-семейства>.<случайное>: по jti выхода/отзыва
    // находится семейство refresh-токенов этого устройства.
    std::string issueToken(long long userId, const std::string& email, std::string& jwtId,
                           const std::string& familyId = "") const;
    static std::string familyOfJwt(const std::string& jwtId);
    // Размер таблицы счётчиков попыток (для тестов: не растёт бесконечно).
    std::size_t attemptsSize();

private:
    // Выдаёт access-токен + сессию + refresh-токен нового семейства.
    Result issue(const UserRecord& user, const std::string& device, const std::string& remoteAddr);
    // Выдаёт access-токен + сессию; refresh-токен создаёт вызывающий (ротация).
    Result issueAccess(const UserRecord& user, const std::string& device, const std::string& remoteAddr,
                       const std::string& familyId);
    // Неверный одноразовый код: считает попытку; после maxCodeAttempts код сгорает.
    void codeFailure(long long userId, const std::string& purpose, const std::string& ipKey);

    void audit(long long userId, const std::string& kind, const Json& detail,
               const std::string& remoteAddr);
    // Создаёт одноразовый код и «отправляет» письмо. Возвращает код.
    std::string issueAuthCode(const UserRecord& user, const std::string& purpose,
                              const std::string& remoteAddr);

    // --- 2FA: вспомогательные -------------------------------------------------
    // 32-байтный ключ AES-256-GCM: SHA-256 от SELFVOY_2FA_KEY (или jwtSecret в демо).
    std::string twoFactorKeyBytes() const;
    // Генерирует 10 резервных кодов, сохраняет их хэши, возвращает открытые коды.
    std::vector<std::string> generateRecoveryCodes(long long userId);
    // Расшифровывает TOTP-секрет из записи. false — если не удалось.
    bool decryptTwoFactorSecret(const TwoFactorRecord& record, std::string& outRaw) const;

    // --- ограничение частоты попыток (защита от перебора и флуда) ----------
    // Ключи: "ip:<ip без порта>", "login:<email>", "2fa:<user>", "pw:<user>",
    // "code:<purpose>:<user>", "mail:<email>", "mailip:<ip>", "reg:<ip>".
    struct Attempts {
        int count = 0;
        int windowSec = 0;
        std::chrono::steady_clock::time_point windowStart;
    };

    // true — по ключу уже limit событий в текущем окне (limit <= 0 — без лимита).
    bool overLimit(const std::string& key, int limit);
    // Засчитать событие; возвращает число событий в текущем окне.
    int hit(const std::string& key, int windowSec);
    void clearKey(const std::string& key);
    void pruneLocked(std::chrono::steady_clock::time_point now);
    static std::string ipKey(const std::string& remoteAddr);

    DatabaseManager& database_;
    const Config& config_;
    std::unique_ptr<MailProvider> mail_;
    static constexpr std::size_t kPruneThreshold = 4096;
    static constexpr std::size_t kMaxAttemptEntries = 100000;
    std::mutex attemptsMutex_;
    std::unordered_map<std::string, Attempts> attempts_;
    std::size_t nextPruneAt_ = kPruneThreshold;
    std::chrono::steady_clock::time_point lastPrune_{};
};

}  // namespace selfvoy
