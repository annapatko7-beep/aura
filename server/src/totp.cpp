#include "aura/totp.h"

#include "aura/crypto.h"

#include <ctime>

namespace aura::totp {
namespace {

std::uint32_t pow10(int exponent) {
    std::uint32_t result = 1;
    for (int i = 0; i < exponent; ++i) result *= 10;
    return result;
}

std::string percentEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    }
    return out;
}

}  // namespace

std::string generateSecretBase32() {
    return aura::crypto::base32Encode(aura::crypto::randomBytes(kSecretBytes));
}

bool decodeSecretBase32(const std::string& text, std::string& outRaw) {
    std::string raw;
    if (!aura::crypto::base32Decode(text, raw)) return false;
    if (raw.size() < 10 || raw.size() > 64) return false;  // разумные границы секрета
    outRaw = raw;
    return true;
}

std::string codeAt(const std::string& secretRaw, std::int64_t unixTime, int digits, int period) {
    if (secretRaw.empty() || digits < 1 || period < 1) return std::string();
    std::uint64_t counter = 0;
    if (unixTime > 0) {
        counter = static_cast<std::uint64_t>(unixTime) / static_cast<std::uint64_t>(period);
    }
    // Счётчик — 8 байт big-endian (RFC 4226, 5.2).
    unsigned char counterBytes[8];
    for (int i = 7; i >= 0; --i) {
        counterBytes[i] = static_cast<unsigned char>(counter & 0xFF);
        counter >>= 8;
    }
    const std::string mac = aura::crypto::hmacSha1(
        secretRaw, std::string(reinterpret_cast<const char*>(counterBytes), sizeof(counterBytes)));
    if (mac.size() != 20) return std::string();

    // Динамическая отсечка (RFC 4226, 5.3).
    const int offset = static_cast<unsigned char>(mac[19]) & 0x0F;
    const std::uint32_t truncated =
        ((static_cast<std::uint32_t>(static_cast<unsigned char>(mac[offset])) & 0x7F) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(mac[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(mac[offset + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(mac[offset + 3]));

    const std::uint32_t code = truncated % pow10(digits);
    std::string text = std::to_string(code);
    while (static_cast<int>(text.size()) < digits) text = "0" + text;
    return text;
}

std::string codeNow(const std::string& secretRaw) {
    return codeAt(secretRaw, static_cast<std::int64_t>(std::time(nullptr)));
}

bool verifyAt(const std::string& secretRaw, const std::string& code, std::int64_t unixTime,
              std::int64_t lastUsedCounter, std::int64_t& matchedCounter) {
    if (secretRaw.empty() || code.size() != static_cast<std::size_t>(kDigits)) return false;
    for (const char ch : code) {
        if (ch < '0' || ch > '9') return false;
    }
    const std::int64_t current = unixTime > 0 ? unixTime / kPeriod : 0;
    // Окно ±1 период: допускаем небольшой рассинхрон часов клиента.
    for (std::int64_t offset = -1; offset <= 1; ++offset) {
        const std::int64_t candidate = current + offset;
        if (candidate <= lastUsedCounter) continue;  // код уже использован
        if (aura::crypto::constantTimeEquals(codeAt(secretRaw, candidate * kPeriod), code)) {
            matchedCounter = candidate;
            return true;
        }
    }
    return false;
}

bool verify(const std::string& secretRaw, const std::string& code, std::int64_t lastUsedCounter,
            std::int64_t& matchedCounter) {
    return verifyAt(secretRaw, code, static_cast<std::int64_t>(std::time(nullptr)), lastUsedCounter,
                    matchedCounter);
}

std::string otpauthUri(const std::string& secretBase32, const std::string& accountName,
                       const std::string& issuer) {
    std::string uri = "otpauth://totp/" + percentEncode(issuer) + ":" + percentEncode(accountName);
    uri += "?secret=" + percentEncode(secretBase32);
    uri += "&issuer=" + percentEncode(issuer);
    uri += "&algorithm=SHA1";
    uri += "&digits=" + std::to_string(kDigits);
    uri += "&period=" + std::to_string(kPeriod);
    return uri;
}

}  // namespace aura::totp
