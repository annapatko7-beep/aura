// aura/crypto.cpp — реализация хэш-функций и кодирования.
#include "aura/crypto.h"

#include <argon2.h>

#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace aura::crypto {

namespace {

inline std::uint32_t rotl(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

inline std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void appendLengthBe(std::string& buffer, std::uint64_t bitLength) {
    for (int i = 7; i >= 0; --i) {
        buffer.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFF));
    }
}

std::string padMessage(const std::string& data, std::size_t blockSize) {
    std::string padded = data;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % blockSize != blockSize - 8) padded.push_back('\0');
    appendLengthBe(padded, bitLength);
    return padded;
}

const std::uint32_t kSha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

const char* kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

std::string randomSource(std::size_t count) {
    std::string output;
    output.resize(count);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom) {
        urandom.read(&output[0], static_cast<std::streamsize>(count));
        if (static_cast<std::size_t>(urandom.gcount()) == count) return output;
    }
    std::random_device device;
    for (std::size_t i = 0; i < count; ++i) {
        output[i] = static_cast<char>(device() & 0xFF);
    }
    return output;
}

}  // namespace

std::string sha1(const std::string& data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::string padded = padMessage(data, 64);

    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string out;
    out.resize(20);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<char>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<char>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<char>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<char>(h[i] & 0xFF);
    }
    return out;
}

std::string sha256(const std::string& data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::string padded = padMessage(data, 64);

    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + S1 + ch + kSha256[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::string out;
    out.resize(32);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<char>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<char>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<char>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<char>(h[i] & 0xFF);
    }
    return out;
}

std::string hmacSha1(const std::string& key, const std::string& data) {
    const std::size_t blockSize = 64;
    std::string normalizedKey = key;
    if (normalizedKey.size() > blockSize) normalizedKey = sha1(normalizedKey);
    normalizedKey.resize(blockSize, '\0');

    std::string inner(blockSize, '\0');
    std::string outer(blockSize, '\0');
    for (std::size_t i = 0; i < blockSize; ++i) {
        inner[i] = static_cast<char>(normalizedKey[i] ^ 0x36);
        outer[i] = static_cast<char>(normalizedKey[i] ^ 0x5C);
    }
    return sha1(outer + sha1(inner + data));
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    const std::size_t blockSize = 64;
    std::string normalizedKey = key;
    if (normalizedKey.size() > blockSize) normalizedKey = sha256(normalizedKey);
    normalizedKey.resize(blockSize, '\0');

    std::string inner(blockSize, '\0');
    std::string outer(blockSize, '\0');
    for (std::size_t i = 0; i < blockSize; ++i) {
        inner[i] = static_cast<char>(normalizedKey[i] ^ 0x36);
        outer[i] = static_cast<char>(normalizedKey[i] ^ 0x5C);
    }
    return sha256(outer + sha256(inner + data));
}

std::string pbkdf2Sha256(const std::string& password,
                         const std::string& salt,
                         int iterations,
                         std::size_t length) {
    std::string output;
    std::uint32_t blockIndex = 1;
    while (output.size() < length) {
        std::string block = salt;
        block.push_back(static_cast<char>((blockIndex >> 24) & 0xFF));
        block.push_back(static_cast<char>((blockIndex >> 16) & 0xFF));
        block.push_back(static_cast<char>((blockIndex >> 8) & 0xFF));
        block.push_back(static_cast<char>(blockIndex & 0xFF));

        std::string u = hmacSha256(password, block);
        std::string result = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmacSha256(password, u);
            for (std::size_t j = 0; j < result.size(); ++j) {
                result[j] = static_cast<char>(result[j] ^ u[j]);
            }
        }
        output += result;
        ++blockIndex;
    }
    output.resize(length);
    return output;
}

std::string base64Encode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t value = (static_cast<unsigned char>(data[i]) << 16) |
                                    (static_cast<unsigned char>(data[i + 1]) << 8) |
                                    static_cast<unsigned char>(data[i + 2]);
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[value & 0x3F]);
        i += 3;
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t value = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t value = (static_cast<unsigned char>(data[i]) << 16) |
                                    (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64UrlEncode(const std::string& data) {
    std::string encoded = base64Encode(data);
    for (char& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

bool base64Decode(const std::string& text, std::string& out) {
    out.clear();
    unsigned int buffer = 0;   // без знакового переполнения при сдвиге
    int bits = 0;
    bool urlSafe = false;
    bool standard = false;
    bool padding = false;
    for (const char ch : text) {
        if (ch == '=' ) { padding = true; continue; }
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        if (padding) return false;                      // данные после '='
        if (ch == '-' || ch == '_') urlSafe = true;
        if (ch == '+' || ch == '/') standard = true;
        const int value = base64Value(ch);
        if (value < 0) return false;
        buffer = ((buffer << 6) | static_cast<unsigned int>(value)) & 0x00FFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFFu));
        }
    }
    if (urlSafe && standard) return false;              // смешанный алфавит
    return bits < 6;                                    // одиночный «хвост» недопустим
}

std::string base32Encode(const std::string& data) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    std::size_t buffer = 0;
    int bits = 0;
    for (const char ch : data) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(alphabet[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(buffer << (5 - bits)) & 0x1F]);
    while (out.size() % 8 != 0) out.push_back('=');
    return out;
}

bool base32Decode(const std::string& encoded, std::string& out) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    out.clear();
    std::size_t buffer = 0;
    int bits = 0;
    bool paddingStarted = false;
    for (const char ch : encoded) {
        if (ch == ' ' || ch == '-' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        if (ch == '=') { paddingStarted = true; continue; }
        if (paddingStarted) return false;  // данные после паддинга недопустимы
        char upper = ch;
        if (upper >= 'a' && upper <= 'z') upper = static_cast<char>(upper - 'a' + 'A');
        const char* pos = std::strchr(alphabet, upper);
        if (pos == nullptr) return false;
        buffer = (buffer << 5) | static_cast<std::size_t>(pos - alphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string toHex(const std::string& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const char ch : data) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string randomBytes(std::size_t count) { return randomSource(count); }

std::string randomHex(std::size_t bytes) { return toHex(randomSource(bytes)); }

// --- AES-256-GCM (собственная реализация, без OpenSSL) ----------------------
//
// AES-256 (FIPS-197): Nk=8, Nr=14. GCM (SP 800-38D): 96-битная nonce,
// J0 = IV||0^31||1, CTR от inc32(J0), GHASH + тег 16 байт.
// Корректность подтверждена known-answer векторами, вычисленными независимой
// библиотекой (Python cryptography / OpenSSL): см. тест crypto_aes256_gcm.
namespace {

const unsigned char kAesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

inline unsigned char xtime(unsigned char value) {
    return static_cast<unsigned char>((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
}

unsigned char gmul(unsigned char a, unsigned char b) {
    unsigned char result = 0;
    while (b != 0) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

struct AesKeySchedule {
    unsigned char roundKeys[15][16];
};

void aes256ExpandKey(const unsigned char key[32], AesKeySchedule& schedule) {
    static const unsigned char rcon[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    std::uint32_t words[60];
    for (int i = 0; i < 8; ++i) {
        words[i] = (static_cast<std::uint32_t>(key[4 * i]) << 24) |
                   (static_cast<std::uint32_t>(key[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(key[4 * i + 2]) << 8) |
                   static_cast<std::uint32_t>(key[4 * i + 3]);
    }
    for (int i = 8; i < 60; ++i) {
        std::uint32_t temp = words[i - 1];
        if (i % 8 == 0) {
            // RotWord → SubWord → Rcon
            temp = (static_cast<std::uint32_t>(kAesSbox[(temp >> 16) & 0xFF]) << 24) |
                   (static_cast<std::uint32_t>(kAesSbox[(temp >> 8) & 0xFF]) << 16) |
                   (static_cast<std::uint32_t>(kAesSbox[temp & 0xFF]) << 8) |
                   static_cast<std::uint32_t>(kAesSbox[(temp >> 24) & 0xFF]);
            temp ^= static_cast<std::uint32_t>(rcon[i / 8 - 1]) << 24;
        } else if (i % 8 == 4) {
            temp = (static_cast<std::uint32_t>(kAesSbox[(temp >> 24) & 0xFF]) << 24) |
                   (static_cast<std::uint32_t>(kAesSbox[(temp >> 16) & 0xFF]) << 16) |
                   (static_cast<std::uint32_t>(kAesSbox[(temp >> 8) & 0xFF]) << 8) |
                   static_cast<std::uint32_t>(kAesSbox[temp & 0xFF]);
        }
        words[i] = words[i - 8] ^ temp;
    }
    for (int round = 0; round < 15; ++round) {
        for (int column = 0; column < 4; ++column) {
            const std::uint32_t word = words[round * 4 + column];
            schedule.roundKeys[round][4 * column] = static_cast<unsigned char>((word >> 24) & 0xFF);
            schedule.roundKeys[round][4 * column + 1] = static_cast<unsigned char>((word >> 16) & 0xFF);
            schedule.roundKeys[round][4 * column + 2] = static_cast<unsigned char>((word >> 8) & 0xFF);
            schedule.roundKeys[round][4 * column + 3] = static_cast<unsigned char>(word & 0xFF);
        }
    }
}

// Шифрование одного блока. state — байты входа в том же порядке (колонки).
void aesEncryptBlock(const AesKeySchedule& schedule, const unsigned char in[16], unsigned char out[16]) {
    unsigned char state[16];
    std::memcpy(state, in, 16);
    for (int i = 0; i < 16; ++i) state[i] ^= schedule.roundKeys[0][i];

    for (int round = 1; round <= 14; ++round) {
        for (int i = 0; i < 16; ++i) state[i] = kAesSbox[state[i]];  // SubBytes
        unsigned char shifted[16];                                    // ShiftRows
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                shifted[row + 4 * column] = state[row + 4 * ((column + row) % 4)];
            }
        }
        std::memcpy(state, shifted, 16);
        if (round < 14) {  // MixColumns
            for (int column = 0; column < 4; ++column) {
                const unsigned char a0 = state[4 * column];
                const unsigned char a1 = state[4 * column + 1];
                const unsigned char a2 = state[4 * column + 2];
                const unsigned char a3 = state[4 * column + 3];
                state[4 * column] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
                state[4 * column + 1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
                state[4 * column + 2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
                state[4 * column + 3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
            }
        }
        for (int i = 0; i < 16; ++i) state[i] ^= schedule.roundKeys[round][i];  // AddRoundKey
    }
    std::memcpy(out, state, 16);
}

// Умножение в GF(2^128) для GHASH (SP 800-38D, Algorithm 1).
void ghashMul(const unsigned char x[16], const unsigned char y[16], unsigned char out[16]) {
    unsigned char z[16] = {0};
    unsigned char v[16];
    std::memcpy(v, y, 16);
    for (int bit = 0; bit < 128; ++bit) {
        if (x[bit / 8] & (0x80 >> (bit % 8))) {
            for (int i = 0; i < 16; ++i) z[i] ^= v[i];
        }
        const unsigned char lsb = v[15] & 1;
        for (int i = 15; i > 0; --i) v[i] = static_cast<unsigned char>((v[i] >> 1) | ((v[i - 1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    std::memcpy(out, z, 16);
}

void ghashUpdate(unsigned char accumulator[16], const unsigned char h[16],
                 const unsigned char* data, std::size_t length) {
    for (std::size_t offset = 0; offset < length; offset += 16) {
        unsigned char block[16] = {0};
        const std::size_t chunk = length - offset < 16 ? length - offset : 16;
        std::memcpy(block, data + offset, chunk);
        for (int i = 0; i < 16; ++i) accumulator[i] ^= block[i];
        unsigned char next[16];
        ghashMul(accumulator, h, next);
        std::memcpy(accumulator, next, 16);
    }
}

void putBe64(unsigned char out[8], std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<unsigned char>((value >> (56 - 8 * i)) & 0xFF);
}

// Ядро GCM: nonce ровно 12 байт, tag — 16 байт. CTR симметричен, поэтому
// функция работает в обе стороны; GHASH всегда считается по ШИФРТЕКСТУ:
// при шифровании это выход CTR (ghashOverOutput=true), при расшифровании —
// вход (ghashOverOutput=false).
void aesGcmCore(const std::string& key, const std::string& nonce,
                const std::string& input, const std::string& aad,
                std::string& output, bool ghashOverOutput, unsigned char tag[16]) {
    AesKeySchedule schedule;
    aes256ExpandKey(reinterpret_cast<const unsigned char*>(key.data()), schedule);

    unsigned char zeroBlock[16] = {0};
    unsigned char h[16];
    aesEncryptBlock(schedule, zeroBlock, h);

    unsigned char counter[16];
    std::memcpy(counter, nonce.data(), 12);
    counter[12] = 0;
    counter[13] = 0;
    counter[14] = 0;
    counter[15] = 1;  // J0

    output.assign(input.size(), '\0');
    for (std::size_t offset = 0; offset < input.size(); offset += 16) {
        for (int i = 15; i >= 12; --i) {  // inc32
            if (++counter[i] != 0) break;
        }
        unsigned char keystream[16];
        aesEncryptBlock(schedule, counter, keystream);
        const std::size_t chunk = input.size() - offset < 16 ? input.size() - offset : 16;
        for (std::size_t i = 0; i < chunk; ++i) {
            output[offset + i] = static_cast<char>(
                static_cast<unsigned char>(input[offset + i]) ^ keystream[i]);
        }
    }

    const std::string& ciphertext = ghashOverOutput ? output : input;
    unsigned char accumulator[16] = {0};
    ghashUpdate(accumulator, h, reinterpret_cast<const unsigned char*>(aad.data()), aad.size());
    ghashUpdate(accumulator, h, reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size());
    unsigned char lengthBlock[16];
    putBe64(lengthBlock, static_cast<std::uint64_t>(aad.size()) * 8);
    putBe64(lengthBlock + 8, static_cast<std::uint64_t>(ciphertext.size()) * 8);
    for (int i = 0; i < 16; ++i) accumulator[i] ^= lengthBlock[i];
    unsigned char ghash[16];
    ghashMul(accumulator, h, ghash);

    unsigned char mask[16];
    unsigned char j0[16];
    std::memcpy(j0, nonce.data(), 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
    aesEncryptBlock(schedule, j0, mask);
    for (int i = 0; i < 16; ++i) tag[i] = static_cast<unsigned char>(mask[i] ^ ghash[i]);
}

}  // namespace

bool aes256GcmEncrypt(const std::string& key, const std::string& plaintext,
                      const std::string& aad, std::string& out) {
    if (key.size() != 32) return false;
    const std::string nonce = randomBytes(12);
    std::string ciphertext;
    unsigned char tag[16];
    // Шифрование: GHASH считается по выходу CTR (= шифртекст).
    aesGcmCore(key, nonce, plaintext, aad, ciphertext, /*ghashOverOutput=*/true, tag);
    out = nonce + ciphertext + std::string(reinterpret_cast<const char*>(tag), 16);
    return true;
}

bool aes256GcmDecrypt(const std::string& key, const std::string& blob,
                      const std::string& aad, std::string& out) {
    if (key.size() != 32 || blob.size() < 12 + 16) return false;
    const std::string nonce = blob.substr(0, 12);
    const std::string ciphertext = blob.substr(12, blob.size() - 12 - 16);
    const std::string expectedTag = blob.substr(blob.size() - 16);

    std::string plaintext;
    unsigned char tag[16];
    // Расшифрование: CTR симметричен; GHASH — по входу (= шифртекст).
    aesGcmCore(key, nonce, ciphertext, aad, plaintext, /*ghashOverOutput=*/false, tag);
    if (!constantTimeEquals(std::string(reinterpret_cast<const char*>(tag), 16), expectedTag)) {
        return false;  // тег не сошёлся — данные или ключ неверны
    }
    out = plaintext;
    return true;
}

// --- Argon2id (RFC 9106) ----------------------------------------------------

namespace {

// Разбирает хэш вида scheme$param$saltHex$hashHex на 4 части по '$'.
std::vector<std::string> splitDollar(const std::string& stored) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : stored) {
        if (ch == '$') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexValue(hex[i]);
        const int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// Старый PBKDF2-формат — остаётся только для проверки при миграции.
bool verifyPbkdf2(const std::string& password, const std::vector<std::string>& parts) {
    if (parts.size() != 4 || parts[0] != "pbkdf2") return false;
    int iterations = 0;
    try {
        iterations = std::stoi(parts[1]);
    } catch (...) {
        return false;
    }
    // Потолок защищает от испорченной/подменённой записи с астрономическим
    // числом итераций — иначе каждая попытка входа жгла бы процессор.
    constexpr int kMaxIterations = 1000000;
    if (iterations <= 0 || iterations > kMaxIterations) return false;
    const std::string computed = toHex(pbkdf2Sha256(password, parts[2], iterations, 32));
    return constantTimeEquals(computed, parts[3]);
}

}  // namespace

std::string argon2idHash(const std::string& password,
                         std::uint32_t memoryKiB,
                         std::uint32_t iterations,
                         std::uint32_t parallelism) {
    const std::string salt = randomHex(16);  // 16 байт соли (RFC 9106 §4)
    std::string saltRaw;
    if (!hexDecode(salt, saltRaw) || saltRaw.empty()) return "";

    std::string hash(32, '\0');  // 32-байтный тег
    const int rc = argon2id_hash_raw(iterations, memoryKiB, parallelism,
                                     password.data(), password.size(),
                                     saltRaw.data(), saltRaw.size(),
                                     hash.data(), hash.size());
    if (rc != ARGON2_OK) return "";

    std::ostringstream out;
    out << "argon2id$v=19$m=" << memoryKiB << ",t=" << iterations << ",p=" << parallelism
        << "$" << salt << "$" << toHex(hash);
    return out.str();
}

bool argon2idVerify(const std::string& password, const std::string& stored) {
    const auto parts = splitDollar(stored);
    if (parts.size() != 5 || parts[0] != "argon2id") return false;

    // parts[1] = "v=19", parts[2] = "m=..,t=..,p=.."
    std::uint32_t memoryKiB = 0;
    std::uint32_t iterations = 0;
    std::uint32_t parallelism = 0;
    try {
        std::size_t pos = 0;
        for (const char* key : {"m=", "t=", "p="}) {
            const std::size_t at = parts[2].find(key, pos);
            if (at == std::string::npos) return false;
            const std::size_t valueStart = at + 2;
            const std::size_t valueEnd = parts[2].find(',', valueStart);
            const std::string value =
                parts[2].substr(valueStart, valueEnd == std::string::npos ? valueEnd : valueEnd - valueStart);
            const unsigned long parsed = std::stoul(value);
            if (key[0] == 'm') memoryKiB = static_cast<std::uint32_t>(parsed);
            else if (key[0] == 't') iterations = static_cast<std::uint32_t>(parsed);
            else parallelism = static_cast<std::uint32_t>(parsed);
            pos = valueEnd == std::string::npos ? parts[2].size() : valueEnd;
        }
    } catch (...) {
        return false;
    }
    // Разумные потолки против испорченной записи (память ограничиваем 1 GiB).
    if (memoryKiB < 8 || memoryKiB > 1048576 || iterations == 0 || iterations > 100 ||
        parallelism == 0 || parallelism > 64) {
        return false;
    }

    std::string saltRaw;
    std::string expectedHash;
    if (!hexDecode(parts[3], saltRaw) || !hexDecode(parts[4], expectedHash) || expectedHash.empty()) {
        return false;
    }

    std::string computed(expectedHash.size(), '\0');
    const int rc = argon2id_hash_raw(iterations, memoryKiB, parallelism,
                                     password.data(), password.size(),
                                     saltRaw.data(), saltRaw.size(),
                                     computed.data(), computed.size());
    if (rc != ARGON2_OK) return false;
    return constantTimeEquals(computed, expectedHash);
}

bool passwordNeedsRehash(const std::string& stored) {
    return stored.compare(0, 7, "pbkdf2$") == 0;
}

std::string hashPassword(const std::string& password) {
    return argon2idHash(password);
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    if (stored.compare(0, 9, "argon2id$") == 0) return argon2idVerify(password, stored);
    return verifyPbkdf2(password, splitDollar(stored));  // миграция со старого формата
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    }
    return diff == 0;
}

}  // namespace aura::crypto
