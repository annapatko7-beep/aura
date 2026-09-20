// aura/server/tests/test_main.cpp — юнит-тесты сервера без внешних зависимостей.
//
// Запуск: make test  (или ./aura-tests)
//
// Проверяются: JSON, криптография (эталонные векторы), JWT, кодек WebSocket,
// протокол и сквозные сценарии через Server::handleMessage на встроенной БД.
#include <argon2.h>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "aura/authmanager.h"
#include "aura/crypto.h"
#include "aura/idatabase.h"
#include "aura/json.h"
#include "aura/jwt.h"
#include "aura/net.h"
#include "aura/protocol.h"
#include "aura/server.h"
#include "aura/toolmanager.h"
#include "aura/totp.h"
#include "aura/ws.h"

namespace {

int g_failures = 0;
int g_checks = 0;

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

#define TEST(name)                                             \
    void name();                                               \
    static Registrar registrar_##name(#name, name);            \
    void name()

#define CHECK(condition)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(condition)) {                                                         \
            ++g_failures;                                                           \
            std::cout << "  FAIL " << __FILE__ << ':' << __LINE__ << " " << #condition \
                      << "\n";                                                      \
        }                                                                           \
    } while (0)

#define CHECK_EQ(actual, expected)                                                           \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto actualValue = (actual);                                                   \
        const auto expectedValue = (expected);                                               \
        if (!(actualValue == expectedValue)) {                                               \
            ++g_failures;                                                                    \
            std::ostringstream message;                                                      \
            message << actualValue;                                                          \
            std::ostringstream other;                                                        \
            other << expectedValue;                                                          \
            std::cout << "  FAIL " << __FILE__ << ':' << __LINE__ << " " << #actual          \
                      << " == " << #expected << " (получено '" << message.str()              \
                      << "', ожидалось '" << other.str() << "')\n";                          \
        }                                                                                    \
    } while (0)

using aura::Json;

// --------------------------------------------------------------------- JSON
TEST(json_parse_and_dump) {
    std::string error;
    const Json parsed = Json::parse(
        R"({"name":"Анна","age":31,"tags":["a","b"],"nested":{"ok":true,"value":null},"ratio":0.5})", &error);
    CHECK(error.empty());
    CHECK(parsed.isObject());
    CHECK_EQ(parsed.getString("name"), std::string("Анна"));
    CHECK_EQ(parsed.getInt("age"), 31);
    CHECK_EQ(parsed.get("tags").size(), static_cast<std::size_t>(2));
    CHECK(parsed.get("nested").getBool("ok"));
    CHECK(parsed.get("nested").get("value").isNull());
    CHECK_EQ(parsed.getDouble("ratio"), 0.5);

    // Порядок ключей сохраняется
    const std::vector<std::string> keys = parsed.keys();
    CHECK_EQ(keys.front(), std::string("name"));
    CHECK_EQ(keys.back(), std::string("ratio"));

    // Round-trip
    const Json again = Json::parse(parsed.dump());
    CHECK_EQ(again.getString("name"), std::string("Анна"));
}

TEST(json_escapes_and_unicode) {
    std::string error;
    const Json parsed = Json::parse(R"({"text":"line1\nline2 \"quoted\" \u0410\u0411"})", &error);
    CHECK(error.empty());
    CHECK_EQ(parsed.getString("text"), std::string("line1\nline2 \"quoted\" АБ"));

    Json out = Json::object();
    out.set("text", Json("кавычки \" и \\ слэш"));
    const std::string dumped = out.dump();
    CHECK(dumped.find("\\\"") != std::string::npos);
    CHECK(dumped.find("\\\\") != std::string::npos);
    CHECK_EQ(Json::parse(dumped).getString("text"), std::string("кавычки \" и \\ слэш"));
}

TEST(json_errors_and_defaults) {
    std::string error;
    CHECK(Json::parse("{bad}", &error).isNull());
    CHECK(!error.empty());
    error.clear();
    CHECK(Json::parse("[1,2", &error).isNull());

    const Json empty = Json::object();
    CHECK_EQ(empty.getString("missing", "def"), std::string("def"));
    CHECK_EQ(empty.getInt("missing", 7), 7);
    CHECK(!empty.contains("missing"));
}

// ------------------------------------------------------------------ crypto
TEST(crypto_known_vectors) {
    CHECK_EQ(aura::crypto::toHex(aura::crypto::sha256("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(aura::crypto::toHex(aura::crypto::sha256("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(aura::crypto::toHex(aura::crypto::sha1("abc")),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));

    // RFC 4231, тест-кейс 1
    const std::string key(20, '\x0b');
    CHECK_EQ(aura::crypto::toHex(aura::crypto::hmacSha256(key, "Hi There")),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    // RFC 6070-подобный вектор для PBKDF2-HMAC-SHA256: password/salt/1 итерация
    CHECK_EQ(aura::crypto::toHex(aura::crypto::pbkdf2Sha256("password", "salt", 1, 32)),
             std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
}

TEST(crypto_base64) {
    CHECK_EQ(aura::crypto::base64Encode("Man"), std::string("TWFu"));
    CHECK_EQ(aura::crypto::base64Encode("Ma"), std::string("TWE="));
    CHECK_EQ(aura::crypto::base64Encode("M"), std::string("TQ=="));
    // UTF-8 «привет» → 0L/RgNC40LLQtdGC (проверено python3 -c base64)
    CHECK_EQ(aura::crypto::base64Encode("привет"), std::string("0L/RgNC40LLQtdGC"));
    std::string decoded;
    CHECK(aura::crypto::base64Decode("0L/RgNC40LLQtdGC", decoded));
    CHECK_EQ(decoded, std::string("привет"));
    CHECK_EQ(aura::crypto::base64UrlEncode(std::string("\xfb\xff", 2)).find('='), std::string::npos);
}

TEST(crypto_password_hash) {
    // Новый пароль хэшируется в Argon2id.
    const std::string hash = aura::crypto::hashPassword("aura1234");
    CHECK(hash.rfind("argon2id$v=19$m=", 0) == 0);
    CHECK(aura::crypto::verifyPassword("aura1234", hash));
    CHECK(!aura::crypto::verifyPassword("aura1235", hash));
    CHECK(!aura::crypto::verifyPassword("aura1234", "argon2id$v=19$m=65536,t=3,p=4$bad"));
    CHECK(!aura::crypto::passwordNeedsRehash(hash));
    CHECK(aura::crypto::constantTimeEquals("secret", "secret"));
    CHECK(!aura::crypto::constantTimeEquals("secret", "secrets"));
}

TEST(crypto_argon2id_vector) {
    // Детерминированный Argon2id с фиксированной солью: пароль "password",
    // соль "somesalt" (8 байт), m=64 KiB, t=3, p=2. Ожидаемый 32-байтный тег
    // вычислен эталонной libargon2 и зафиксирован здесь как known-answer.
    const std::string password = "password";
    const std::string saltRaw = "somesalt";
    std::string tag(32, '\0');
    const int rc = argon2id_hash_raw(3, 64, 2, password.data(), password.size(),
                                     saltRaw.data(), saltRaw.size(), tag.data(), tag.size());
    CHECK_EQ(rc, ARGON2_OK);
    const std::string tagHex = aura::crypto::toHex(tag);
    // Known-answer: тег сверен с независимой реализацией (argon2-cffi, Python)
    // для этих же параметров. Совпадение подтверждает корректность libargon2.
    CHECK_EQ(tagHex,
             std::string("c7904b6301d03676acbd4dd657486c42509ccc6d0113fce6495ef4b693392fbf"));
    // Тег детерминирован: повторный вызов даёт тот же результат.
    std::string tag2(32, '\0');
    argon2id_hash_raw(3, 64, 2, password.data(), password.size(),
                      saltRaw.data(), saltRaw.size(), tag2.data(), tag2.size());
    CHECK_EQ(tagHex, aura::crypto::toHex(tag2));
    // Неверный пароль меняет тег.
    std::string tagBad(32, '\0');
    argon2id_hash_raw(3, 64, 2, "Password", 8, saltRaw.data(), saltRaw.size(),
                      tagBad.data(), tagBad.size());
    CHECK(tagHex != aura::crypto::toHex(tagBad));

    // Тот же тег проверяется через argon2idVerify (собираем encoded-строку).
    const std::string encoded = "argon2id$v=19$m=64,t=3,p=2$" +
                                aura::crypto::toHex(saltRaw) + "$" + tagHex;
    CHECK(aura::crypto::argon2idVerify("password", encoded));
    CHECK(!aura::crypto::argon2idVerify("password2", encoded));
}

TEST(crypto_password_migration) {
    // Старый PBKDF2-хэш остаётся проверяемым (прозрачная миграция на входе).
    const std::string salt = "aabbccddeeff0011";
    const std::string legacyHash = "pbkdf2$1000$" + salt + "$" +
        aura::crypto::toHex(aura::crypto::pbkdf2Sha256("aura1234", salt, 1000, 32));
    CHECK(aura::crypto::verifyPassword("aura1234", legacyHash));
    CHECK(!aura::crypto::verifyPassword("aura1235", legacyHash));
    CHECK(aura::crypto::passwordNeedsRehash(legacyHash));
}

namespace {
// Тестовый хелпер: hex → байты (вне класса crypto, только для тестов).
std::string unhex(const std::string& hex) {
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return 0;
    };
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}
}  // namespace

TEST(crypto_hmac_sha1_rfc2202) {
    // RFC 2202, case 1: ключ 20×0x0b, данные "Hi There".
    const std::string key(20, '\x0b');
    CHECK_EQ(aura::crypto::toHex(aura::crypto::hmacSha1(key, "Hi There")),
             std::string("b617318655057264e28bc0b6fb378c8ef146be00"));
    // RFC 2202, case 2.
    CHECK_EQ(aura::crypto::toHex(aura::crypto::hmacSha1("Jefe", "what do ya want for nothing?")),
             std::string("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
}

TEST(crypto_base32_rfc4648) {
    CHECK_EQ(aura::crypto::base32Encode(""), std::string(""));
    CHECK_EQ(aura::crypto::base32Encode("f"), std::string("MY======"));
    CHECK_EQ(aura::crypto::base32Encode("fo"), std::string("MZXQ===="));
    CHECK_EQ(aura::crypto::base32Encode("foo"), std::string("MZXW6==="));
    CHECK_EQ(aura::crypto::base32Encode("foob"), std::string("MZXW6YQ="));
    CHECK_EQ(aura::crypto::base32Encode("fooba"), std::string("MZXW6YTB"));
    CHECK_EQ(aura::crypto::base32Encode("foobar"), std::string("MZXW6YTBOI======"));
}

TEST(crypto_aes256_gcm) {
    const std::string key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    // Known-answer: вектор вычислен независимой библиотекой
    // (Python cryptography/OpenSSL) для nonce=000102…0b, aad="aura-2fa-secret".
    const std::string blob =
        unhex("000102030405060708090a0b0d40854c9cd6864bc809c7c082b9203d8ce94805d58e969523c40d978a79c599");
    const std::string aad = "aura-2fa-secret";
    std::string plain;
    CHECK(aura::crypto::aes256GcmDecrypt(key, blob, aad, plain));
    CHECK_EQ(plain, std::string("JBSWY3DPEHPK3PXP"));

    // Подмены отклоняются: другой AAD, другой ключ, испорченный байт.
    CHECK(!aura::crypto::aes256GcmDecrypt(key, blob, "wrong-aad", plain));
    std::string badKey = key;
    badKey[0] = '\x01';
    CHECK(!aura::crypto::aes256GcmDecrypt(badKey, blob, aad, plain));
    std::string tampered = blob;
    tampered[13] = static_cast<char>(tampered[13] ^ 0x01);
    CHECK(!aura::crypto::aes256GcmDecrypt(key, tampered, aad, plain));

    // Roundtrip: шифрование → расшифровка, включая длинный текст.
    std::string blob2;
    CHECK(aura::crypto::aes256GcmEncrypt(key, "some longer secret value 12345", aad, blob2));
    std::string back;
    CHECK(aura::crypto::aes256GcmDecrypt(key, blob2, aad, back));
    CHECK_EQ(back, std::string("some longer secret value 12345"));

    // Второй known-answer: пустой AAD, короткий текст.
    const std::string blob3 = unhex("00000000000000000000000066d51ec39250ea9203b6e1e6789b7a0f3082");
    CHECK(aura::crypto::aes256GcmDecrypt(key, blob3, "", back));
    CHECK_EQ(back, std::string("hi"));

    // Ключ неверной длины отклоняется.
    CHECK(!aura::crypto::aes256GcmEncrypt("short", "x", "", blob2));
    CHECK(!aura::crypto::aes256GcmDecrypt("short", blob, aad, plain));
}

TEST(totp_rfc6238_vectors) {
    // RFC 6238, приложение B (SHA-1, секрет — ASCII "12345678901234567890").
    // В таблице RFC коды 8-значные; Aura использует 6 цифр — сверяем последние 6.
    const std::string secret = "12345678901234567890";
    CHECK_EQ(aura::totp::codeAt(secret, 59), std::string("287082"));
    CHECK_EQ(aura::totp::codeAt(secret, 1111111109), std::string("081804"));
    CHECK_EQ(aura::totp::codeAt(secret, 1111111111), std::string("050471"));
    CHECK_EQ(aura::totp::codeAt(secret, 1234567890), std::string("005924"));
    CHECK_EQ(aura::totp::codeAt(secret, 2000000000), std::string("279037"));  // RFC: 69279037
    CHECK_EQ(aura::totp::codeAt(secret, 20000000000), std::string("353130"));
    // 8-значный режим даёт ровно значения из таблицы RFC.
    CHECK_EQ(aura::totp::codeAt(secret, 59, 8), std::string("94287082"));
    CHECK_EQ(aura::totp::codeAt(secret, 2000000000, 8), std::string("69279037"));
    CHECK_EQ(aura::totp::codeAt(secret, 20000000000, 8), std::string("65353130"));
}

TEST(totp_verify_window_and_reuse) {
    const std::string secretB32 = aura::totp::generateSecretBase32();
    std::string raw;
    CHECK(aura::totp::decodeSecretBase32(secretB32, raw));
    CHECK_EQ(raw.size(), std::size_t(20));

    const std::int64_t now = 900;  // счётчик 30
    const std::string currentCode = aura::totp::codeAt(raw, now);
    std::int64_t matched = -1;
    CHECK(aura::totp::verifyAt(raw, currentCode, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(30));

    // Одноразовость: тот же код с lastUsedCounter=30 отклоняется.
    CHECK(!aura::totp::verifyAt(raw, currentCode, now, 30, matched));

    // Окно ±1 период: коды соседних счётчиков принимаются.
    const std::string prev = aura::totp::codeAt(raw, 29 * 30);
    CHECK(aura::totp::verifyAt(raw, prev, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(29));
    const std::string next = aura::totp::codeAt(raw, 31 * 30);
    CHECK(aura::totp::verifyAt(raw, next, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(31));

    // Два периода назад — уже вне окна.
    const std::string old = aura::totp::codeAt(raw, 28 * 30);
    CHECK(!aura::totp::verifyAt(raw, old, now, 0, matched));

    // Неверный код, нецифровой ввод, короткая строка.
    std::string wrong = currentCode;
    wrong[0] = (wrong[0] == '9') ? '0' : static_cast<char>(wrong[0] + 1);
    CHECK(!aura::totp::verifyAt(raw, wrong, now, 0, matched));
    CHECK(!aura::totp::verifyAt(raw, "abcdef", now, 0, matched));
    CHECK(!aura::totp::verifyAt(raw, "12345", now, 0, matched));
}

TEST(totp_secret_decode_and_uri) {
    // Канонический тестовый секрет: "JBSWY3DPEHPK3PXP" = "Hello!" + deadbeef.
    std::string raw;
    CHECK(aura::totp::decodeSecretBase32("JBSWY3DPEHPK3PXP", raw));
    CHECK_EQ(aura::crypto::toHex(raw), std::string("48656c6c6f21deadbeef"));
    // Строчные буквы, пробелы, дефисы — допустимы.
    CHECK(aura::totp::decodeSecretBase32("jbsw y3dp-ehpk3pxp", raw));
    CHECK_EQ(aura::crypto::toHex(raw), std::string("48656c6c6f21deadbeef"));
    // Недопустимый символ и слишком короткий секрет.
    CHECK(!aura::totp::decodeSecretBase32("JBSWY3DPEHPK3PX!", raw));
    CHECK(!aura::totp::decodeSecretBase32("MY======", raw));

    const std::string uri = aura::totp::otpauthUri("JBSWY3DPEHPK3PXP", "anna@aura.app", "Aura");
    CHECK(uri.rfind("otpauth://totp/Aura:anna%40aura.app?", 0) == 0);
    CHECK(uri.find("secret=JBSWY3DPEHPK3PXP") != std::string::npos);
    CHECK(uri.find("issuer=Aura") != std::string::npos);
    CHECK(uri.find("algorithm=SHA1") != std::string::npos);
    CHECK(uri.find("digits=6") != std::string::npos);
    CHECK(uri.find("period=30") != std::string::npos);
}

// --------------------------------------------------------------------- JWT
TEST(jwt_sign_and_verify) {
    Json claims = Json::object();
    claims.set("sub", Json(42));
    claims.set("email", Json("anna@example.com"));
    const std::string token = aura::jwt::sign(claims, "test-secret", 60);
    CHECK_EQ(std::count(token.begin(), token.end(), '.'), 2);

    const aura::jwt::Token verified = aura::jwt::verify(token, "test-secret");
    CHECK(verified.valid);
    CHECK_EQ(verified.claims.getInt("sub"), 42);
    CHECK_EQ(verified.claims.getString("email"), std::string("anna@example.com"));

    CHECK(!aura::jwt::verify(token, "other-secret").valid);
    const aura::jwt::Token expired = aura::jwt::verify(token, "test-secret", aura::jwt::nowEpoch() + 3600);
    CHECK(!expired.valid);
    CHECK(!aura::jwt::verify("not.a.token", "test-secret").valid);
}

// --------------------------------------------------------------- WebSocket
TEST(ws_accept_token_rfc6455) {
    // Пример из RFC 6455, раздел 1.3
    CHECK_EQ(aura::ws::acceptToken("dGhlIHNhbXBsZSBub25jZQ=="),
             std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(ws_handshake_parse) {
    const std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost:9000\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer abc.def.ghi\r\n"
        "Origin: http://localhost\r\n\r\n";
    const aura::ws::Handshake handshake = aura::ws::parseHandshake(request);
    CHECK(handshake.valid);
    CHECK_EQ(handshake.path, std::string("/ws"));
    CHECK_EQ(handshake.key, std::string("dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK_EQ(handshake.authorization, std::string("Bearer abc.def.ghi"));
    CHECK_EQ(handshake.origin, std::string("http://localhost"));

    CHECK(!aura::ws::parseHandshake("POST / HTTP/1.1\r\n\r\n").valid);
    CHECK(!aura::ws::parseHandshake("GET / HTTP/1.1\r\nHost: x\r\n\r\n").valid);
    CHECK(aura::ws::handshakeResponse("dGhlIHNhbXBsZSBub25jZQ==").find("101 Switching Protocols") !=
          std::string::npos);
}

TEST(ws_frame_roundtrip) {
    const std::string payload = R"({"type":"ping","payload":{"n":1}})";
    const std::string frame = aura::ws::encodeFrame(aura::ws::Opcode::Text, payload);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(frame[0])), 0x81);

    // Имитируем клиентский кадр с маской
    std::string masked = frame;
    masked[1] = static_cast<char>(static_cast<unsigned char>(masked[1]) | 0x80);
    const std::size_t headerLength = 2;
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    masked.insert(masked.begin() + static_cast<long>(headerLength), mask, mask + 4);
    for (std::size_t i = headerLength + 4; i < masked.size(); ++i) {
        masked[i] = static_cast<char>(static_cast<unsigned char>(masked[i]) ^ mask[(i - headerLength - 4) % 4]);
    }

    std::size_t consumed = 0;
    aura::ws::Frame decoded;
    std::string error;
    const auto result = aura::ws::decodeFrame(masked, 1024 * 1024, consumed, decoded, error);
    CHECK(result == aura::ws::DecodeResult::Complete);
    CHECK_EQ(consumed, masked.size());
    CHECK_EQ(decoded.payload, payload);
    CHECK(decoded.opcode == aura::ws::Opcode::Text);

    // Неполный кадр
    std::size_t consumedPartial = 0;
    aura::ws::Frame partial;
    CHECK(aura::ws::decodeFrame(masked.substr(0, 3), 1024, consumedPartial, partial, error) ==
          aura::ws::DecodeResult::Incomplete);

    // Превышение лимита
    std::size_t consumedBig = 0;
    aura::ws::Frame big;
    CHECK(aura::ws::decodeFrame(masked, 10, consumedBig, big, error) ==
          aura::ws::DecodeResult::ProtocolError);
}

// ---------------------------------------------------------------- protocol
TEST(protocol_envelope) {
    const Json okMessage = aura::protocol::ok("req-1", [] {
        Json payload = Json::object();
        payload.set("value", Json(1));
        return payload;
    }());
    CHECK_EQ(okMessage.getString("type"), std::string("ok"));
    CHECK_EQ(okMessage.getString("id"), std::string("req-1"));
    CHECK_EQ(okMessage.get("payload").getInt("value"), 1);

    const Json errorMessage = aura::protocol::error("req-2", aura::protocol::code::kUnauthorized, "нужен вход");
    CHECK_EQ(errorMessage.getString("code"), std::string("unauthorized"));

    const Json eventMessage = aura::protocol::event("chat.message");
    CHECK_EQ(eventMessage.getString("event"), std::string("chat.message"));

    aura::protocol::Request request;
    std::string error;
    CHECK(!aura::protocol::parse(Json::array(), request, error));
    CHECK(aura::protocol::isPublicType("auth.login"));
    CHECK(!aura::protocol::isPublicType("chat.send"));
}

// --------------------------------------------------------------------- net
TEST(net_url_parsing) {
    aura::net::Url url;
    std::string error;
    CHECK(aura::net::parseUrl("http://127.0.0.1:8000/v1/agent/run", url, error));
    CHECK_EQ(url.host, std::string("127.0.0.1"));
    CHECK_EQ(url.port, 8000);
    CHECK_EQ(url.path, std::string("/v1/agent/run"));

    CHECK(aura::net::parseUrl("http://ai-service/healthz", url, error));
    CHECK_EQ(url.port, 80);
    CHECK_EQ(url.path, std::string("/healthz"));

    CHECK(!aura::net::parseUrl("https://example.com", url, error));
    CHECK(!error.empty());

    // Разбор chunked-ответа
    const std::string chunked = "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n";
    aura::net::HttpResponse response;
    CHECK(aura::net::parseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n", response));
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.header("transfer-encoding"), std::string("chunked"));
    CHECK_EQ(aura::net::decodeChunked(chunked), std::string("Hello World"));
}

// -------------------------------------------------------------------- auth
TEST(auth_validation_rules) {
    CHECK_EQ(aura::AuthManager::normalizeEmail("  Anna@Example.COM "), std::string("anna@example.com"));
    CHECK(!aura::AuthManager::passwordError("short1").empty());
    CHECK(!aura::AuthManager::passwordError("onlyletters").empty());
    CHECK(!aura::AuthManager::passwordError("12345678").empty());
    CHECK(aura::AuthManager::passwordError("aura1234").empty());
}

// ------------------------------------------------------------- интеграция
namespace integration {

struct Fixture {
    aura::Config config;
    std::string dbPath;
    std::unique_ptr<aura::Server> server;
    int observerFd = -1;

    Fixture() {
        dbPath = "/tmp/aura-test-" + std::to_string(::getpid()) + ".json";
        std::remove(dbPath.c_str());
        config = aura::Config::fromEnv();
        config.databaseUrl.clear();  // встроенное хранилище
        config.embeddedDbPath = dbPath;
        config.port = 0;             // свободный порт
        config.jwtSecret = "test-secret";
        config.logLevel = "warn";
        config.aiServiceUrl = "http://127.0.0.1:1";  // гарантированно недоступен
        config.aiTimeoutMs = 500;
        config.schedulerIntervalMs = 0;  // фоновый поток не нужен: tick зовём вручную
        server = std::make_unique<aura::Server>(config);
        std::string error;
        if (!server->start(error)) {
            std::cout << "  не удалось поднять сервер: " << error << "\n";
            ++g_failures;
        }
    }

    ~Fixture() {
        server->stop();
        server.reset();
        if (observerFd >= 0) ::close(observerFd);
        std::remove(dbPath.c_str());
    }

    // Сессия поверх socketpair: второй конец отдаём наблюдателю.
    std::shared_ptr<aura::Session> makeSession(const std::string& id = "test") {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            std::cout << "  socketpair failed\n";
            ++g_failures;
            return nullptr;
        }
        if (observerFd >= 0) ::close(observerFd);
        observerFd = fds[0];
        return std::make_shared<aura::Session>(fds[1], id, "127.0.0.1:0", config);
    }

    Json call(const std::shared_ptr<aura::Session>& session,
              const std::string& type,
              const Json& payload = Json::object()) {
        Json message = Json::object();
        message.set("id", Json("req-" + type));
        message.set("type", Json(type));
        message.set("payload", payload);
        return server->handleMessage(session, message);
    }

    Json registerUser(const std::shared_ptr<aura::Session>& session,
                      const std::string& email,
                      const std::string& name) {
        Json payload = Json::object();
        payload.set("email", Json(email));
        payload.set("password", Json("aura1234"));
        payload.set("display_name", Json(name));
        const Json registered = call(session, "auth.register", payload);
        if (registered.getString("type") != "ok") return registered;

        // Подтверждение email: код возвращён в ответе (драйвер почты dev).
        const std::string code = registered.get("payload").getString("email_code");
        Json verify = Json::object();
        verify.set("email", Json(email));
        verify.set("code", Json(code));
        const Json verified = call(session, "auth.verifyEmail", verify);
        if (verified.getString("type") != "ok") return verified;

        // Вход: выдаёт access+refresh и аутентифицирует соединение.
        Json login = Json::object();
        login.set("email", Json(email));
        login.set("password", Json("aura1234"));
        return call(session, "auth.login", login);
    }
};

Json payloadOf(const Json& response) { return response.get("payload"); }

}  // namespace integration

using integration::Fixture;

TEST(server_handler_registry) {
    Fixture fixture;
    CHECK(fixture.server->hasHandler("auth.login"));
    CHECK(fixture.server->hasHandler("chat.send"));
    CHECK(fixture.server->hasHandler("agent.ask"));
    CHECK(fixture.server->hasHandler("memory.list"));
    CHECK(fixture.server->hasHandler("prefs.set"));
    CHECK(fixture.server->hasHandler("tool.run"));
    CHECK(!fixture.server->hasHandler("nope.nope"));
    CHECK(fixture.server->handlerTypes().size() >= 20);
    CHECK_EQ(fixture.server->database().backend().rfind("embedded", 0), static_cast<std::size_t>(0));
}

TEST(server_unknown_type_and_auth_guard) {
    Fixture fixture;
    auto session = fixture.makeSession();
    const Json unknown = fixture.call(session, "does.not.exist");
    CHECK_EQ(unknown.getString("type"), std::string("error"));
    CHECK_EQ(unknown.getString("code"), std::string("bad_request"));

    const Json guarded = fixture.call(session, "chat.list");
    CHECK_EQ(guarded.getString("code"), std::string("unauthorized"));

    // ping — публичный
    CHECK_EQ(fixture.call(session, "ping").getString("type"), std::string("ok"));
}

TEST(server_register_login_flow) {
    Fixture fixture;
    auto session = fixture.makeSession();

    // 1) Регистрация: сессия НЕ выдаётся, ждём подтверждения email.
    Json regPayload = Json::object();
    regPayload.set("email", Json("anna@example.com"));
    regPayload.set("password", Json("aura1234"));
    regPayload.set("display_name", Json("Анна"));
    const Json registered = fixture.call(session, "auth.register", regPayload);
    CHECK_EQ(registered.getString("type"), std::string("ok"));
    CHECK(integration::payloadOf(registered).getBool("requires_verification"));
    CHECK(integration::payloadOf(registered).getString("token").empty());
    CHECK(!session->authenticated());
    const std::string emailCode = integration::payloadOf(registered).getString("email_code");
    CHECK_EQ(emailCode.size(), static_cast<std::size_t>(6));  // шестизначный код (dev-драйвер)

    // 2) Вход до подтверждения — отдельный код ошибки.
    auto earlySession = fixture.makeSession("early");
    Json login = Json::object();
    login.set("email", Json("anna@example.com"));
    login.set("password", Json("aura1234"));
    CHECK_EQ(fixture.call(earlySession, "auth.login", login).getString("code"),
             std::string("email_not_verified"));

    // 3) Неверный код отклоняется, верный — подтверждает (идемпотентно повторно).
    Json badVerify = Json::object();
    badVerify.set("email", Json("anna@example.com"));
    badVerify.set("code", Json("000000"));
    CHECK_EQ(fixture.call(session, "auth.verifyEmail", badVerify).getString("code"),
             std::string("bad_request"));
    Json verify = Json::object();
    verify.set("email", Json("anna@example.com"));
    verify.set("code", Json(emailCode));
    CHECK(fixture.call(session, "auth.verifyEmail", verify).get("payload").getBool("verified"));
    CHECK(fixture.call(session, "auth.verifyEmail", verify).get("payload").getBool("verified"));

    // 4) Дубль email отклоняется.
    auto other = fixture.makeSession("other");
    const Json duplicate = fixture.registerUser(other, "ANNA@example.com", "Анна 2");
    CHECK_EQ(duplicate.getString("code"), std::string("conflict"));

    // 5) Слабый пароль отклоняется.
    auto third = fixture.makeSession("third");
    Json weak = Json::object();
    weak.set("email", Json("bob@example.com"));
    weak.set("password", Json("123"));
    weak.set("display_name", Json("Боб"));
    CHECK_EQ(fixture.call(third, "auth.register", weak).getString("code"), std::string("bad_request"));

    // 6) Вход с верным и неверным паролем.
    auto loginSession = fixture.makeSession("login");
    const Json loggedIn = fixture.call(loginSession, "auth.login", login);
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));
    const std::string token = integration::payloadOf(loggedIn).getString("token");
    const std::string refreshToken = integration::payloadOf(loggedIn).getString("refresh_token");
    CHECK(!token.empty());
    CHECK(!refreshToken.empty());
    CHECK(loginSession->authenticated());

    auto badSession = fixture.makeSession("bad");
    login.set("password", Json("wrongpass1"));
    CHECK_EQ(fixture.call(badSession, "auth.login", login).getString("code"), std::string("unauthorized"));

    // 7) auth.me работает в аутентифицированной сессии.
    const Json me = fixture.call(loginSession, "auth.me");
    CHECK_EQ(integration::payloadOf(me).getString("display_name"), std::string("Анна"));

    // 8) Живой токен можно предъявить повторно (auth.token).
    auto tokenSession = fixture.makeSession("token");
    Json authPayload = Json::object();
    authPayload.set("token", Json(token));
    CHECK_EQ(fixture.call(tokenSession, "auth.token", authPayload).getString("type"), std::string("ok"));
    CHECK_EQ(tokenSession->userId(), loginSession->userId());

    // 9) После выхода тот же токен отозван.
    CHECK(fixture.call(loginSession, "auth.logout").get("payload").getBool("revoked"));
    CHECK(!loginSession->authenticated());
    auto staleSession = fixture.makeSession("stale");
    CHECK_EQ(fixture.call(staleSession, "auth.token", authPayload).getString("code"),
             std::string("unauthorized"));
}

TEST(server_refresh_rotation_and_reuse) {
    Fixture fixture;
    auto session = fixture.makeSession();
    const Json loggedIn = fixture.registerUser(session, "rita@example.com", "Рита");
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));
    const std::string refresh1 = integration::payloadOf(loggedIn).getString("refresh_token");
    CHECK(!refresh1.empty());

    // Ротация: старый токен заменяется новым в том же семействе.
    Json refreshReq = Json::object();
    refreshReq.set("refresh_token", Json(refresh1));
    const Json rotated = fixture.call(session, "auth.refresh", refreshReq);
    CHECK_EQ(rotated.getString("type"), std::string("ok"));
    const std::string refresh2 = integration::payloadOf(rotated).getString("refresh_token");
    const std::string access2 = integration::payloadOf(rotated).getString("token");
    CHECK(!refresh2.empty());
    CHECK(refresh1 != refresh2);
    CHECK(!access2.empty());

    // Новый refresh-токен работает.
    refreshReq.set("refresh_token", Json(refresh2));
    const Json rotated2 = fixture.call(session, "auth.refresh", refreshReq);
    CHECK_EQ(rotated2.getString("type"), std::string("ok"));
    const std::string refresh3 = integration::payloadOf(rotated2).getString("refresh_token");

    // Повторное использование ОТОЗВАННОГО refresh1 = кража:
    // всё семейство и все сессии отзываются.
    refreshReq.set("refresh_token", Json(refresh1));
    CHECK_EQ(fixture.call(session, "auth.refresh", refreshReq).getString("code"),
             std::string("unauthorized"));
    refreshReq.set("refresh_token", Json(refresh3));
    CHECK_EQ(fixture.call(session, "auth.refresh", refreshReq).getString("code"),
             std::string("unauthorized"));

    // Access-токен, выданный до детекта кражи, тоже отозван (сессии обнулены).
    auto accessSession = fixture.makeSession("acc");
    Json authPayload = Json::object();
    authPayload.set("token", Json(access2));
    CHECK_EQ(fixture.call(accessSession, "auth.token", authPayload).getString("code"),
             std::string("unauthorized"));

    // Неизвестный токен — просто unauthorized.
    refreshReq.set("refresh_token", Json("нет-такого"));
    CHECK_EQ(fixture.call(session, "auth.refresh", refreshReq).getString("code"),
             std::string("unauthorized"));
}

TEST(server_password_reset_flow) {
    Fixture fixture;
    auto session = fixture.makeSession();
    const Json loggedIn = fixture.registerUser(session, "oleg@example.com", "Олег");
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));
    const std::string accessToken = integration::payloadOf(loggedIn).getString("token");

    // Запрос кода: ответ не раскрывает существование email, код — в dev-режиме.
    Json forgot = Json::object();
    forgot.set("email", Json("oleg@example.com"));
    const Json forgotResp = fixture.call(session, "auth.forgotPassword", forgot);
    CHECK_EQ(forgotResp.getString("type"), std::string("ok"));
    const std::string resetCode = integration::payloadOf(forgotResp).getString("reset_code");
    CHECK(!resetCode.empty());
    // Для несуществующего email ответ тоже «ок», но без кода.
    Json unknown = Json::object();
    unknown.set("email", Json("nobody@example.com"));
    CHECK_EQ(fixture.call(session, "auth.forgotPassword", unknown).getString("type"),
             std::string("ok"));

    // Неверный код отклоняется.
    Json badReset = Json::object();
    badReset.set("email", Json("oleg@example.com"));
    badReset.set("code", Json("ffffffffffffffffffffffffffffffff"));
    badReset.set("new_password", Json("newpass123"));
    CHECK_EQ(fixture.call(session, "auth.resetPassword", badReset).getString("code"),
             std::string("bad_request"));

    // Верный код меняет пароль и обрывает все сессии.
    badReset.set("code", Json(resetCode));
    CHECK(fixture.call(session, "auth.resetPassword", badReset).get("payload").getBool("reset"));
    auto stale = fixture.makeSession("stale");
    Json authPayload = Json::object();
    authPayload.set("token", Json(accessToken));
    CHECK_EQ(fixture.call(stale, "auth.token", authPayload).getString("code"),
             std::string("unauthorized"));

    // Вход со старым паролем не проходит, с новым — проходит.
    Json login = Json::object();
    login.set("email", Json("oleg@example.com"));
    login.set("password", Json("aura1234"));
    auto oldSession = fixture.makeSession("old");
    CHECK_EQ(fixture.call(oldSession, "auth.login", login).getString("code"),
             std::string("unauthorized"));
    login.set("password", Json("newpass123"));
    CHECK_EQ(fixture.call(oldSession, "auth.login", login).getString("type"), std::string("ok"));
}

TEST(server_sessions_management) {
    Fixture fixture;
    auto first = fixture.makeSession("first");
    const Json firstLogin = fixture.registerUser(first, "mia@example.com", "Миа");
    CHECK_EQ(firstLogin.getString("type"), std::string("ok"));
    auto second = fixture.makeSession("second");
    Json login = Json::object();
    login.set("email", Json("mia@example.com"));
    login.set("password", Json("aura1234"));
    const Json secondLogin = fixture.call(second, "auth.login", login);
    CHECK_EQ(secondLogin.getString("type"), std::string("ok"));

    // Две живые сессии, текущая помечена.
    const Json list = fixture.call(second, "sessions.list");
    const Json sessions = integration::payloadOf(list).get("sessions");
    CHECK_EQ(sessions.items().size(), static_cast<std::size_t>(2));
    bool currentMarked = false;
    std::string otherId;
    for (const auto& item : sessions.items()) {
        if (item.getBool("current")) currentMarked = true;
        else otherId = item.getString("id");
    }
    CHECK(currentMarked);
    CHECK(!otherId.empty());

    // Отзыв чужой (первой) сессии: её токен больше не проходит.
    Json revoke = Json::object();
    revoke.set("session_id", Json(otherId));
    CHECK(fixture.call(second, "sessions.revoke", revoke).get("payload").getBool("revoked"));
    auto probe = fixture.makeSession("probe");
    Json authPayload = Json::object();
    authPayload.set("token", Json(integration::payloadOf(firstLogin).getString("token")));
    CHECK_EQ(fixture.call(probe, "auth.token", authPayload).getString("code"),
             std::string("unauthorized"));

    // revokeAll держит текущую сессию живой.
    CHECK(fixture.call(second, "sessions.revokeAll").get("payload").getBool("revoked"));
    const Json after = fixture.call(second, "sessions.list");
    CHECK_EQ(integration::payloadOf(after).get("sessions").items().size(), static_cast<std::size_t>(1));
    CHECK(second->authenticated());
}

TEST(server_password_migration_to_argon2id) {
    Fixture fixture;
    // Создаём пользователя со старым PBKDF2-хэшем напрямую в БД.
    const std::string salt = "0123456789abcdef";
    const std::string legacy = "pbkdf2$1000$" + salt + "$" +
        aura::crypto::toHex(aura::crypto::pbkdf2Sha256("aura1234", salt, 1000, 32));
    long long userId = 0;
    CHECK(fixture.server->database().db()
              .createUser("legacy@example.com", legacy, "Леги", userId)
              .ok);
    CHECK(fixture.server->database().db().setEmailVerified(userId, true).ok);

    auto session = fixture.makeSession();
    Json login = Json::object();
    login.set("email", Json("legacy@example.com"));
    login.set("password", Json("aura1234"));
    CHECK_EQ(fixture.call(session, "auth.login", login).getString("type"), std::string("ok"));

    // После входа хэш пересчитан в Argon2id.
    const auto user = fixture.server->database().db().findUserByEmail("legacy@example.com");
    CHECK(user.has_value());
    CHECK(user->passwordHash.rfind("argon2id$", 0) == 0);
    CHECK(aura::crypto::verifyPassword("aura1234", user->passwordHash));
}

TEST(server_change_password) {
    Fixture fixture;
    auto session = fixture.makeSession();
    const Json loggedIn = fixture.registerUser(session, "kai@example.com", "Кай");
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));

    // Неверный текущий пароль отклоняется.
    Json change = Json::object();
    change.set("old_password", Json("wrongold1"));
    change.set("new_password", Json("brandnew1"));
    CHECK_EQ(fixture.call(session, "auth.changePassword", change).getString("code"),
             std::string("forbidden"));

    // Верная смена: доступ сохраняется, старый пароль больше не работает.
    change.set("old_password", Json("aura1234"));
    CHECK(fixture.call(session, "auth.changePassword", change).get("payload").getBool("changed"));
    auto fresh = fixture.makeSession("fresh");
    Json login = Json::object();
    login.set("email", Json("kai@example.com"));
    login.set("password", Json("aura1234"));
    CHECK_EQ(fixture.call(fresh, "auth.login", login).getString("code"), std::string("unauthorized"));
    login.set("password", Json("brandnew1"));
    CHECK_EQ(fixture.call(fresh, "auth.login", login).getString("type"), std::string("ok"));
}

TEST(server_chat_flow_and_delivery) {
    Fixture fixture;
    auto anna = fixture.makeSession("anna");
    auto anya = fixture.makeSession("anya");
    fixture.registerUser(anna, "anna@example.com", "Анна");
    fixture.registerUser(anya, "anya@example.com", "Аня");

    // Чат с самим собой запрещён
    Json self = Json::object();
    self.set("contact", Json("anna@example.com"));
    CHECK_EQ(fixture.call(anna, "chat.open", self).getString("code"), std::string("bad_request"));

    // Открытие чата
    Json open = Json::object();
    open.set("contact", Json("anya@example.com"));
    const Json opened = fixture.call(anna, "chat.open", open);
    CHECK_EQ(opened.getString("type"), std::string("ok"));
    const long long chatId = integration::payloadOf(opened).get("chat").getInt("id");
    CHECK(chatId > 0);

    // Повторное открытие возвращает тот же чат
    const Json reopened = fixture.call(anna, "chat.open", open);
    CHECK_EQ(integration::payloadOf(reopened).get("chat").getInt("id"), chatId);
    CHECK(!integration::payloadOf(reopened).getBool("created"));

    // Список чатов
    const Json list = fixture.call(anna, "chat.list");
    CHECK_EQ(integration::payloadOf(list).get("chats").size(), static_cast<std::size_t>(1));

    // Отправка сообщения
    Json send = Json::object();
    send.set("chat_id", Json(chatId));
    send.set("body", Json("Привет! Обсудим стартап?"));
    const Json sent = fixture.call(anna, "chat.send", send);
    CHECK_EQ(sent.getString("type"), std::string("ok"));
    CHECK(integration::payloadOf(sent).getInt("id") > 0);
    CHECK_EQ(integration::payloadOf(sent).getString("sender_name"), std::string("Анна"));

    // История
    Json history = Json::object();
    history.set("chat_id", Json(chatId));
    const Json historyResponse = fixture.call(anya, "chat.history", history);
    const Json messages = integration::payloadOf(historyResponse).get("messages");
    CHECK_EQ(messages.size(), static_cast<std::size_t>(1));
    CHECK_EQ(messages.at(0).getString("body"), std::string("Привет! Обсудим стартап?"));

    // Чужой чат недоступен
    auto bob = fixture.makeSession("bob");
    fixture.registerUser(bob, "bob@example.com", "Боб");
    CHECK_EQ(fixture.call(bob, "chat.history", history).getString("code"), std::string("forbidden"));

    // Посторонний не может писать в чат
    Json intruder = Json::object();
    intruder.set("chat_id", Json(chatId));
    intruder.set("body", Json("я тут"));
    CHECK_EQ(fixture.call(bob, "chat.send", intruder).getString("code"), std::string("forbidden"));

    // Поиск пользователей и участники
    Json search = Json::object();
    search.set("query", Json("anya"));
    CHECK_EQ(integration::payloadOf(fixture.call(anna, "users.search", search))
                 .get("users")
                 .size(),
             static_cast<std::size_t>(1));
    Json members = Json::object();
    members.set("chat_id", Json(chatId));
    CHECK_EQ(integration::payloadOf(fixture.call(anna, "chat.members", members))
                 .get("members")
                 .size(),
             static_cast<std::size_t>(2));
}

TEST(server_session_delivers_frames_over_socket) {
    Fixture fixture;
    auto session = fixture.makeSession("socket");
    fixture.registerUser(session, "socket@example.com", "Сокет");

    Json payload = Json::object();
    payload.set("hello", Json("мир"));
    session->send(aura::protocol::event("test.event", payload));

    // Читаем кадр из второго конца socketpair
    char buffer[1024];
    const ssize_t received = ::recv(fixture.observerFd, buffer, sizeof(buffer), 0);
    CHECK(received > 0);
    std::string raw(buffer, static_cast<std::size_t>(received));
    std::size_t consumed = 0;
    aura::ws::Frame frame;
    std::string error;
    // Кадр пришёл от сервера, поэтому маска в нём запрещена (requireMask=false).
    CHECK(aura::ws::decodeFrame(raw, sizeof(buffer), consumed, frame, error, false) ==
          aura::ws::DecodeResult::Complete);
    const Json decoded = Json::parse(frame.payload);
    CHECK_EQ(decoded.getString("event"), std::string("test.event"));
    CHECK_EQ(decoded.get("payload").getString("hello"), std::string("мир"));
}

TEST(server_memory_and_preferences) {
    Fixture fixture;
    auto session = fixture.makeSession("memory");
    fixture.registerUser(session, "anna@example.com", "Анна");

    Json add = Json::object();
    add.set("text", Json("Люблю тихие кофейни"));
    add.set("kind", Json("preference"));
    CHECK_EQ(fixture.call(session, "memory.add", add).get("payload").getInt("saved"), 1);

    Json query = Json::object();
    query.set("query", Json("кофейни"));
    const Json listed = fixture.call(session, "memory.list", query);
    const Json entries = integration::payloadOf(listed).get("entries");
    CHECK(entries.size() >= 1);
    CHECK_EQ(entries.at(0).getString("text"), std::string("Люблю тихие кофейни"));

    // Настройки: чтение по умолчанию и запись
    const Json defaults = fixture.call(session, "prefs.get");
    CHECK_EQ(integration::payloadOf(defaults).getString("theme"), std::string("graphite"));

    Json update = Json::object();
    Json diet = Json::array();
    diet.push(Json("vegan"));
    update.set("diet", diet);
    update.set("city", Json("Керкраде"));
    update.set("lat", Json(50.861));
    update.set("lon", Json(6.064));
    update.set("budget_limit", Json(3));
    const Json updated = fixture.call(session, "prefs.set", update);
    CHECK_EQ(updated.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(updated).getString("city"), std::string("Керкраде"));
    CHECK_EQ(integration::payloadOf(updated).get("diet").size(), static_cast<std::size_t>(1));

    // Контекст для AI-сервиса содержит память и настройки
    const Json context = fixture.server->memory().context(session->userId(), "найдём кофейню");
    CHECK_EQ(context.getString("email"), std::string("anna@example.com"));
    CHECK(context.get("memory").size() >= 1);
    CHECK_EQ(context.get("preferences").getString("city"), std::string("Керкраде"));
}

TEST(server_tools_execution) {
    Fixture fixture;
    auto session = fixture.makeSession("tools");
    fixture.registerUser(session, "anna@example.com", "Анна");

    CHECK(integration::payloadOf(fixture.call(session, "tool.list")).get("tools").size() >= 6);

    // create_note
    Json note = Json::object();
    note.set("tool", Json("create_note"));
    Json noteArgs = Json::object();
    noteArgs.set("text", Json("Идея: сеть ИИ-агентов"));
    note.set("args", noteArgs);
    const Json noteResult = fixture.call(session, "tool.run", note);
    CHECK_EQ(noteResult.getString("type"), std::string("ok"));
    CHECK(!integration::payloadOf(noteResult).getString("note_id").empty());

    // find_cafe уважает диету
    Json cafe = Json::object();
    cafe.set("tool", Json("find_cafe"));
    Json cafeArgs = Json::object();
    Json diet = Json::array();
    diet.push(Json("vegan"));
    cafeArgs.set("diet", diet);
    cafeArgs.set("city", Json("Керкраде"));
    cafe.set("args", cafeArgs);
    const Json cafes = fixture.call(session, "tool.run", cafe);
    const Json results = integration::payloadOf(cafes).get("results");
    CHECK(results.size() >= 1);
    bool meatFree = true;
    for (const auto& item : results.items()) {
        const Json tagArray = item.get("tags");
        for (const auto& tag : tagArray.items()) {
            if (tag.asString() == "meat") meatFree = false;
        }
    }
    CHECK(meatFree);

    // book_table
    Json book = Json::object();
    book.set("tool", Json("book_table"));
    Json bookArgs = Json::object();
    bookArgs.set("place", Json("Кофе на полпути"));
    bookArgs.set("at", Json("2026-09-19T12:00:00Z"));
    bookArgs.set("people", Json(2));
    book.set("args", bookArgs);
    const Json booked = fixture.call(session, "tool.run", book);
    CHECK(integration::payloadOf(booked).getString("confirmation").rfind("AURA-", 0) == 0);

    // Неизвестный инструмент
    Json unknown = Json::object();
    unknown.set("tool", Json("launch_rockets"));
    unknown.set("args", Json::object());
    CHECK_EQ(fixture.call(session, "tool.run", unknown).getString("type"), std::string("error"));
}

TEST(server_agent_reports_upstream_failure) {
    Fixture fixture;
    auto session = fixture.makeSession("agent");
    fixture.registerUser(session, "anna@example.com", "Анна");

    Json ask = Json::object();
    ask.set("message", Json("Хочу встретиться с Аней в эти выходные обсудить стартап"));
    const Json response = fixture.call(session, "agent.ask", ask);
    CHECK_EQ(response.getString("type"), std::string("error"));
    CHECK_EQ(response.getString("code"), std::string("upstream_error"));

    // Статус агента показывает, что сервис недоступен
    const Json status = fixture.call(session, "agent.status");
    CHECK(!integration::payloadOf(status).getBool("available"));

    // A2A без собеседника — not_found
    Json negotiate = Json::object();
    negotiate.set("target", Json("ghost@example.com"));
    CHECK_EQ(fixture.call(session, "agent.negotiate", negotiate).getString("code"),
             std::string("not_found"));
}

TEST(server_speech_transcribe) {
    Fixture fixture;
    auto session = fixture.makeSession("speech");
    fixture.registerUser(session, "sonya@example.com", "Соня");

    // Пустое аудио — bad_request (до обращения к AI-сервису).
    const Json empty = fixture.call(session, "speech.transcribe", Json::object());
    CHECK_EQ(empty.getString("type"), std::string("error"));
    CHECK_EQ(empty.getString("code"), std::string("bad_request"));

    // С аудио, но AI-сервис недоступен — upstream_error.
    Json req = Json::object();
    req.set("audio", Json("AQIDBA=="));  // base64-заглушка
    req.set("language", Json("ru"));
    const Json response = fixture.call(session, "speech.transcribe", req);
    CHECK_EQ(response.getString("type"), std::string("error"));
    CHECK_EQ(response.getString("code"), std::string("upstream_error"));
}

TEST(server_health_snapshot) {
    Fixture fixture;
    const Json health = fixture.server->health();
    CHECK_EQ(health.getString("status"), std::string("ok"));
    CHECK(health.get("database_healthy").asBool());
    CHECK(health.get("connections").isObject());
    CHECK(health.getInt("port") >= 0);
}

TEST(server_two_factor_full_flow) {
    Fixture fixture;

    // 1) Регистрация + вход (сессию выдаёт обычный вход — 2FA ещё не включена).
    auto setupSession = fixture.makeSession("2fa-setup");
    const Json registered = fixture.registerUser(setupSession, "vera@example.com", "Вера");
    CHECK_EQ(registered.getString("type"), std::string("ok"));
    CHECK(setupSession->authenticated());

    // 2) Статус до настройки: выключена, не в процессе.
    const Json statusBefore = fixture.call(setupSession, "auth.status2fa");
    CHECK_EQ(statusBefore.get("payload").getBool("enabled"), false);
    CHECK_EQ(statusBefore.get("payload").getBool("pending"), false);

    // 3) Настройка: возвращает секрет (base32) и otpauth-URI.
    const Json setup = fixture.call(setupSession, "auth.setup2fa");
    CHECK_EQ(setup.getString("type"), std::string("ok"));
    const std::string secretB32 = setup.get("payload").getString("secret");
    CHECK(!secretB32.empty());
    CHECK(setup.get("payload").getString("otpauth_uri").rfind("otpauth://totp/", 0) == 0);
    CHECK_EQ(setup.get("payload").getInt("digits"), 6);
    CHECK_EQ(setup.get("payload").getInt("period"), 30);

    // До подтверждения 2FA всё ещё выключена (нельзя «включить» непроверенный секрет).
    CHECK_EQ(fixture.call(setupSession, "auth.status2fa").get("payload").getBool("enabled"), false);
    CHECK_EQ(fixture.call(setupSession, "auth.status2fa").get("payload").getBool("pending"), true);

    // 4) Подтверждение одноразовым кодом (считаем из секрета по RFC 6238).
    std::string rawSecret;
    CHECK(aura::totp::decodeSecretBase32(secretB32, rawSecret));
    Json confirm = Json::object();
    confirm.set("code", Json(aura::totp::codeNow(rawSecret)));
    const Json confirmed = fixture.call(setupSession, "auth.confirm2fa", confirm);
    CHECK_EQ(confirmed.getString("type"), std::string("ok"));
    // Держим Json в именованной переменной: get() возвращает по значению,
    // а items() — ссылку внутрь него (иначе ссылка повисла бы на временном).
    const Json recoveryCodesJson = confirmed.get("payload").get("recovery_codes");
    const auto& recoveryCodes = recoveryCodesJson.items();
    CHECK_EQ(recoveryCodes.size(), std::size_t(10));
    CHECK_EQ(recoveryCodes[0].asString().size(), std::size_t(11));  // XXXXX-XXXXX
    const std::string firstRecovery = recoveryCodes[0].asString();

    // 5) Теперь включена.
    const Json statusAfter = fixture.call(setupSession, "auth.status2fa");
    CHECK_EQ(statusAfter.get("payload").getBool("enabled"), true);
    CHECK_EQ(statusAfter.get("payload").getInt("recovery_codes_left"), 10);

    // 6) Выход; новый вход требует 2FA (пароль → TOTP → сессия).
    fixture.call(setupSession, "auth.logout");
    auto loginSession = fixture.makeSession("2fa-login");
    Json login = Json::object();
    login.set("email", Json("vera@example.com"));
    login.set("password", Json("aura1234"));
    const Json needsCode = fixture.call(loginSession, "auth.login", login);
    CHECK_EQ(needsCode.getString("type"), std::string("error"));
    CHECK_EQ(needsCode.getString("code"), std::string("requires_2fa"));
    CHECK(!loginSession->authenticated());

    // 7) Неверный код отклоняется.
    Json badLogin = login;
    badLogin.set("code", Json("000000"));
    CHECK_EQ(fixture.call(loginSession, "auth.login2fa", badLogin).getString("code"),
             std::string("unauthorized"));
    CHECK(!loginSession->authenticated());

    // 8) Верный TOTP-код → сессия. Текущий код уже использован при confirm2fa
    // (одноразовость), поэтому берём код следующего окна — сервер принимает его
    // (окно ±1 период) и проверка одноразовости проходит.
    const std::int64_t nextWindow = (std::time(nullptr) / aura::totp::kPeriod + 1) * aura::totp::kPeriod;
    Json goodLogin = login;
    goodLogin.set("code", Json(aura::totp::codeAt(rawSecret, nextWindow)));
    goodLogin.set("device_id", Json("device-vera-1"));
    goodLogin.set("trust_device", Json(true));
    const Json loggedIn = fixture.call(loginSession, "auth.login2fa", goodLogin);
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));
    CHECK(loginSession->authenticated());
    CHECK(!loggedIn.get("payload").getString("token").empty());

    // 9) Доверенное устройство зарегистрировано.
    const Json devices = fixture.call(loginSession, "devices.list");
    CHECK_EQ(devices.get("payload").get("devices").items().size(), std::size_t(1));

    // 10) Вход с доверенным device_id пропускает 2FA.
    auto trustedSession = fixture.makeSession("2fa-trusted");
    Json trustedLogin = login;
    trustedLogin.set("device_id", Json("device-vera-1"));
    const Json trustedResult = fixture.call(trustedSession, "auth.login", trustedLogin);
    CHECK_EQ(trustedResult.getString("type"), std::string("ok"));
    CHECK(trustedSession->authenticated());

    // 11) Вход с НЕдоверенным device_id снова требует код.
    auto otherSession = fixture.makeSession("2fa-other");
    Json otherLogin = login;
    otherLogin.set("device_id", Json("device-vera-OTHER"));
    CHECK_EQ(fixture.call(otherSession, "auth.login", otherLogin).getString("code"),
             std::string("requires_2fa"));

    // 12) Резервный код работает и сгорает (одноразовый).
    auto recoverySession = fixture.makeSession("2fa-recovery");
    Json recoveryLogin = login;
    recoveryLogin.set("code", Json(firstRecovery));
    const Json recoveryResult = fixture.call(recoverySession, "auth.login2fa", recoveryLogin);
    CHECK_EQ(recoveryResult.getString("type"), std::string("ok"));
    CHECK(recoverySession->authenticated());
    // Осталось 9 неиспользованных кодов.
    CHECK_EQ(fixture.call(recoverySession, "auth.status2fa").get("payload").getInt("recovery_codes_left"), 9);
    // Тот же код повторно не принимается.
    auto reuseSession = fixture.makeSession("2fa-reuse");
    Json reuseLogin = login;
    reuseLogin.set("code", Json(firstRecovery));
    CHECK_EQ(fixture.call(reuseSession, "auth.login2fa", reuseLogin).getString("code"),
             std::string("unauthorized"));

    // 13) Отзыв доверенного устройства → вход с ним снова требует код.
    const std::string deviceId = devices.get("payload").get("devices").items()[0].getString("id");
    Json revoke = Json::object();
    revoke.set("id", Json(deviceId));
    CHECK_EQ(fixture.call(recoverySession, "devices.revoke", revoke).getString("type"), std::string("ok"));
    auto afterRevoke = fixture.makeSession("2fa-after-revoke");
    Json afterRevokeLogin = login;
    afterRevokeLogin.set("device_id", Json("device-vera-1"));
    CHECK_EQ(fixture.call(afterRevoke, "auth.login", afterRevokeLogin).getString("code"),
             std::string("requires_2fa"));

    // 14) Отключение 2FA требует пароль.
    Json disableBad = Json::object();
    disableBad.set("password", Json("wrongpass1"));
    CHECK_EQ(fixture.call(recoverySession, "auth.disable2fa", disableBad).getString("code"),
             std::string("unauthorized"));
    Json disableOk = Json::object();
    disableOk.set("password", Json("aura1234"));
    CHECK_EQ(fixture.call(recoverySession, "auth.disable2fa", disableOk).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(recoverySession, "auth.status2fa").get("payload").getBool("enabled"), false);

    // 15) После отключения обычный вход снова выдаёт сессию без кода.
    auto plainSession = fixture.makeSession("2fa-plain");
    Json plainLogin = Json::object();
    plainLogin.set("email", Json("vera@example.com"));
    plainLogin.set("password", Json("aura1234"));
    CHECK_EQ(fixture.call(plainSession, "auth.login", plainLogin).getString("type"), std::string("ok"));
    CHECK(plainSession->authenticated());
}

// ------------------------------------------- этап 8: разрешения и подтверждения
TEST(tool_danger_classification) {
    using aura::ToolManager;
    // Опасные: внешние побочные эффекты.
    CHECK(ToolManager::isDangerous("send_message"));
    CHECK(ToolManager::isDangerous("send_email"));
    CHECK(ToolManager::isDangerous("book_table"));
    // Безопасные: локальные/читают.
    CHECK(!ToolManager::isDangerous("create_note"));
    CHECK(!ToolManager::isDangerous("create_reminder"));
    CHECK(!ToolManager::isDangerous("find_cafe"));
    CHECK(!ToolManager::isDangerous("check_calendar"));
    CHECK(!ToolManager::isDangerous("suggest_time"));
    // Режим по умолчанию: опасные → ask, безопасные → allow.
    CHECK_EQ(ToolManager::defaultMode("send_email"), std::string("ask"));
    CHECK_EQ(ToolManager::defaultMode("find_cafe"), std::string("allow"));
    CHECK(ToolManager::isKnownTool("book_table"));
    CHECK(!ToolManager::isKnownTool("launch_missiles"));
}

TEST(ai_tool_permissions_and_pending_actions_db) {
    auto db = aura::makeEmbeddedDatabase("");  // in-memory (без файла)
    long long userId = 0;
    CHECK(db->createUser("perm@example.com", "argon2id$x", "Перм", userId).ok);
    CHECK(userId > 0);

    // По умолчанию разрешения нет.
    CHECK_EQ(db->getToolPermission(userId, "send_email"), std::string(""));
    // Установка и чтение.
    CHECK(db->setToolPermission(userId, "send_email", "allow").ok);
    CHECK_EQ(db->getToolPermission(userId, "send_email"), std::string("allow"));
    // Недопустимый режим отклоняется.
    CHECK(!db->setToolPermission(userId, "send_email", "bogus").ok);
    CHECK_EQ(db->getToolPermission(userId, "send_email"), std::string("allow"));
    // Список разрешений.
    CHECK(db->setToolPermission(userId, "book_table", "deny").ok);
    const auto perms = db->listToolPermissions(userId);
    CHECK_EQ(perms.size(), static_cast<std::size_t>(2));

    // Отложенное действие: создание → поиск → список → разрешение.
    aura::PendingActionRecord pending;
    pending.userId = userId;
    pending.tool = "send_email";
    pending.args = Json::object();
    pending.args.set("to", Json("x@example.com"));
    pending.summary = "Отправить письмо на x@example.com";
    const long long id = db->createPendingAction(pending);
    CHECK(id > 0);
    const auto found = db->findPendingAction(id);
    CHECK(found.has_value());
    CHECK_EQ(found->status, std::string("pending"));
    CHECK_EQ(found->tool, std::string("send_email"));
    CHECK_EQ(found->args.getString("to"), std::string("x@example.com"));
    CHECK_EQ(db->listPendingActions(userId, "pending", 10).size(), static_cast<std::size_t>(1));
    CHECK_EQ(db->listPendingActions(userId, "executed", 10).size(), static_cast<std::size_t>(0));

    Json outcome = Json::object();
    outcome.set("ok", Json(true));
    CHECK(db->resolvePendingAction(id, "executed", outcome).ok);
    CHECK_EQ(db->findPendingAction(id)->status, std::string("executed"));
    CHECK_EQ(db->listPendingActions(userId, "pending", 10).size(), static_cast<std::size_t>(0));
    // Несуществующее действие не разрешается.
    CHECK(!db->resolvePendingAction(99999, "executed", outcome).ok);
}

TEST(ai_permissions_ws_and_confirmation_barrier) {
    Fixture fixture;
    auto session = fixture.makeSession();
    fixture.registerUser(session, "dan@example.com", "Дан");
    const long long userId = session->userId();
    auto& db = fixture.server->database().db();

    // permissions.list: каталог с эффективными режимами и флагом опасности.
    const Json list = fixture.call(session, "permissions.list");
    CHECK_EQ(list.getString("type"), std::string("ok"));
    const Json listPayload = list.get("payload");
    const Json toolsList = listPayload.get("tools");
    bool sawEmail = false;
    for (const auto& tool : toolsList.items()) {
        if (tool.getString("tool") == "send_email") {
            sawEmail = true;
            CHECK(tool.getBool("dangerous"));
            CHECK_EQ(tool.getString("default_mode"), std::string("ask"));
            CHECK_EQ(tool.getString("mode"), std::string("ask"));  // по умолчанию
        }
    }
    CHECK(sawEmail);

    // permissions.set: валидный режим применяется, невалидный отклоняется.
    Json setReq = Json::object();
    setReq.set("tool", Json("send_email"));
    setReq.set("mode", Json("allow"));
    CHECK_EQ(fixture.call(session, "permissions.set", setReq).getString("type"), std::string("ok"));
    setReq.set("mode", Json("bogus"));
    CHECK_EQ(fixture.call(session, "permissions.set", setReq).getString("code"), std::string("bad_request"));
    setReq.set("tool", Json("nope"));
    setReq.set("mode", Json("allow"));
    CHECK_EQ(fixture.call(session, "permissions.set", setReq).getString("code"), std::string("bad_request"));

    // Барьер подтверждения: создаём отложенное опасное действие (как это
    // сделал бы агент) и подтверждаем — сервер исполняет инструмент.
    aura::PendingActionRecord pending;
    pending.userId = userId;
    pending.tool = "send_email";
    pending.args = Json::object();
    pending.args.set("to", Json("x@example.com"));
    pending.args.set("body", Json("привет"));
    pending.summary = "Отправить письмо на x@example.com";
    const long long id = db.createPendingAction(pending);
    CHECK(id > 0);

    const Json pendingList = fixture.call(session, "confirmation.list");
    CHECK_EQ(pendingList.getString("type"), std::string("ok"));
    CHECK(pendingList.get("payload").get("actions").size() >= 1);

    Json approve = Json::object();
    approve.set("id", Json(id));
    const Json approved = fixture.call(session, "confirmation.approve", approve);
    CHECK_EQ(approved.getString("type"), std::string("ok"));
    CHECK_EQ(approved.get("payload").getString("status"), std::string("executed"));
    // Повторное подтверждение уже обработанного — ошибка.
    CHECK_EQ(fixture.call(session, "confirmation.approve", approve).getString("code"),
             std::string("bad_request"));

    // Отклонение: действие помечается denied, не исполняется.
    aura::PendingActionRecord second;
    second.userId = userId;
    second.tool = "book_table";
    second.summary = "Забронировать столик";
    const long long id2 = db.createPendingAction(second);
    Json deny = Json::object();
    deny.set("id", Json(id2));
    const Json denied = fixture.call(session, "confirmation.deny", deny);
    CHECK_EQ(denied.get("payload").getString("status"), std::string("denied"));

    // Чужое отложенное действие недоступно (проверка владения).
    auto other = fixture.makeSession("other");
    fixture.registerUser(other, "eve@example.com", "Ева");
    Json approveOther = Json::object();
    approveOther.set("id", Json(id));
    CHECK_EQ(fixture.call(other, "confirmation.approve", approveOther).getString("code"),
             std::string("not_found"));
}

TEST(ai_tasks_crud_ws) {
    Fixture fixture;
    auto session = fixture.makeSession();
    fixture.registerUser(session, "task@example.com", "Таск");

    Json create = Json::object();
    create.set("title", Json("Купить кофе"));
    create.set("notes", Json("эспрессо"));
    create.set("priority", Json(2));
    const Json created = fixture.call(session, "tasks.create", create);
    CHECK_EQ(created.getString("type"), std::string("ok"));
    const long long taskId = created.get("payload").getInt("id");
    CHECK(taskId > 0);
    CHECK_EQ(created.get("payload").getString("status"), std::string("pending"));
    CHECK_EQ(created.get("payload").getInt("priority"), 2);

    // Пустой заголовок отклоняется.
    Json empty = Json::object();
    empty.set("title", Json(""));
    CHECK_EQ(fixture.call(session, "tasks.create", empty).getString("code"), std::string("bad_request"));

    // Список содержит задачу.
    CHECK(fixture.call(session, "tasks.list", Json::object()).get("payload").get("tasks").size() >= 1);

    // Отметка «выполнено».
    Json complete = Json::object();
    complete.set("id", Json(taskId));
    CHECK_EQ(fixture.call(session, "tasks.complete", complete).get("payload").getString("status"),
             std::string("done"));

    // Фильтр по статусу pending теперь пуст.
    Json pending = Json::object();
    pending.set("status", Json("pending"));
    CHECK_EQ(fixture.call(session, "tasks.list", pending).get("payload").get("tasks").size(),
             static_cast<std::size_t>(0));

    // Чужая задача недоступна (проверка владения).
    auto other = fixture.makeSession("other");
    fixture.registerUser(other, "other@example.com", "Другой");
    CHECK_EQ(fixture.call(other, "tasks.complete", complete).getString("code"), std::string("not_found"));

    // Удаление: первый раз ок, второй — not_found.
    CHECK_EQ(fixture.call(session, "tasks.delete", complete).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(session, "tasks.delete", complete).getString("code"), std::string("not_found"));
}

TEST(ai_task_scheduler_tick) {
    Fixture fixture;
    auto session = fixture.makeSession();
    fixture.registerUser(session, "sched@example.com", "Шед");
    auto& db = fixture.server->database().db();

    // Задача с напоминанием, срок которого уже наступил (remind_at в прошлом).
    aura::TaskRecord task;
    task.userId = session->userId();
    task.title = "Позвонить клиенту";
    task.remindAt = "2020-01-01T00:00:00.000Z";  // заведомо <= now
    long long id = 0;
    CHECK(db.createTask(task, id).ok);
    CHECK(db.findTask(id)->remindedAt.empty());
    CHECK_EQ(db.listDueTasks(10).size(), static_cast<std::size_t>(1));

    // Проход планировщика помечает напомненным и возвращает задачу.
    CHECK_EQ(fixture.server->runSchedulerTick(10), static_cast<std::size_t>(1));
    CHECK(!db.findTask(id)->remindedAt.empty());
    // Повторно не отправляется (одноразово).
    CHECK_EQ(fixture.server->runSchedulerTick(10), static_cast<std::size_t>(0));

    // Задача без remind_at не становится due.
    aura::TaskRecord noRemind;
    noRemind.userId = session->userId();
    noRemind.title = "Без напоминания";
    long long id2 = 0;
    CHECK(db.createTask(noRemind, id2).ok);
    CHECK_EQ(fixture.server->runSchedulerTick(10), static_cast<std::size_t>(0));
}

TEST(ai_create_reminder_makes_task) {
    Fixture fixture;
    auto session = fixture.makeSession();
    fixture.registerUser(session, "rem@example.com", "Рем");

    Json run = Json::object();
    run.set("tool", Json("create_reminder"));
    Json args = Json::object();
    args.set("text", Json("выпить воды"));
    args.set("at", Json("2030-01-01T10:00:00.000Z"));  // будущее → не due
    run.set("args", args);
    const Json result = fixture.call(session, "tool.run", run);
    CHECK_EQ(result.getString("type"), std::string("ok"));
    CHECK(result.get("payload").getInt("task_id") > 0);

    // Напоминание видно как задача; срок в будущем → планировщик не трогает.
    CHECK(fixture.call(session, "tasks.list", Json::object()).get("payload").get("tasks").size() >= 1);
    CHECK_EQ(fixture.server->runSchedulerTick(10), static_cast<std::size_t>(0));
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // сессии в тестах пишут в socketpair
    std::cout.setf(std::ios::unitbuf);
    const std::string filter = argc > 1 ? argv[1] : "";
    int executed = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++executed;
        std::cout << "[ RUN  ] " << test.name << "\n";
        const int before = g_failures;
        test.body();
        std::cout << (g_failures == before ? "[  OK  ] " : "[ FAIL ] ") << test.name << "\n";
    }
    std::cout << "\nтестов: " << executed << ", проверок: " << g_checks << ", ошибок: " << g_failures
              << "\n";
    return g_failures == 0 ? 0 : 1;
}
