#pragma once

#include <cstdint>
#include <string>

// TOTP (RFC 6238): 6 цифр, период 30 секунд, HMAC-SHA1 — параметры,
// совместимые со всеми распространёнными приложениями-аутентификаторами.
namespace aura::totp {

constexpr int kDigits = 6;
constexpr int kPeriod = 30;
constexpr int kSecretBytes = 20;  // 160-битный секрет (рекомендация RFC 4226)

// Случайный секрет: base32-строка для ввода в приложение-аутентификатор.
std::string generateSecretBase32();

// Декодирование секрета, введённого вручную: регистр не важен, пробелы и
// дефисы игнорируются, паддинг необязателен. false — недопустимые символы
// или длина результата вне диапазона 10..64 байта.
bool decodeSecretBase32(const std::string& text, std::string& outRaw);

// Код для конкретного момента времени (unixTime). Используется и для
// генерации, и для проверки окон — см. RFC 6238, раздел 4.
std::string codeAt(const std::string& secretRaw, std::int64_t unixTime,
                   int digits = kDigits, int period = kPeriod);

// Текущий код.
std::string codeNow(const std::string& secretRaw);

// Проверка с окном ±1 период и защитой от повторного использования:
// кандидаты с counter <= lastUsedCounter пропускаются (одноразовость кода).
// При успехе matchedCounter получает номер принятого счётчика.
bool verifyAt(const std::string& secretRaw, const std::string& code,
              std::int64_t unixTime, std::int64_t lastUsedCounter,
              std::int64_t& matchedCounter);
bool verify(const std::string& secretRaw, const std::string& code,
            std::int64_t lastUsedCounter, std::int64_t& matchedCounter);

// otpauth:// URI (keyuri) — для QR-кода и ручного добавления аккаунта:
// otpauth://totp/Issuer:account?secret=...&issuer=...&algorithm=SHA1...
std::string otpauthUri(const std::string& secretBase32, const std::string& accountName,
                       const std::string& issuer);

}  // namespace aura::totp
