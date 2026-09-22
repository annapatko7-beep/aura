// aura/crypto.h — SHA-1/SHA-256/HMAC/PBKDF2/Base64 без внешних зависимостей.
//
// OpenSSL в сборке не требуется: всё, что нужно для WebSocket-рукопожатия
// (SHA-1), для JWT (HMAC-SHA256) и для паролей (PBKDF2), реализовано здесь и
// покрыто тестами с эталонными векторами (FIPS 180-4, RFC 4231, RFC 7638).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aura::crypto {

using Bytes = std::vector<std::uint8_t>;

std::string sha1(const std::string& data);             // 20 байт, сырой вид
std::string sha256(const std::string& data);           // 32 байта, сырой вид
std::string hmacSha1(const std::string& key, const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);
std::string pbkdf2Sha256(const std::string& password,
                         const std::string& salt,
                         int iterations,
                         std::size_t length);

std::string base64Encode(const std::string& data);
std::string base64UrlEncode(const std::string& data);
bool base64Decode(const std::string& text, std::string& out);
std::string base32Encode(const std::string& data);     // RFC 4648, с padding '='
// Декодирование base32: регистр не важен, пробелы/дефисы игнорируются,
// паддинг '=' необязателен. false — если встретился недопустимый символ.
bool base32Decode(const std::string& encoded, std::string& out);

std::string toHex(const std::string& data);
std::string randomBytes(std::size_t count);            // криптографически стойкий источник
std::string randomHex(std::size_t bytes);

// --- AEAD: AES-256-GCM (шифрование чувствительных полей) --------------------
// Ключ — ровно 32 байта. Выход формата: nonce(12) || ciphertext || tag(16).
// Реализация собственная (без OpenSSL), сверена с векторами NIST и
// независимой библиотекой (Python cryptography).
bool aes256GcmEncrypt(const std::string& key,
                      const std::string& plaintext,
                      const std::string& aad,
                      std::string& out);
bool aes256GcmDecrypt(const std::string& key,
                      const std::string& blob,
                      const std::string& aad,
                      std::string& out);

// --- хэширование паролей ---------------------------------------------------
// Основной формат: argon2id$v=19$m=<KiB>,t=<итерации>,p=<параллелизм>$<saltHex>$<hashHex>
// (RFC 9106, профиль m=64MiB/t=3/p=4). Старый формат pbkdf2$... понимается
// при проверке: после успешного входа хэш прозрачно пересчитывается в Argon2id
// (миграция — см. AuthManager::login).
std::string argon2idHash(const std::string& password,
                         std::uint32_t memoryKiB = 65536,
                         std::uint32_t iterations = 3,
                         std::uint32_t parallelism = 4);
bool argon2idVerify(const std::string& password, const std::string& stored);
// true, если хэш устарел (pbkdf2) и его нужно пересчитать при следующем входе.
bool passwordNeedsRehash(const std::string& stored);

// Универсальные обёртки: hashPassword теперь выдаёт Argon2id,
// verifyPassword понимает и argon2id$, и pbkdf2$ (прозрачная миграция).
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& stored);

// Сравнение без утечки по времени (для токенов и хэшей).
bool constantTimeEquals(const std::string& left, const std::string& right);

}  // namespace aura::crypto
