// aura/authmanager.h — регистрация, вход, JWT, refresh-токены, управление сессиями.
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

#include "aura/config.h"
#include "aura/databasemanager.h"
#include "aura/json.h"
#include "aura/mailprovider.h"

namespace aura {

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

    // Подтверждение email одноразовым кодом.
    Result verifyEmail(const std::string& email, const std::string& code);
    // Повторная отправка кода (ответ не раскрывает существование email).
    Result resendVerification(const std::string& email, const std::string& remoteAddr);

    Result login(const std::string& email,
                 const std::string& password,
                 const std::string& device,
                 const std::string& deviceId,
                 const std::string& remoteAddr);

    // Обмен refresh-токена на новую пару access+refresh (ротация).
    // Повторное использование отозванного токена отзывает всё семейство.
    Result refresh(const std::string& refreshToken,
                   const std::string& device,
                   const std::string& remoteAddr);

    // Проверяет JWT и наличие живой сессии в таблице Sessions.
    Result verifyToken(const std::string& token);

    Result logout(const std::string& jwtId);

    // Смена пароля: проверяет старый, отзывает все прочие сессии.
    Result changePassword(long long userId,
                          const std::string& jwtId,
                          const std::string& oldPassword,
                          const std::string& newPassword);

    // Запрос кода сброса пароля (ответ всегда «ок» — без раскрытия email).
    Result forgotPassword(const std::string& email, const std::string& remoteAddr);
    // Сброс пароля по коду: отзывает все сессии и refresh-токены.
    Result resetPassword(const std::string& email,
                         const std::string& code,
                         const std::string& newPassword,
                         const std::string& remoteAddr);

    // Управление сессиями.
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
    static std::string passwordError(const std::string& password);
    std::string issueToken(long long userId, const std::string& email, std::string& jwtId) const;

private:
    // Выдаёт access-токен + сессию + refresh-токен нового семейства.
    Result issue(const UserRecord& user, const std::string& device, const std::string& remoteAddr);
    // Выдаёт access-токен + сессию; refresh-токен создаёт вызывающий (ротация).
    Result issueAccess(const UserRecord& user, const std::string& device, const std::string& remoteAddr);

    void audit(long long userId, const std::string& kind, const Json& detail,
               const std::string& remoteAddr);
    // Создаёт одноразовый код и «отправляет» письмо. Возвращает код.
    std::string issueAuthCode(const UserRecord& user, const std::string& purpose,
                              const std::string& remoteAddr);

    // --- 2FA: вспомогательные -------------------------------------------------
    // 32-байтный ключ AES-256-GCM: SHA-256 от AURA_2FA_KEY (или jwtSecret в демо).
    std::string twoFactorKeyBytes() const;
    // Генерирует 10 резервных кодов, сохраняет их хэши, возвращает открытые коды.
    std::vector<std::string> generateRecoveryCodes(long long userId);
    // Расшифровывает TOTP-секрет из записи. false — если не удалось.
    bool decryptTwoFactorSecret(const TwoFactorRecord& record, std::string& outRaw) const;

    // --- ограничение частоты попыток (защита от перебора и флуда) ----------
    struct Attempts {
        int count = 0;
        std::chrono::steady_clock::time_point windowStart;
    };

    // true — запрос нужно отклонить как слишком частый
    bool throttled(const std::string& key);
    void registerFailure(const std::string& remoteAddr, const std::string& email);
    void clearFailures(const std::string& remoteAddr, const std::string& email);

    DatabaseManager& database_;
    const Config& config_;
    std::unique_ptr<MailProvider> mail_;
    std::mutex attemptsMutex_;
    std::unordered_map<std::string, Attempts> attempts_;
};

}  // namespace aura
