// selfvoy/server/tests/test_main.cpp — юнит-тесты сервера без внешних зависимостей.
//
// Запуск: make test  (или ./selfvoy-tests)
//
// Проверяются: JSON, криптография (эталонные векторы), JWT, кодек WebSocket,
// протокол и сквозные сценарии через Server::handleMessage на встроенной БД.
#include <argon2.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <chrono>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "selfvoy/authmanager.h"
#include "selfvoy/bookingmanager.h"
#include "selfvoy/calendarfeed.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/invitemanager.h"
#include "selfvoy/oauthauthmanager.h"
#include "selfvoy/mailprovider.h"
#include "selfvoy/json.h"
#include "selfvoy/jwt.h"
#include "selfvoy/mailprovider.h"
#include "selfvoy/memorymanager.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"
#include "selfvoy/schedulelock.h"
#include "selfvoy/server.h"
#include "selfvoy/session.h"
#include "selfvoy/slotspost.h"
#include "selfvoy/plan.h"
#include "selfvoy/smtp.h"
#include "selfvoy/telegrambot.h"
#include "selfvoy/tls.h"
#include "selfvoy/toolmanager.h"
#include "selfvoy/totp.h"
#include "selfvoy/ws.h"
#include "../src/notificationsmanager.h"  // уведомления владельцу → Telegram (рост, S1a)

#ifdef SELFVOY_WITH_LIBPQ
#include <libpq-fe.h>
#endif

#ifdef SELFVOY_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

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

using selfvoy::Json;

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
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::sha256("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::sha256("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::sha1("abc")),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));

    // RFC 4231, тест-кейс 1
    const std::string key(20, '\x0b');
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::hmacSha256(key, "Hi There")),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    // RFC 6070-подобный вектор для PBKDF2-HMAC-SHA256: password/salt/1 итерация
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::pbkdf2Sha256("password", "salt", 1, 32)),
             std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
}

TEST(crypto_base64) {
    CHECK_EQ(selfvoy::crypto::base64Encode("Man"), std::string("TWFu"));
    CHECK_EQ(selfvoy::crypto::base64Encode("Ma"), std::string("TWE="));
    CHECK_EQ(selfvoy::crypto::base64Encode("M"), std::string("TQ=="));
    // UTF-8 «привет» → 0L/RgNC40LLQtdGC (проверено python3 -c base64)
    CHECK_EQ(selfvoy::crypto::base64Encode("привет"), std::string("0L/RgNC40LLQtdGC"));
    std::string decoded;
    CHECK(selfvoy::crypto::base64Decode("0L/RgNC40LLQtdGC", decoded));
    CHECK_EQ(decoded, std::string("привет"));
    CHECK_EQ(selfvoy::crypto::base64UrlEncode(std::string("\xfb\xff", 2)).find('='), std::string::npos);
}

TEST(smtp_message_format) {
    selfvoy::smtp::Settings settings;
    settings.from = "Сервис Selfvoy <no-reply@selfvoy.example>";
    selfvoy::smtp::Message message;
    message.to = "anna@example.com";
    message.subject = "Код подтверждения Selfvoy: 123456";
    message.text = "Код: 123456";
    message.html = "<b>123456</b>";
    const std::string mail =
        selfvoy::smtp::buildMessage(settings, message, "Tue, 13 Oct 2026 12:00:00 +0000", "<id@selfvoy.example>");
    CHECK(mail.find("To: <anna@example.com>\r\n") != std::string::npos);
    CHECK(mail.find("Subject: =?UTF-8?B?") != std::string::npos);
    CHECK(mail.find("From: =?UTF-8?B?") != std::string::npos);
    CHECK(mail.find("<no-reply@selfvoy.example>") != std::string::npos);
    CHECK(mail.find("multipart/alternative") != std::string::npos);
    CHECK(mail.find("Message-ID: <id@selfvoy.example>") != std::string::npos);
    // Только CRLF, без одиночных LF, и тело в base64 (не-ASCII не уходит «как есть»).
    for (std::size_t i = 0; i < mail.size(); ++i) {
        if (mail[i] == '\n') CHECK(i > 0 && mail[i - 1] == '\r');
    }
    std::string decoded;
    CHECK(selfvoy::crypto::base64Decode(selfvoy::crypto::base64Encode("Код: 123456"), decoded));
    CHECK(mail.find(selfvoy::crypto::base64Encode("Код: 123456")) != std::string::npos);

    CHECK(!selfvoy::smtp::safeHeaderValue("x\r\nBcc: evil@example.com"));
    CHECK_EQ(selfvoy::smtp::addressOf("Selfvoy <no-reply@selfvoy.example>"), std::string("no-reply@selfvoy.example"));
    CHECK_EQ(selfvoy::smtp::addressOf(" a@b.c "), std::string("a@b.c"));
}

TEST(smtp_dialogue_with_fake_server) {
    // Поддельный SMTP-сервер на 127.0.0.1: проверяем порядок команд, AUTH и DATA.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    CHECK_EQ(::listen(listener, 1), 0);
    socklen_t len = sizeof addr;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const int port = ntohs(addr.sin_port);

    std::vector<std::string> commands;
    std::string data;
    std::thread fake([&] {
        const int fd = ::accept(listener, nullptr, nullptr);
        std::string buffer;
        auto line = [&]() {
            for (;;) {
                const auto eol = buffer.find("\r\n");
                if (eol != std::string::npos) {
                    std::string out = buffer.substr(0, eol);
                    buffer.erase(0, eol + 2);
                    return out;
                }
                char chunk[1024];
                const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
                if (n <= 0) return std::string("<eof>");
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
        };
        auto say = [&](const std::string& text) { ::send(fd, text.data(), text.size(), MSG_NOSIGNAL); };
        say("220 fake ESMTP\r\n");
        for (;;) {
            const std::string cmd = line();
            commands.push_back(cmd);
            if (cmd.rfind("EHLO", 0) == 0) say("250-fake\r\n250 AUTH LOGIN PLAIN\r\n");
            else if (cmd.rfind("AUTH PLAIN", 0) == 0) say("235 ok\r\n");
            else if (cmd.rfind("MAIL FROM", 0) == 0 || cmd.rfind("RCPT TO", 0) == 0) say("250 ok\r\n");
            else if (cmd == "DATA") {
                say("354 go\r\n");
                for (std::string l = line(); l != "." && l != "<eof>"; l = line()) data += l + "\n";
                say("250 queued\r\n");
            } else if (cmd == "QUIT") {
                say("221 bye\r\n");
                break;
            } else {
                say("500 ?\r\n");
                if (cmd == "<eof>") break;
            }
        }
        ::close(fd);
    });

    selfvoy::smtp::Settings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.tls = "none";  // localhost: AUTH без TLS разрешён только для ретранслятора на этой машине
    settings.user = "mailer";
    settings.password = "secret-pass";
    settings.from = "Selfvoy <no-reply@selfvoy.example>";
    settings.timeoutMs = 5000;
    selfvoy::smtp::Message message;
    message.to = "anna@example.com";
    message.subject = "Код подтверждения Selfvoy: 654321";
    message.text = "Ваш код: 654321";
    std::string error;
    const bool sent = selfvoy::smtp::send(settings, message, error);
    fake.join();
    ::close(listener);

    CHECK(sent);
    CHECK_EQ(error, std::string());
    CHECK(commands.size() >= 6);
    CHECK_EQ(commands[0], std::string("EHLO selfvoy.example"));
    CHECK_EQ(commands[1], std::string("AUTH PLAIN ") +
                              selfvoy::crypto::base64Encode(std::string("\0mailer\0secret-pass", 19)));
    CHECK_EQ(commands[2], std::string("MAIL FROM:<no-reply@selfvoy.example>"));
    CHECK_EQ(commands[3], std::string("RCPT TO:<anna@example.com>"));
    CHECK(data.find(selfvoy::crypto::base64Encode("Ваш код: 654321")) != std::string::npos);

    // Подстановка заголовка в адресе отклоняется до соединения.
    message.to = "anna@example.com\r\nBcc: evil@example.com";
    CHECK(!selfvoy::smtp::send(settings, message, error));
    CHECK(error.find("недопустимый") != std::string::npos);
}

TEST(crypto_password_hash) {
    // Новый пароль хэшируется в Argon2id.
    const std::string hash = selfvoy::crypto::hashPassword("selfvoy1234");
    CHECK(hash.rfind("argon2id$v=19$m=", 0) == 0);
    CHECK(selfvoy::crypto::verifyPassword("selfvoy1234", hash));
    CHECK(!selfvoy::crypto::verifyPassword("selfvoy1235", hash));
    CHECK(!selfvoy::crypto::verifyPassword("selfvoy1234", "argon2id$v=19$m=65536,t=3,p=4$bad"));
    CHECK(!selfvoy::crypto::passwordNeedsRehash(hash));
    CHECK(selfvoy::crypto::constantTimeEquals("secret", "secret"));
    CHECK(!selfvoy::crypto::constantTimeEquals("secret", "secrets"));
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
    const std::string tagHex = selfvoy::crypto::toHex(tag);
    // Known-answer: тег сверен с независимой реализацией (argon2-cffi, Python)
    // для этих же параметров. Совпадение подтверждает корректность libargon2.
    CHECK_EQ(tagHex,
             std::string("c7904b6301d03676acbd4dd657486c42509ccc6d0113fce6495ef4b693392fbf"));
    // Тег детерминирован: повторный вызов даёт тот же результат.
    std::string tag2(32, '\0');
    argon2id_hash_raw(3, 64, 2, password.data(), password.size(),
                      saltRaw.data(), saltRaw.size(), tag2.data(), tag2.size());
    CHECK_EQ(tagHex, selfvoy::crypto::toHex(tag2));
    // Неверный пароль меняет тег.
    std::string tagBad(32, '\0');
    argon2id_hash_raw(3, 64, 2, "Password", 8, saltRaw.data(), saltRaw.size(),
                      tagBad.data(), tagBad.size());
    CHECK(tagHex != selfvoy::crypto::toHex(tagBad));

    // Тот же тег проверяется через argon2idVerify (собираем encoded-строку).
    const std::string encoded = "argon2id$v=19$m=64,t=3,p=2$" +
                                selfvoy::crypto::toHex(saltRaw) + "$" + tagHex;
    CHECK(selfvoy::crypto::argon2idVerify("password", encoded));
    CHECK(!selfvoy::crypto::argon2idVerify("password2", encoded));
}

TEST(crypto_password_migration) {
    // Старый PBKDF2-хэш остаётся проверяемым (прозрачная миграция на входе).
    const std::string salt = "aabbccddeeff0011";
    const std::string legacyHash = "pbkdf2$1000$" + salt + "$" +
        selfvoy::crypto::toHex(selfvoy::crypto::pbkdf2Sha256("selfvoy1234", salt, 1000, 32));
    CHECK(selfvoy::crypto::verifyPassword("selfvoy1234", legacyHash));
    CHECK(!selfvoy::crypto::verifyPassword("selfvoy1235", legacyHash));
    CHECK(selfvoy::crypto::passwordNeedsRehash(legacyHash));
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
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::hmacSha1(key, "Hi There")),
             std::string("b617318655057264e28bc0b6fb378c8ef146be00"));
    // RFC 2202, case 2.
    CHECK_EQ(selfvoy::crypto::toHex(selfvoy::crypto::hmacSha1("Jefe", "what do ya want for nothing?")),
             std::string("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
}

TEST(crypto_base32_rfc4648) {
    CHECK_EQ(selfvoy::crypto::base32Encode(""), std::string(""));
    CHECK_EQ(selfvoy::crypto::base32Encode("f"), std::string("MY======"));
    CHECK_EQ(selfvoy::crypto::base32Encode("fo"), std::string("MZXQ===="));
    CHECK_EQ(selfvoy::crypto::base32Encode("foo"), std::string("MZXW6==="));
    CHECK_EQ(selfvoy::crypto::base32Encode("foob"), std::string("MZXW6YQ="));
    CHECK_EQ(selfvoy::crypto::base32Encode("fooba"), std::string("MZXW6YTB"));
    CHECK_EQ(selfvoy::crypto::base32Encode("foobar"), std::string("MZXW6YTBOI======"));
}

TEST(crypto_aes256_gcm) {
    const std::string key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    // Known-answer: вектор вычислен независимой библиотекой
    // (Python cryptography/OpenSSL) для nonce=000102…0b, aad="aura-2fa-secret"
    // (исторический AAD TOTP-секретов, при переименовании в Selfvoy не меняется).
    const std::string blob =
        unhex("000102030405060708090a0b0d40854c9cd6864bc809c7c082b9203d8ce94805d58e969523c40d978a79c599");
    const std::string aad = "aura-2fa-secret";
    std::string plain;
    CHECK(selfvoy::crypto::aes256GcmDecrypt(key, blob, aad, plain));
    CHECK_EQ(plain, std::string("JBSWY3DPEHPK3PXP"));

    // Подмены отклоняются: другой AAD, другой ключ, испорченный байт.
    CHECK(!selfvoy::crypto::aes256GcmDecrypt(key, blob, "wrong-aad", plain));
    std::string badKey = key;
    badKey[0] = '\x01';
    CHECK(!selfvoy::crypto::aes256GcmDecrypt(badKey, blob, aad, plain));
    std::string tampered = blob;
    tampered[13] = static_cast<char>(tampered[13] ^ 0x01);
    CHECK(!selfvoy::crypto::aes256GcmDecrypt(key, tampered, aad, plain));

    // Roundtrip: шифрование → расшифровка, включая длинный текст.
    std::string blob2;
    CHECK(selfvoy::crypto::aes256GcmEncrypt(key, "some longer secret value 12345", aad, blob2));
    std::string back;
    CHECK(selfvoy::crypto::aes256GcmDecrypt(key, blob2, aad, back));
    CHECK_EQ(back, std::string("some longer secret value 12345"));

    // Второй known-answer: пустой AAD, короткий текст.
    const std::string blob3 = unhex("00000000000000000000000066d51ec39250ea9203b6e1e6789b7a0f3082");
    CHECK(selfvoy::crypto::aes256GcmDecrypt(key, blob3, "", back));
    CHECK_EQ(back, std::string("hi"));

    // Ключ неверной длины отклоняется.
    CHECK(!selfvoy::crypto::aes256GcmEncrypt("short", "x", "", blob2));
    CHECK(!selfvoy::crypto::aes256GcmDecrypt("short", blob, aad, plain));
}

TEST(totp_rfc6238_vectors) {
    // RFC 6238, приложение B (SHA-1, секрет — ASCII "12345678901234567890").
    // В таблице RFC коды 8-значные; Selfvoy использует 6 цифр — сверяем последние 6.
    const std::string secret = "12345678901234567890";
    CHECK_EQ(selfvoy::totp::codeAt(secret, 59), std::string("287082"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 1111111109), std::string("081804"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 1111111111), std::string("050471"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 1234567890), std::string("005924"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 2000000000), std::string("279037"));  // RFC: 69279037
    CHECK_EQ(selfvoy::totp::codeAt(secret, 20000000000), std::string("353130"));
    // 8-значный режим даёт ровно значения из таблицы RFC.
    CHECK_EQ(selfvoy::totp::codeAt(secret, 59, 8), std::string("94287082"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 2000000000, 8), std::string("69279037"));
    CHECK_EQ(selfvoy::totp::codeAt(secret, 20000000000, 8), std::string("65353130"));
}

TEST(totp_verify_window_and_reuse) {
    const std::string secretB32 = selfvoy::totp::generateSecretBase32();
    std::string raw;
    CHECK(selfvoy::totp::decodeSecretBase32(secretB32, raw));
    CHECK_EQ(raw.size(), std::size_t(20));

    const std::int64_t now = 900;  // счётчик 30
    const std::string currentCode = selfvoy::totp::codeAt(raw, now);
    std::int64_t matched = -1;
    CHECK(selfvoy::totp::verifyAt(raw, currentCode, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(30));

    // Одноразовость: тот же код с lastUsedCounter=30 отклоняется.
    CHECK(!selfvoy::totp::verifyAt(raw, currentCode, now, 30, matched));

    // Окно ±1 период: коды соседних счётчиков принимаются.
    const std::string prev = selfvoy::totp::codeAt(raw, 29 * 30);
    CHECK(selfvoy::totp::verifyAt(raw, prev, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(29));
    const std::string next = selfvoy::totp::codeAt(raw, 31 * 30);
    CHECK(selfvoy::totp::verifyAt(raw, next, now, 0, matched));
    CHECK_EQ(matched, std::int64_t(31));

    // Два периода назад — уже вне окна.
    const std::string old = selfvoy::totp::codeAt(raw, 28 * 30);
    CHECK(!selfvoy::totp::verifyAt(raw, old, now, 0, matched));

    // Неверный код, нецифровой ввод, короткая строка.
    std::string wrong = currentCode;
    wrong[0] = (wrong[0] == '9') ? '0' : static_cast<char>(wrong[0] + 1);
    CHECK(!selfvoy::totp::verifyAt(raw, wrong, now, 0, matched));
    CHECK(!selfvoy::totp::verifyAt(raw, "abcdef", now, 0, matched));
    CHECK(!selfvoy::totp::verifyAt(raw, "12345", now, 0, matched));
}

TEST(totp_secret_decode_and_uri) {
    // Канонический тестовый секрет: "JBSWY3DPEHPK3PXP" = "Hello!" + deadbeef.
    std::string raw;
    CHECK(selfvoy::totp::decodeSecretBase32("JBSWY3DPEHPK3PXP", raw));
    CHECK_EQ(selfvoy::crypto::toHex(raw), std::string("48656c6c6f21deadbeef"));
    // Строчные буквы, пробелы, дефисы — допустимы.
    CHECK(selfvoy::totp::decodeSecretBase32("jbsw y3dp-ehpk3pxp", raw));
    CHECK_EQ(selfvoy::crypto::toHex(raw), std::string("48656c6c6f21deadbeef"));
    // Недопустимый символ и слишком короткий секрет.
    CHECK(!selfvoy::totp::decodeSecretBase32("JBSWY3DPEHPK3PX!", raw));
    CHECK(!selfvoy::totp::decodeSecretBase32("MY======", raw));

    const std::string uri = selfvoy::totp::otpauthUri("JBSWY3DPEHPK3PXP", "anna@selfvoy.app", "Selfvoy");
    CHECK(uri.rfind("otpauth://totp/Selfvoy:anna%40selfvoy.app?", 0) == 0);
    CHECK(uri.find("secret=JBSWY3DPEHPK3PXP") != std::string::npos);
    CHECK(uri.find("issuer=Selfvoy") != std::string::npos);
    CHECK(uri.find("algorithm=SHA1") != std::string::npos);
    CHECK(uri.find("digits=6") != std::string::npos);
    CHECK(uri.find("period=30") != std::string::npos);
}

// --------------------------------------------------------------------- JWT
TEST(jwt_sign_and_verify) {
    Json claims = Json::object();
    claims.set("sub", Json(42));
    claims.set("email", Json("anna@example.com"));
    const std::string token = selfvoy::jwt::sign(claims, "test-secret", 60);
    CHECK_EQ(std::count(token.begin(), token.end(), '.'), 2);

    const selfvoy::jwt::Token verified = selfvoy::jwt::verify(token, "test-secret");
    CHECK(verified.valid);
    CHECK_EQ(verified.claims.getInt("sub"), 42);
    CHECK_EQ(verified.claims.getString("email"), std::string("anna@example.com"));

    CHECK(!selfvoy::jwt::verify(token, "other-secret").valid);
    const selfvoy::jwt::Token expired = selfvoy::jwt::verify(token, "test-secret", selfvoy::jwt::nowEpoch() + 3600);
    CHECK(!expired.valid);
    CHECK(!selfvoy::jwt::verify("not.a.token", "test-secret").valid);
}

// --------------------------------------------------------------- WebSocket
TEST(ws_accept_token_rfc6455) {
    // Пример из RFC 6455, раздел 1.3
    CHECK_EQ(selfvoy::ws::acceptToken("dGhlIHNhbXBsZSBub25jZQ=="),
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
    const selfvoy::ws::Handshake handshake = selfvoy::ws::parseHandshake(request);
    CHECK(handshake.valid);
    CHECK_EQ(handshake.path, std::string("/ws"));
    CHECK_EQ(handshake.key, std::string("dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK_EQ(handshake.authorization, std::string("Bearer abc.def.ghi"));
    CHECK_EQ(handshake.origin, std::string("http://localhost"));

    CHECK(!selfvoy::ws::parseHandshake("POST / HTTP/1.1\r\n\r\n").valid);
    CHECK(!selfvoy::ws::parseHandshake("GET / HTTP/1.1\r\nHost: x\r\n\r\n").valid);
    CHECK(selfvoy::ws::handshakeResponse("dGhlIHNhbXBsZSBub25jZQ==").find("101 Switching Protocols") !=
          std::string::npos);
}

TEST(ws_frame_roundtrip) {
    const std::string payload = R"({"type":"ping","payload":{"n":1}})";
    const std::string frame = selfvoy::ws::encodeFrame(selfvoy::ws::Opcode::Text, payload);
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
    selfvoy::ws::Frame decoded;
    std::string error;
    const auto result = selfvoy::ws::decodeFrame(masked, 1024 * 1024, consumed, decoded, error);
    CHECK(result == selfvoy::ws::DecodeResult::Complete);
    CHECK_EQ(consumed, masked.size());
    CHECK_EQ(decoded.payload, payload);
    CHECK(decoded.opcode == selfvoy::ws::Opcode::Text);

    // Неполный кадр
    std::size_t consumedPartial = 0;
    selfvoy::ws::Frame partial;
    CHECK(selfvoy::ws::decodeFrame(masked.substr(0, 3), 1024, consumedPartial, partial, error) ==
          selfvoy::ws::DecodeResult::Incomplete);

    // Превышение лимита
    std::size_t consumedBig = 0;
    selfvoy::ws::Frame big;
    CHECK(selfvoy::ws::decodeFrame(masked, 10, consumedBig, big, error) ==
          selfvoy::ws::DecodeResult::ProtocolError);
}

// ---------------------------------------------------------------- protocol
TEST(protocol_envelope) {
    const Json okMessage = selfvoy::protocol::ok("req-1", [] {
        Json payload = Json::object();
        payload.set("value", Json(1));
        return payload;
    }());
    CHECK_EQ(okMessage.getString("type"), std::string("ok"));
    CHECK_EQ(okMessage.getString("id"), std::string("req-1"));
    CHECK_EQ(okMessage.get("payload").getInt("value"), 1);

    const Json errorMessage = selfvoy::protocol::error("req-2", selfvoy::protocol::code::kUnauthorized, "нужен вход");
    CHECK_EQ(errorMessage.getString("code"), std::string("unauthorized"));

    const Json eventMessage = selfvoy::protocol::event("chat.message");
    CHECK_EQ(eventMessage.getString("event"), std::string("chat.message"));

    selfvoy::protocol::Request request;
    std::string error;
    CHECK(!selfvoy::protocol::parse(Json::array(), request, error));
    CHECK(selfvoy::protocol::isPublicType("auth.login"));
    CHECK(!selfvoy::protocol::isPublicType("chat.send"));
}

// --------------------------------------------------------------------- net
TEST(net_url_parsing) {
    selfvoy::net::Url url;
    std::string error;
    CHECK(selfvoy::net::parseUrl("http://127.0.0.1:8000/v1/agent/run", url, error));
    CHECK_EQ(url.host, std::string("127.0.0.1"));
    CHECK_EQ(url.port, 8000);
    CHECK_EQ(url.path, std::string("/v1/agent/run"));

    CHECK(selfvoy::net::parseUrl("http://ai-service/healthz", url, error));
    CHECK_EQ(url.port, 80);
    CHECK_EQ(url.path, std::string("/healthz"));

    // https — порт 443 по умолчанию (TLS — selfvoy/tls.h); прочие схемы и порты вне 1–65535 — нет.
    CHECK(selfvoy::net::parseUrl("https://api.telegram.org/bot1:x/getMe", url, error));
    CHECK_EQ(url.scheme, std::string("https"));
    CHECK_EQ(url.port, 443);
    CHECK_EQ(url.host, std::string("api.telegram.org"));
    CHECK_EQ(url.path, std::string("/bot1:x/getMe"));
    CHECK(selfvoy::net::parseUrl("HTTPS://example.com:8443", url, error));
    CHECK_EQ(url.port, 8443);
    CHECK_EQ(url.path, std::string("/"));
    error.clear();
    CHECK(!selfvoy::net::parseUrl("ftp://example.com", url, error));
    CHECK(!error.empty());
    CHECK(!selfvoy::net::parseUrl("https://example.com:70000/", url, error));

    // Разбор chunked-ответа
    const std::string chunked = "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n";
    selfvoy::net::HttpResponse response;
    CHECK(selfvoy::net::parseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n", response));
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.header("transfer-encoding"), std::string("chunked"));
    CHECK_EQ(selfvoy::net::decodeChunked(chunked), std::string("Hello World"));
}

// -------------------------------------------------------------------- auth
TEST(auth_validation_rules) {
    CHECK_EQ(selfvoy::AuthManager::normalizeEmail("  Anna@Example.COM "), std::string("anna@example.com"));
    CHECK(!selfvoy::AuthManager::passwordError("short1").empty());
    CHECK(!selfvoy::AuthManager::passwordError("onlyletters").empty());
    CHECK(!selfvoy::AuthManager::passwordError("12345678").empty());
    CHECK(selfvoy::AuthManager::passwordError("selfvoy1234").empty());
}

// ------------------------------------------------------------- интеграция
namespace integration {

struct Fixture {
    selfvoy::Config config;
    std::string dbPath;
    std::unique_ptr<selfvoy::Server> server;
    int observerFd = -1;

    Fixture() {
        dbPath = "/tmp/selfvoy-test-" + std::to_string(::getpid()) + ".json";
        std::remove(dbPath.c_str());
        config = selfvoy::Config::fromEnv();
        config.databaseUrl.clear();  // встроенное хранилище
        config.embeddedDbPath = dbPath;
        config.port = 0;             // свободный порт
        config.jwtSecret = "test-secret";
        config.devMode = true;        // слабый секрет и mail=dev допустимы только в dev-режиме
        config.mailDriver = "dev";    // коды подтверждения возвращаются в ответе API
        config.logLevel = "warn";
        config.aiServiceUrl = "http://127.0.0.1:1";  // гарантированно недоступен
        config.aiTimeoutMs = 500;
        config.schedulerIntervalMs = 0;  // фоновый поток не нужен: tick зовём вручную
        server = std::make_unique<selfvoy::Server>(config);
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
    std::shared_ptr<selfvoy::Session> makeSession(const std::string& id = "test") {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            std::cout << "  socketpair failed\n";
            ++g_failures;
            return nullptr;
        }
        if (observerFd >= 0) ::close(observerFd);
        observerFd = fds[0];
        return std::make_shared<selfvoy::Session>(fds[1], id, "127.0.0.1:0", config);
    }

    Json call(const std::shared_ptr<selfvoy::Session>& session,
              const std::string& type,
              const Json& payload = Json::object()) {
        Json message = Json::object();
        message.set("id", Json("req-" + type));
        message.set("type", Json(type));
        message.set("payload", payload);
        return server->handleMessage(session, message);
    }

    Json registerUser(const std::shared_ptr<selfvoy::Session>& session,
                      const std::string& email,
                      const std::string& name) {
        Json payload = Json::object();
        payload.set("email", Json(email));
        payload.set("password", Json("selfvoy1234"));
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
        login.set("password", Json("selfvoy1234"));
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
    regPayload.set("password", Json("selfvoy1234"));
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
    login.set("password", Json("selfvoy1234"));
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
    const Json again = fixture.call(session, "auth.verifyEmail", verify);
    CHECK(again.get("payload").getBool("verified"));
    // Повтор для уже подтверждённого адреса не раскрывает данные пользователя.
    CHECK(again.get("payload").get("user").isNull());

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

TEST(server_account_deletion_flow) {
    Fixture fixture;
    auto session = fixture.makeSession();
    const Json loggedIn = fixture.registerUser(session, "del_test@example.com", "Для удаления");
    CHECK_EQ(loggedIn.getString("type"), std::string("ok"));
    CHECK(session->authenticated());
    const long long userId = session->userId();

    // Проверяем, что пользователь существует в БД
    auto userBefore = fixture.server->database().db().findUserById(userId);
    CHECK(userBefore.has_value());

    // Вызываем удаление аккаунта
    const Json deleted = fixture.call(session, "auth.deleteAccount");
    CHECK_EQ(deleted.getString("type"), std::string("ok"));
    CHECK(deleted.get("payload").getBool("deleted"));
    CHECK(!session->authenticated());

    // Пользователь полностью удален из БД
    auto userAfter = fixture.server->database().db().findUserById(userId);
    CHECK(!userAfter.has_value());

    // Попытка входа с прежними учетными данными возвращает unauthorized
    auto newSession = fixture.makeSession("after_del");
    Json login = Json::object();
    login.set("email", Json("del_test@example.com"));
    login.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(newSession, "auth.login", login).getString("code"), std::string("unauthorized"));
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
    login.set("password", Json("selfvoy1234"));
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
    login.set("password", Json("selfvoy1234"));
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
        selfvoy::crypto::toHex(selfvoy::crypto::pbkdf2Sha256("selfvoy1234", salt, 1000, 32));
    long long userId = 0;
    CHECK(fixture.server->database().db()
              .createUser("legacy@example.com", legacy, "Леги", userId)
              .ok);
    CHECK(fixture.server->database().db().setEmailVerified(userId, true).ok);

    auto session = fixture.makeSession();
    Json login = Json::object();
    login.set("email", Json("legacy@example.com"));
    login.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(session, "auth.login", login).getString("type"), std::string("ok"));

    // После входа хэш пересчитан в Argon2id.
    const auto user = fixture.server->database().db().findUserByEmail("legacy@example.com");
    CHECK(user.has_value());
    CHECK(user->passwordHash.rfind("argon2id$", 0) == 0);
    CHECK(selfvoy::crypto::verifyPassword("selfvoy1234", user->passwordHash));
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
    change.set("old_password", Json("selfvoy1234"));
    CHECK(fixture.call(session, "auth.changePassword", change).get("payload").getBool("changed"));
    auto fresh = fixture.makeSession("fresh");
    Json login = Json::object();
    login.set("email", Json("kai@example.com"));
    login.set("password", Json("selfvoy1234"));
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

    // Поиск пользователей (M11): по подстроке email больше не находит,
    // по имени — да, по точному email — да; наружу только публичная карточка.
    Json search = Json::object();
    search.set("query", Json("anya"));
    CHECK_EQ(integration::payloadOf(fixture.call(anna, "users.search", search))
                 .get("users")
                 .size(),
             static_cast<std::size_t>(0));
    search.set("query", Json("Аня"));
    const Json byName = integration::payloadOf(fixture.call(anna, "users.search", search)).get("users");
    CHECK_EQ(byName.size(), static_cast<std::size_t>(1));
    CHECK(!byName.at(0).contains("email"));
    CHECK(!byName.at(0).contains("last_seen_at"));
    CHECK(!byName.at(0).contains("email_verified"));
    CHECK(byName.at(0).getInt("id") > 0);
    CHECK_EQ(byName.at(0).getString("display_name"), std::string("Аня"));
    search.set("query", Json("anya@example.com"));
    CHECK_EQ(integration::payloadOf(fixture.call(anna, "users.search", search)).get("users").size(),
             static_cast<std::size_t>(1));
    search.set("query", Json("anya@example.co"));
    CHECK_EQ(integration::payloadOf(fixture.call(anna, "users.search", search)).get("users").size(),
             static_cast<std::size_t>(0));
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
    session->send(selfvoy::protocol::event("test.event", payload));

    // Читаем кадры из второго конца socketpair, пока не встретим test.event.
    // Вход (внутри registerUser) теперь порождает событие notification.new
    // («Новый вход в Selfvoy», этап 13) — проверяем, что оно приходит первым.
    // Кадр пришёл от сервера, поэтому маска в нём запрещена (requireMask=false).
    std::string raw;
    bool sawNotification = false;
    bool sawTestEvent = false;
    Json testEvent;
    for (int attempt = 0; attempt < 8 && !sawTestEvent; ++attempt) {
        char buffer[1024];
        const ssize_t received = ::recv(fixture.observerFd, buffer, sizeof(buffer), 0);
        CHECK(received > 0);
        raw.append(buffer, static_cast<std::size_t>(received));
        while (true) {
            selfvoy::ws::Frame frame;
            std::string error;
            std::size_t consumed = 0;
            const auto result =
                selfvoy::ws::decodeFrame(raw, raw.size(), consumed, frame, error, false);
            if (result != selfvoy::ws::DecodeResult::Complete) break;
            raw.erase(0, consumed);
            const Json decoded = Json::parse(frame.payload);
            if (decoded.getString("event") == std::string("notification.new")) {
                sawNotification = true;
            } else if (decoded.getString("event") == std::string("test.event")) {
                sawTestEvent = true;
                testEvent = decoded;
                break;
            }
        }
    }
    CHECK(sawNotification);
    CHECK(sawTestEvent);
    CHECK_EQ(testEvent.get("payload").getString("hello"), std::string("мир"));
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
    // L20: book_table опасный (default ask) — прямой tool.run не исполняет его,
    // а создаёт отложенное действие; после allow — исполняет сразу.
    const Json pendingBook = fixture.call(session, "tool.run", book);
    CHECK_EQ(pendingBook.getString("type"), std::string("ok"));
    CHECK(integration::payloadOf(pendingBook).getBool("requires_confirmation"));
    CHECK(integration::payloadOf(pendingBook).getInt("confirmation_id") > 0);
    CHECK(integration::payloadOf(pendingBook).getString("confirmation").empty());
    Json allow = Json::object();
    allow.set("tool", Json("book_table"));
    allow.set("mode", Json("allow"));
    CHECK_EQ(fixture.call(session, "permissions.set", allow).getString("type"), std::string("ok"));
    const Json booked = fixture.call(session, "tool.run", book);
    CHECK(integration::payloadOf(booked).getString("confirmation").rfind("SELFVOY-", 0) == 0);
    // deny блокирует и прямой вызов.
    allow.set("tool", Json("create_note"));
    allow.set("mode", Json("deny"));
    fixture.call(session, "permissions.set", allow);
    const Json deniedNote = fixture.call(session, "tool.run", note);
    CHECK_EQ(deniedNote.getString("code"), std::string("forbidden"));

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
    CHECK(selfvoy::totp::decodeSecretBase32(secretB32, rawSecret));
    Json confirm = Json::object();
    confirm.set("code", Json(selfvoy::totp::codeNow(rawSecret)));
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
    login.set("password", Json("selfvoy1234"));
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
    const std::int64_t nextWindow = (std::time(nullptr) / selfvoy::totp::kPeriod + 1) * selfvoy::totp::kPeriod;
    Json goodLogin = login;
    goodLogin.set("code", Json(selfvoy::totp::codeAt(rawSecret, nextWindow)));
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
    disableOk.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(recoverySession, "auth.disable2fa", disableOk).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(recoverySession, "auth.status2fa").get("payload").getBool("enabled"), false);

    // 15) После отключения обычный вход снова выдаёт сессию без кода.
    auto plainSession = fixture.makeSession("2fa-plain");
    Json plainLogin = Json::object();
    plainLogin.set("email", Json("vera@example.com"));
    plainLogin.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(plainSession, "auth.login", plainLogin).getString("type"), std::string("ok"));
    CHECK(plainSession->authenticated());
}

// ------------------------------------------- этап 8: разрешения и подтверждения
TEST(tool_danger_classification) {
    using selfvoy::ToolManager;
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
    auto db = selfvoy::makeEmbeddedDatabase("");  // in-memory (без файла)
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
    selfvoy::PendingActionRecord pending;
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
    selfvoy::PendingActionRecord pending;
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
    selfvoy::PendingActionRecord second;
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
    selfvoy::TaskRecord task;
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
    selfvoy::TaskRecord noRemind;
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

    // В контекст AI напоминание уходит занятым слотом с start/end (иначе AI отвечает 422
    // и у пользователя ломаются переговоры); запись расписания без времени — только в memory.
    fixture.server->memory().remember(session->userId(), [] {
        Json updates = Json::array();
        Json entry = Json::object();
        entry.set("kind", Json("schedule"));
        entry.set("text", Json("По средам бассейн"));
        updates.push(entry);
        return updates;
    }());
    Json offsetRun = run;
    Json offsetArgs = args;
    offsetArgs.set("text", Json("позвонить юристу"));
    offsetArgs.set("at", Json("2030-01-02T10:00:00+03:00"));
    offsetRun.set("args", offsetArgs);
    CHECK_EQ(fixture.call(session, "tool.run", offsetRun).getString("type"), std::string("ok"));
    const Json calendar = fixture.server->memory().context(session->userId(), "").get("calendar");
    CHECK_EQ(calendar.size(), static_cast<std::size_t>(2));
    bool water = false, lawyer = false;
    for (const auto& event : calendar.items()) {
        CHECK(!event.getString("start").empty());
        CHECK(!event.getString("end").empty());
        if (event.getString("title") == "Напоминание: выпить воды") {
            water = true;
            CHECK_EQ(event.getString("start"), std::string("2030-01-01T10:00:00Z"));
            CHECK_EQ(event.getString("end"), std::string("2030-01-01T10:30:00Z"));
        }
        if (event.getString("title") == "Напоминание: позвонить юристу") {
            lawyer = true;
            CHECK_EQ(event.getString("start"), std::string("2030-01-02T07:00:00Z"));
        }
    }
    CHECK(water);
    CHECK(lawyer);
}

// --------------------------------------------------- интеграции (этап 9)

TEST(integrations_pkce_s256_known_vector) {
    // RFC 7636, Приложение B: эталонная пара verifier → challenge.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string challenge = selfvoy::crypto::base64UrlEncode(selfvoy::crypto::sha256(verifier));
    CHECK_EQ(challenge, std::string("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
}

TEST(integrations_begin_and_state_rules) {
    ::setenv("SELFVOY_GOOGLE_CLIENT_ID", "test-client-id", 1);
    ::setenv("SELFVOY_GOOGLE_CLIENT_SECRET", "test-client-secret", 1);
    ::setenv("SELFVOY_GOOGLE_TOKEN_URL", "http://127.0.0.1:1/token", 1);  // недоступен
    Fixture fixture;
    auto anna = fixture.makeSession("anna");
    auto oleg = fixture.makeSession("oleg");
    fixture.registerUser(anna, "anna@example.com", "Анна");
    fixture.registerUser(oleg, "oleg@example.com", "Олег");

    Json beginPayload = Json::object();
    beginPayload.set("provider", Json("google_calendar"));
    beginPayload.set("redirect_uri", Json("http://127.0.0.1:9999/callback"));
    const Json begin = fixture.call(anna, "integrations.begin", beginPayload);
    CHECK_EQ(begin.getString("type"), std::string("ok"));
    const Json beginData = begin.get("payload");
    const std::string state = beginData.getString("state");
    CHECK(!state.empty());
    const std::string url = beginData.getString("authorize_url");
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.find("code_challenge=") != std::string::npos);
    CHECK(url.find("state=" + state) != std::string::npos);
    CHECK(url.find("calendar.events.readonly") != std::string::npos);

    // Неизвестный провайдер отклоняется.
    Json badProvider = beginPayload;
    badProvider.set("provider", Json("yandex_disk"));
    CHECK_EQ(fixture.call(anna, "integrations.begin", badProvider).getString("code"),
             std::string("bad_request"));

    Json callback = Json::object();
    callback.set("provider", Json("google_calendar"));
    callback.set("code", Json("any-code"));
    callback.set("state", Json(state));

    // Чужой state недействителен.
    CHECK_EQ(fixture.call(oleg, "integrations.callback", callback).getString("code"),
             std::string("bad_request"));

    // Несовпадение провайдера недействительно.
    Json wrongProvider = callback;
    wrongProvider.set("provider", Json("google_gmail"));
    CHECK_EQ(fixture.call(anna, "integrations.callback", wrongProvider).getString("code"),
             std::string("bad_request"));

    // Эндаунт токенов недоступен → upstream_error, state сгорает (одноразовый).
    CHECK_EQ(fixture.call(anna, "integrations.callback", callback).getString("code"),
             std::string("upstream_error"));
    CHECK_EQ(fixture.call(anna, "integrations.callback", callback).getString("code"),
             std::string("bad_request"));

    ::unsetenv("SELFVOY_GOOGLE_CLIENT_ID");
    ::unsetenv("SELFVOY_GOOGLE_CLIENT_SECRET");
    ::unsetenv("SELFVOY_GOOGLE_TOKEN_URL");
}

TEST(integrations_state_expires) {
    ::setenv("SELFVOY_GOOGLE_CLIENT_ID", "test-client-id", 1);
    ::setenv("SELFVOY_GOOGLE_CLIENT_SECRET", "test-client-secret", 1);
    ::setenv("SELFVOY_OAUTH_STATE_TTL", "-5", 1);  // мгновенно истёкший state
    Fixture fixture;
    auto anna = fixture.makeSession("anna");
    fixture.registerUser(anna, "anna@example.com", "Анна");

    Json beginPayload = Json::object();
    beginPayload.set("provider", Json("google_gmail"));
    beginPayload.set("redirect_uri", Json("http://127.0.0.1:9999/callback"));
    const Json begin = fixture.call(anna, "integrations.begin", beginPayload);
    CHECK_EQ(begin.getString("type"), std::string("ok"));

    Json callback = Json::object();
    callback.set("provider", Json("google_gmail"));
    callback.set("code", Json("any-code"));
    callback.set("state", Json(begin.get("payload").getString("state")));
    const Json expired = fixture.call(anna, "integrations.callback", callback);
    CHECK_EQ(expired.getString("code"), std::string("bad_request"));

    ::unsetenv("SELFVOY_GOOGLE_CLIENT_ID");
    ::unsetenv("SELFVOY_GOOGLE_CLIENT_SECRET");
    ::unsetenv("SELFVOY_OAUTH_STATE_TTL");
}

TEST(integrations_database_roundtrip) {
    Fixture fixture;
    auto& db = fixture.server->database().db();

    selfvoy::IntegrationConnectionRecord record;
    record.userId = 7;
    record.provider = "google_calendar";
    record.account = "anna@gmail.com";
    record.scope = "https://www.googleapis.com/auth/calendar.events.readonly";
    record.tokenEncrypted = "cipher-blob";
    record.status = "active";
    long long id = 0;
    CHECK(db.upsertIntegrationConnection(record, id).ok);
    CHECK(id > 0);

    const auto found = db.findIntegrationConnection(7, "google_calendar");
    CHECK(found.has_value());
    CHECK_EQ(found->account, std::string("anna@gmail.com"));
    CHECK_EQ(found->tokenEncrypted, std::string("cipher-blob"));

    // Upsert по (user, provider) обновляет существующую запись.
    record.tokenEncrypted = "cipher-blob-2";
    record.account = "anna2@gmail.com";
    long long sameId = 0;
    CHECK(db.upsertIntegrationConnection(record, sameId).ok);
    CHECK_EQ(sameId, id);
    const auto updated = db.findIntegrationConnection(7, "google_calendar");
    CHECK_EQ(updated->tokenEncrypted, std::string("cipher-blob-2"));
    CHECK_EQ(db.listIntegrationConnections(7).size(), static_cast<std::size_t>(1));

    // Наружу токен не отдаётся.
    const Json json = updated->toJson();
    CHECK(!json.contains("token_encrypted"));
    CHECK_EQ(json.getString("provider"), std::string("google_calendar"));
    CHECK_EQ(updated->toJson(true).getString("token_encrypted"), std::string("cipher-blob-2"));

    CHECK(db.updateIntegrationToken(id, "cipher-3", "expired", "токен истёк").ok);
    CHECK_EQ(db.findIntegrationConnection(7, "google_calendar")->status, std::string("expired"));
    CHECK(db.touchIntegrationUsed(id).ok);
    CHECK(!db.findIntegrationConnection(7, "google_calendar")->lastUsedAt.empty());
    CHECK(db.deleteIntegrationConnection(id).ok);
    CHECK(!db.findIntegrationConnection(7, "google_calendar").has_value());
    CHECK(!db.deleteIntegrationConnection(id).ok);  // уже удалено

    // Одноразовые OAuth-состояния.
    selfvoy::OauthStateRecord state;
    state.state = "st-1";
    state.userId = 7;
    state.provider = "google_gmail";
    state.verifier = "verifier-1";
    state.redirectUri = "http://127.0.0.1:9999/cb";
    state.expiresAt = "2030-01-01T00:00:00Z";
    CHECK(db.createOauthState(state).ok);
    const auto loaded = db.findOauthState("st-1");
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->verifier, std::string("verifier-1"));
    CHECK(loaded->usedAt.empty());
    CHECK(db.useOauthState("st-1").ok);
    CHECK(!db.findOauthState("st-1")->usedAt.empty());
    CHECK(!db.useOauthState("missing").ok);
    CHECK(!db.findOauthState("missing").has_value());
}

TEST(integrations_list_hides_secret) {
    Fixture fixture;
    auto anna = fixture.makeSession("anna");
    fixture.registerUser(anna, "anna@example.com", "Анна");

    selfvoy::IntegrationConnectionRecord record;
    record.userId = anna->userId();
    record.provider = "google_gmail";
    record.account = "anna@gmail.com";
    record.tokenEncrypted = "super-secret-cipher";
    long long id = 0;
    CHECK(fixture.server->database().db().upsertIntegrationConnection(record, id).ok);

    const Json listed = fixture.call(anna, "integrations.list");
    CHECK_EQ(listed.getString("type"), std::string("ok"));
    const std::string dump = listed.dump();
    CHECK(dump.find("super-secret-cipher") == std::string::npos);
    CHECK(dump.find("google_gmail") != std::string::npos);
    const Json connections = listed.get("payload").get("connections");
    CHECK_EQ(connections.size(), static_cast<std::size_t>(1));
    CHECK_EQ(connections.items()[0].getString("account"), std::string("anna@gmail.com"));
}

}  // namespace

// --- Этап 13: уведомления и push-устройства -------------------------------

TEST(server_notifications_flow) {
    Fixture fixture;
    auto session = fixture.makeSession("notify");
    fixture.registerUser(session, "nina@example.com", "Нина");

    // Вход внутри registerUser уже создал уведомление login.new.
    const Json listed = integration::payloadOf(fixture.call(session, "notifications.list"));
    CHECK(listed.getInt("unread") >= 1);
    const Json notifications = listed.get("notifications");
    bool sawLogin = false;
    long long loginId = 0;
    for (const auto& item : notifications.items()) {
        if (item.getString("kind") == std::string("login.new")) {
            sawLogin = true;
            loginId = item.getInt("id");
            CHECK_EQ(item.getString("title"), std::string("Новый вход в Selfvoy"));
            CHECK_EQ(item.getBool("read"), false);
        }
    }
    CHECK(sawLogin);

    // Пометить прочитанным: непрочитанных становится меньше, повтор — не ошибка.
    Json readOne = Json::object();
    readOne.set("id", Json(loginId));
    const Json afterRead = integration::payloadOf(fixture.call(session, "notifications.read", readOne));
    CHECK_EQ(afterRead.getInt("unread"), listed.getInt("unread") - 1);
    CHECK_EQ(fixture.call(session, "notifications.read", readOne).getString("type"), std::string("ok"));

    // Включение 2FA порождает уведомление безопасности.
    const Json setup = fixture.call(session, "auth.setup2fa");
    CHECK_EQ(setup.getString("type"), std::string("ok"));
    std::string rawSecret;
    CHECK(selfvoy::totp::decodeSecretBase32(setup.get("payload").getString("secret"), rawSecret));
    Json confirm = Json::object();
    confirm.set("code", Json(selfvoy::totp::codeNow(rawSecret)));
    CHECK_EQ(fixture.call(session, "auth.confirm2fa", confirm).getString("type"), std::string("ok"));

    // Фильтр непрочитанных видит twofactor.enabled.
    Json unreadReq = Json::object();
    unreadReq.set("unread", Json(true));
    const Json unreadList = integration::payloadOf(fixture.call(session, "notifications.list", unreadReq));
    // Именованная переменная: get() возвращает по значению, items() — ссылку
    // внутрь него (временный Json в range-for оставил бы ссылку висячей).
    const Json unreadItems = unreadList.get("notifications");
    bool saw2fa = false;
    for (const auto& item : unreadItems.items()) {
        if (item.getString("kind") == std::string("twofactor.enabled")) saw2fa = true;
    }
    CHECK(saw2fa);

    // «Прочитать все» (без id) очищает непрочитанные.
    CHECK_EQ(fixture.call(session, "notifications.read").getString("type"), std::string("ok"));
    const Json afterAll = integration::payloadOf(fixture.call(session, "notifications.list", unreadReq));
    CHECK_EQ(afterAll.getInt("unread"), 0);
    CHECK_EQ(afterAll.get("notifications").items().size(), std::size_t(0));

    // Изоляция: у другого пользователя только его собственные уведомления.
    auto other = fixture.makeSession("notify-other");
    fixture.registerUser(other, "oleg@example.com", "Олег");
    const Json otherList = integration::payloadOf(fixture.call(other, "notifications.list"));
    CHECK_EQ(otherList.get("notifications").items().size(), std::size_t(1));
    CHECK_EQ(otherList.get("notifications").at(0).getString("kind"), std::string("login.new"));
}

TEST(server_push_devices) {
    Fixture fixture;
    auto session = fixture.makeSession("push");
    fixture.registerUser(session, "push@example.com", "Пуш");

    // Валидная регистрация APNs-токена (64 hex-символа).
    Json reg = Json::object();
    reg.set("platform", Json("apns"));
    reg.set("token", Json(std::string(64, 'a')));
    const Json registered = integration::payloadOf(fixture.call(session, "devices.push.register", reg));
    CHECK(registered.getInt("id") > 0);
    const long long deviceId = registered.getInt("id");

    // Повторная регистрация того же токена — upsert, тот же id.
    const Json again = integration::payloadOf(fixture.call(session, "devices.push.register", reg));
    CHECK_EQ(again.getInt("id"), deviceId);

    // Список: устройство есть, токен наружу не отдаётся.
    const Json devices = integration::payloadOf(fixture.call(session, "devices.push.list"));
    CHECK_EQ(devices.get("devices").items().size(), std::size_t(1));
    CHECK(devices.get("devices").at(0).contains("token") == false);
    CHECK_EQ(devices.get("devices").at(0).getString("platform"), std::string("apns"));

    // Невалидные входные данные — bad_request.
    Json badPlatform = Json::object();
    badPlatform.set("platform", Json("sms"));
    badPlatform.set("token", Json(std::string(64, 'b')));
    CHECK_EQ(fixture.call(session, "devices.push.register", badPlatform).getString("code"),
             std::string("bad_request"));
    Json badToken = Json::object();
    badToken.set("platform", Json("apns"));
    badToken.set("token", Json(""));
    CHECK_EQ(fixture.call(session, "devices.push.register", badToken).getString("code"),
             std::string("bad_request"));
    // Токен попадает в путь запроса к шлюзу APNs — только hex.
    badToken.set("token", Json(std::string(64, 'a') + "/../x?"));
    CHECK_EQ(fixture.call(session, "devices.push.register", badToken).getString("code"),
             std::string("bad_request"));
    badToken.set("platform", Json("webhook"));
    badToken.set("token", Json("a b"));
    CHECK_EQ(fixture.call(session, "devices.push.register", badToken).getString("code"),
             std::string("bad_request"));
    // Не больше 20 устройств; уже известный токен регистрируется повторно.
    int accepted = 1;  // apns-устройство выше
    for (int i = 0; i < 25; ++i) {
        Json device = Json::object();
        device.set("platform", Json("webhook"));
        device.set("token", Json("cap-device-" + std::to_string(i)));
        if (fixture.call(session, "devices.push.register", device).getString("type") == "ok") ++accepted;
    }
    CHECK_EQ(accepted, 20);
    Json repeat = Json::object();
    repeat.set("platform", Json("webhook"));
    repeat.set("token", Json("cap-device-0"));
    CHECK_EQ(fixture.call(session, "devices.push.register", repeat).getString("type"), std::string("ok"));
    // Убираем добавленные устройства — дальше тест работает с одним apns.
    const Json all = integration::payloadOf(fixture.call(session, "devices.push.list")).get("devices");
    for (const auto& device : all.items()) {
        if (device.getInt("id") == deviceId) continue;
        Json drop = Json::object();
        drop.set("id", Json(device.getInt("id")));
        fixture.call(session, "devices.push.revoke", drop);
    }

    // Отзыв: чужое устройство — not_found, своё — revoked, повтор — not_found.
    auto other = fixture.makeSession("push-other");
    fixture.registerUser(other, "intruder@example.com", "Чужой");
    Json rev = Json::object();
    rev.set("id", Json(deviceId));
    CHECK_EQ(fixture.call(other, "devices.push.revoke", rev).getString("type"), std::string("error"));
    CHECK_EQ(fixture.call(other, "devices.push.revoke", rev).getString("code"), std::string("not_found"));
    const Json revoked = integration::payloadOf(fixture.call(session, "devices.push.revoke", rev));
    CHECK(revoked.getBool("revoked"));
    CHECK_EQ(fixture.call(session, "devices.push.revoke", rev).getString("code"), std::string("not_found"));
    CHECK_EQ(integration::payloadOf(fixture.call(session, "devices.push.list"))
                 .get("devices")
                 .items()
                 .size(),
             std::size_t(0));
}

TEST(server_onboarding_prefs) {
    // Онбординг-опрос после регистрации: новые поля настроек и флаг onboarded.
    Fixture fixture;
    auto session = fixture.makeSession("onboarding");
    fixture.registerUser(session, "survey@example.com", "Анкета");

    // У нового пользователя опрос не пройден.
    const Json defaults = integration::payloadOf(fixture.call(session, "prefs.get"));
    CHECK(defaults.getBool("onboarded") == false);
    CHECK_EQ(defaults.getString("birthday"), std::string(""));

    // Заполнение анкеты: день рождения, аллергии, диета, город.
    Json survey = Json::object();
    survey.set("birthday", Json("1995-04-18"));
    Json allergies = Json::array();
    allergies.push(Json("орехи"));
    allergies.push(Json("лактоза"));
    survey.set("allergies", allergies);
    Json diet = Json::array();
    diet.push(Json("vegetarian"));
    survey.set("diet", diet);
    survey.set("city", Json("Керкраде"));
    const Json saved = integration::payloadOf(fixture.call(session, "prefs.set", survey));
    CHECK_EQ(saved.getString("birthday"), std::string("1995-04-18"));
    CHECK_EQ(saved.get("allergies").size(), static_cast<std::size_t>(2));
    CHECK(saved.getBool("onboarded"));

    // Флаг устойчив: последующие правки настроек его не сбрасывают.
    Json theme = Json::object();
    theme.set("theme", Json("graphite-light"));
    const Json after = integration::payloadOf(fixture.call(session, "prefs.set", theme));
    CHECK(after.getBool("onboarded"));
    CHECK_EQ(after.getString("birthday"), std::string("1995-04-18"));

    // Анкета видна в контексте AI-сервиса (как остальные настройки).
    const Json context = fixture.server->memory().context(session->userId(), "спланируй ужин");
    CHECK_EQ(context.get("preferences").getString("birthday"), std::string("1995-04-18"));
}

TEST(server_push_devices_fcm) {
    // Этап 11 (Android): платформа 'fcm' принимается наравне с 'apns'.
    Fixture fixture;
    auto session = fixture.makeSession("push-fcm");
    fixture.registerUser(session, "android@example.com", "Андроид");

    Json reg = Json::object();
    reg.set("platform", Json("fcm"));
    reg.set("token", Json("fcm-device-token-01"));
    const Json registered = integration::payloadOf(fixture.call(session, "devices.push.register", reg));
    CHECK(registered.getInt("id") > 0);
    CHECK_EQ(registered.getString("platform"), std::string("fcm"));

    // Upsert по (user, token): повтор не плодит устройства.
    const Json again = integration::payloadOf(fixture.call(session, "devices.push.register", reg));
    CHECK_EQ(again.getInt("id"), registered.getInt("id"));

    const Json devices = integration::payloadOf(fixture.call(session, "devices.push.list"));
    CHECK_EQ(devices.get("devices").items().size(), std::size_t(1));
    CHECK_EQ(devices.get("devices").at(0).getString("platform"), std::string("fcm"));
    CHECK(devices.get("devices").at(0).contains("token") == false);
}

// --- Этап 14: сценарии безопасности -----------------------------------------

TEST(security_brute_force_throttle) {
    // Порог снижаем до 3, окно — 60 с (Fixture читает Config::fromEnv()).
    ::setenv("SELFVOY_MAX_LOGIN_ATTEMPTS", "3", 1);
    ::setenv("SELFVOY_MAX_IP_ATTEMPTS", "3", 1);
    ::setenv("SELFVOY_LOGIN_WINDOW", "60", 1);
    {
        Fixture fixture;
        auto session = fixture.makeSession("brute");
        fixture.registerUser(session, "brute@example.com", "Брут");

        Json login = Json::object();
        login.set("email", Json("brute@example.com"));
        login.set("password", Json("неверный-пароль"));
        for (int attempt = 0; attempt < 3; ++attempt) {
            const Json failed = fixture.call(session, "auth.login", login);
            CHECK_EQ(failed.getString("code"), std::string("unauthorized"));
        }

        // Превышен порог: даже верный пароль заблокирован до конца окна.
        login.set("password", Json("selfvoy1234"));
        const Json blocked = fixture.call(session, "auth.login", login);
        CHECK_EQ(blocked.getString("code"), std::string("forbidden"));

        // Регистрация под перебором тоже блокируется (тот же счётчик адреса).
        Json reg = Json::object();
        reg.set("email", Json("fresh@example.com"));
        reg.set("password", Json("selfvoy1234"));
        reg.set("display_name", Json("Фреш"));
        CHECK_EQ(fixture.call(session, "auth.register", reg).getString("code"),
                 std::string("forbidden"));
    }
    ::unsetenv("SELFVOY_MAX_LOGIN_ATTEMPTS");
    ::unsetenv("SELFVOY_MAX_IP_ATTEMPTS");
    ::unsetenv("SELFVOY_LOGIN_WINDOW");
}

TEST(security_injection_payloads_are_literal) {
    Fixture fixture;
    auto session = fixture.makeSession("inject");
    fixture.registerUser(session, "inject@example.com", "Инъекций");

    // SQL-инъекция в память: текст хранится и возвращается буквально.
    // kind — из CHECK схемы (fact|preference|schedule|contact).
    const std::string sqli = "'); DROP TABLE memories; -- <script>alert(1)</script>";
    Json add = Json::object();
    add.set("text", Json(sqli));
    add.set("kind", Json("fact"));
    CHECK_EQ(fixture.call(session, "memory.add", add).get("payload").getInt("saved"), 1);
    const Json entries =
        integration::payloadOf(fixture.call(session, "memory.list")).get("entries");
    bool foundLiteral = false;
    for (const auto& entry : entries.items()) {
        if (entry.getString("text") == sqli) foundLiteral = true;
    }
    CHECK(foundLiteral);

    // Задача с кавычками и точкой с запятой — round-trip без искажений.
    Json task = Json::object();
    task.set("title", Json("купить 'молоко'; удалить \"всё\""));
    const Json created = fixture.call(session, "tasks.create", task);
    CHECK_EQ(created.get("payload").getString("title"),
             std::string("купить 'молоко'; удалить \"всё\""));

    // Поиск пользователей с инъекцией: пустой результат, без падения.
    Json search = Json::object();
    search.set("query", Json("' OR '1'='1"));
    const Json found = fixture.call(session, "users.search", search);
    CHECK_EQ(found.getString("type"), std::string("ok"));
    CHECK_EQ(found.get("payload").get("users").items().size(), std::size_t(0));

    // Чужой email с инъекцией не входит (и не ломает хранилище).
    Json login = Json::object();
    login.set("email", Json("inject@example.com' OR '1'='1"));
    login.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(session, "auth.login", login).getString("type"), std::string("error"));
}

TEST(security_xss_message_roundtrip_is_literal) {
    Fixture fixture;
    auto anna = fixture.makeSession("xss-anna");
    auto bob = fixture.makeSession("xss-bob");
    fixture.registerUser(anna, "xanna@example.com", "Анна");
    fixture.registerUser(bob, "xbob@example.com", "Боб");

    Json open = Json::object();
    open.set("contact", Json("xbob@example.com"));
    const Json opened = fixture.call(anna, "chat.open", open);
    const long long chat = integration::payloadOf(opened).get("chat").getInt("id");
    CHECK(chat > 0);

    // XSS-payload доставляется как текст: сервер ничего не исполняет и не
    // экранирует — клиенты (QML Text, SwiftUI Text) HTML не рендерят.
    const std::string xss = "<script>alert('xss')</script><img src=x onerror=alert(2)>";
    Json send = Json::object();
    send.set("chat_id", Json(chat));
    send.set("body", Json(xss));
    CHECK_EQ(fixture.call(anna, "chat.send", send).getString("type"), std::string("ok"));

    Json history = Json::object();
    history.set("chat_id", Json(chat));
    const Json messages =
        integration::payloadOf(fixture.call(bob, "chat.history", history)).get("messages");
    bool sawLiteral = false;
    for (const auto& message : messages.items()) {
        if (message.getString("body") == xss) sawLiteral = true;
    }
    CHECK(sawLiteral);
}

TEST(security_cross_user_isolation) {
    Fixture fixture;
    auto anna = fixture.makeSession("iso-anna");
    auto bob = fixture.makeSession("iso-bob");
    fixture.registerUser(anna, "isoanna@example.com", "Анна");
    fixture.registerUser(bob, "isobob@example.com", "Боб");

    // Память Анны не видна Бобу.
    Json add = Json::object();
    add.set("text", Json("Секретный рецепт Анны"));
    add.set("kind", Json("preference"));
    CHECK_EQ(fixture.call(anna, "memory.add", add).get("payload").getInt("saved"), 1);
    const Json bobMemory =
        integration::payloadOf(fixture.call(bob, "memory.list")).get("entries");
    CHECK_EQ(bobMemory.items().size(), std::size_t(0));

    // Настройки Анны не видны Бобу (у него свои значения по умолчанию).
    Json prefs = Json::object();
    prefs.set("city", Json("Керкраде"));
    fixture.call(anna, "prefs.set", prefs);
    CHECK_EQ(integration::payloadOf(fixture.call(bob, "prefs.get")).getString("city"),
             std::string(""));

    // Задачи и уведомления тоже изолированы.
    Json task = Json::object();
    task.set("title", Json("Задача Анны"));
    fixture.call(anna, "tasks.create", task);
    CHECK_EQ(integration::payloadOf(fixture.call(bob, "tasks.list"))
                 .get("tasks")
                 .items()
                 .size(),
             std::size_t(0));
    CHECK_EQ(integration::payloadOf(fixture.call(bob, "notifications.list"))
                 .get("notifications")
                 .items()
                 .size(),
             std::size_t(1));  // только его собственный login.new

    // Чужой чат недоступен: у Анны есть чат с Бобом, список у обоих по одному.
    Json open = Json::object();
    open.set("contact", Json("isobob@example.com"));
    const Json annaChat = fixture.call(anna, "chat.open", open);
    CHECK_EQ(annaChat.getString("type"), std::string("ok"));
    const Json list = integration::payloadOf(fixture.call(anna, "chat.list"));
    CHECK_EQ(list.get("chats").items().size(), std::size_t(1));
    const Json bobList = integration::payloadOf(fixture.call(bob, "chat.list"));
    CHECK_EQ(bobList.get("chats").items().size(), std::size_t(1));
}

TEST(security_sessions_list_hides_secrets) {
    Fixture fixture;
    auto session = fixture.makeSession("leak");
    fixture.registerUser(session, "leak@example.com", "Лик");

    const Json sessions =
        integration::payloadOf(fixture.call(session, "sessions.list")).get("sessions");
    CHECK(sessions.items().size() >= 1);
    for (const auto& item : sessions.items()) {
        // Ни токена, ни его хэша, ни jwt_id наружу.
        CHECK(item.contains("token") == false);
        CHECK(item.contains("token_hash") == false);
        CHECK(item.contains("jwt_id") == false);
        CHECK(item.contains("refresh_token") == false);
        CHECK(!item.getString("id").empty());
    }
}

// ===================================================== аудит безопасности (C1–L24)
namespace audit {

// Кадр клиента с маской (RFC 6455 §5.3) — как шлёт браузер.
std::string maskedFrame(const std::string& payload, unsigned char opcode = 0x1) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(0x80 | size));
    } else if (size < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((static_cast<unsigned long long>(size) >> (i * 8)) & 0xFF));
    }
    const char mask[4] = {0x11, 0x22, 0x33, 0x44};
    frame.append(mask, 4);
    for (std::size_t i = 0; i < size; ++i) frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return frame;
}

Json loginPayload(const std::string& email, const std::string& password) {
    Json login = Json::object();
    login.set("email", Json(email));
    login.set("password", Json(password));
    return login;
}

}  // namespace audit

TEST(audit_c1_c2_config_validation) {
    std::vector<std::string> warnings;
    std::string error;

    // C1: драйвер почты по умолчанию — log; dev без SELFVOY_DEV_MODE не стартует.
    selfvoy::Config defaults;
    CHECK_EQ(defaults.mailDriver, std::string("log"));
    CHECK(defaults.jwtSecret.empty());
    CHECK(!defaults.devMode);

    selfvoy::Config prod;
    prod.jwtSecret = std::string(48, 'k') + "random";
    prod.twoFactorKey = "0123456789abcdef0123456789abcdef";
    CHECK(prod.validate(error, warnings));
    prod.mailDriver = "dev";
    error.clear();
    CHECK(!prod.validate(error, warnings));
    CHECK(error.find("SELFVOY_DEV_MODE") != std::string::npos);

    // Сервер с такой конфигурацией не стартует.
    selfvoy::Server refused(prod);
    error.clear();
    CHECK(!refused.start(error));
    CHECK(error.find("SELFVOY_DEV_MODE") != std::string::npos);

    // C2: слабые/пустые/шаблонные секреты и отсутствие ключа 2FA вне dev — отказ.
    for (const std::string secret : {std::string(""), std::string("selfvoy-dev-secret"), std::string("short-secret"),
                                     std::string("CHANGE-IN-PRODUCTION-0123456789abcdefghijklmnop")}) {
        selfvoy::Config weak;
        weak.jwtSecret = secret;
        weak.twoFactorKey = "0123456789abcdef0123456789abcdef";
        error.clear();
        CHECK(!weak.validate(error, warnings));
        CHECK(error.find("SELFVOY_JWT_SECRET") != std::string::npos);
    }
    selfvoy::Config noTwoFactor;
    noTwoFactor.jwtSecret = std::string(64, 'x');
    error.clear();
    CHECK(!noTwoFactor.validate(error, warnings));
    CHECK(error.find("SELFVOY_2FA_KEY") != std::string::npos);

    // В dev-режиме без секрета — случайный секрет на процесс (с предупреждением).
    selfvoy::Config dev;
    dev.devMode = true;
    dev.mailDriver = "dev";
    warnings.clear();
    error.clear();
    CHECK(dev.validate(error, warnings));
    CHECK_EQ(dev.jwtSecret.size(), static_cast<std::size_t>(64));
    CHECK(!warnings.empty());
    selfvoy::Config dev2;
    dev2.devMode = true;
    CHECK(dev2.validate(error, warnings));
    CHECK(dev.jwtSecret != dev2.jwtSecret);

    // Код сброса живёт не дольше часа.
    selfvoy::Config longReset = prod;
    longReset.mailDriver = "log";
    longReset.resetTtlSeconds = 86400;
    CHECK(longReset.validate(error, warnings));
    CHECK_EQ(longReset.resetTtlSeconds, 3600LL);
}

// Переименование Aura -> Selfvoy: старые AURA_* читаются один релиз, если SELFVOY_* не задана.
TEST(config_legacy_aura_env_fallback) {
    ::unsetenv("SELFVOY_PORT");
    ::unsetenv("SELFVOY_LOG_LEVEL");
    ::setenv("AURA_PORT", "9123", 1);
    ::setenv("AURA_LOG_LEVEL", "debug", 1);
    selfvoy::Config legacy = selfvoy::Config::fromEnv();
    CHECK_EQ(legacy.port, 9123);
    CHECK_EQ(legacy.logLevel, std::string("debug"));
    CHECK_EQ(legacy.legacyEnvNames.size(), static_cast<std::size_t>(2));
    CHECK(std::find(legacy.legacyEnvNames.begin(), legacy.legacyEnvNames.end(), "AURA_PORT") !=
          legacy.legacyEnvNames.end());

    // Одно предупреждение со списком старых имён.
    legacy.jwtSecret = std::string(48, 'k') + "random";
    legacy.twoFactorKey = "0123456789abcdef0123456789abcdef";
    std::vector<std::string> warnings;
    std::string error;
    CHECK(legacy.validate(error, warnings));
    std::size_t deprecations = 0;
    for (const auto& warning : warnings) {
        if (warning.find("AURA_PORT") != std::string::npos) {
            ++deprecations;
            CHECK(warning.find("AURA_LOG_LEVEL") != std::string::npos);
        }
    }
    CHECK_EQ(deprecations, static_cast<std::size_t>(1));

    // Если заданы обе — побеждает SELFVOY_*, старое имя не считается использованным.
    ::setenv("SELFVOY_PORT", "9456", 1);
    selfvoy::Config both = selfvoy::Config::fromEnv();
    CHECK_EQ(both.port, 9456);
    CHECK(std::find(both.legacyEnvNames.begin(), both.legacyEnvNames.end(), "AURA_PORT") ==
          both.legacyEnvNames.end());
    CHECK_EQ(selfvoy::envOr("SELFVOY_LOG_LEVEL", "info"), std::string("debug"));

    ::unsetenv("SELFVOY_PORT");
    ::unsetenv("AURA_PORT");
    ::unsetenv("AURA_LOG_LEVEL");
    selfvoy::Config clean = selfvoy::Config::fromEnv();
    CHECK_EQ(clean.port, 9000);
    CHECK(clean.legacyEnvNames.empty());
}

TEST(audit_c1_log_driver_hides_codes) {
    selfvoy::Config config;
    config.mailDriver = "log";
    CHECK(!selfvoy::makeMailProvider(config)->exposesCodes());
    // dev без devMode тоже не раскрывает коды (вторая линия защиты).
    config.mailDriver = "dev";
    CHECK(!selfvoy::makeMailProvider(config)->exposesCodes());
    config.devMode = true;
    CHECK(selfvoy::makeMailProvider(config)->exposesCodes());
}

TEST(audit_c2_forged_sub_rejected) {
    Fixture fixture;
    auto victim = fixture.makeSession("victim");
    fixture.registerUser(victim, "victim@example.com", "Жертва");
    auto mallory = fixture.makeSession("mallory");
    const Json logged = fixture.registerUser(mallory, "mallory@example.com", "Мэллори");
    const std::string token = integration::payloadOf(logged).getString("token");

    // Токен с собственным живым jti, но sub жертвы (подписан тем же секретом).
    const std::string payloadPart = token.substr(token.find('.') + 1, token.rfind('.') - token.find('.') - 1);
    std::string claimsRaw;
    std::string padded = payloadPart;
    for (auto& ch : padded) {
        if (ch == '-') ch = '+';
        if (ch == '_') ch = '/';
    }
    while (padded.size() % 4) padded.push_back('=');
    CHECK(selfvoy::crypto::base64Decode(padded, claimsRaw));
    Json claims = Json::parse(claimsRaw);
    claims.set("sub", Json(victim->userId()));
    claims.set("email", Json("victim@example.com"));
    claims.erase("exp");
    claims.erase("iat");
    const std::string forged = selfvoy::jwt::sign(claims, fixture.config.jwtSecret, 3600);

    auto attacker = fixture.makeSession("attacker");
    Json auth = Json::object();
    auth.set("token", Json(forged));
    CHECK_EQ(fixture.call(attacker, "auth.token", auth).getString("code"), std::string("unauthorized"));
    CHECK(!attacker->authenticated());
    // Неподделанный токен по-прежнему работает.
    auth.set("token", Json(token));
    CHECK_EQ(fixture.call(attacker, "auth.token", auth).getString("type"), std::string("ok"));
    CHECK_EQ(attacker->userId(), mallory->userId());
}

TEST(audit_h3_json_many_keys_and_preauth_frame) {
    // 90 000 ключей в одном объекте: разбор отклоняется быстро (лимит ключей).
    std::string text = "{";
    for (int i = 0; i < 90000; ++i) {
        if (i) text += ",";
        text += "\"" + std::to_string(100000 + i) + "\":0";
    }
    text += "}";
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    CHECK(Json::parse(text, &error).isNull());
    CHECK(error.find("ключей") != std::string::npos);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    CHECK(elapsed < 1000);

    // Дубли ключей: последнее значение побеждает, порядок — по первому вхождению.
    const Json dup = Json::parse(R"({"a":1,"b":2,"a":3})");
    CHECK_EQ(dup.getInt("a"), 3);
    CHECK_EQ(dup.keys().size(), static_cast<std::size_t>(2));
    CHECK_EQ(dup.keys().front(), std::string("a"));

    // Ровно на лимите — разбирается.
    std::string ok = "{";
    for (std::size_t i = 0; i < Json::kMaxObjectKeys; ++i) {
        if (i) ok += ",";
        ok += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
    }
    ok += "}";
    CHECK_EQ(Json::parse(ok).keys().size(), Json::kMaxObjectKeys);

    // До входа кадр больше SELFVOY_MAX_PREAUTH_FRAME закрывает соединение.
    selfvoy::Config config;
    config.maxPreAuthFrameBytes = 1024;
    config.authTimeoutSec = 0;
    int fds[2] = {-1, -1};
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto session = std::make_shared<selfvoy::Session>(fds[1], "big", "127.0.0.1:1", config);
    bool handled = false;
    std::thread reader([&] { session->readLoop([&](std::shared_ptr<selfvoy::Session>, const Json&) { handled = true; }); });
    const std::string frame = audit::maskedFrame(R"({"type":"ping","payload":{"x":")" + std::string(4000, 'a') + "\"}}");
    CHECK(::send(fds[0], frame.data(), frame.size(), 0) > 0);
    reader.join();
    CHECK(!handled);
    CHECK(!session->alive());
    ::close(fds[0]);
}

TEST(audit_h4_argon2_gate_and_register_limits) {
    // Параллельные Argon2id ограничены числом ядер.
    selfvoy::crypto::resetArgon2Peak();
    const int limit = selfvoy::crypto::argon2Stats().limit;
    CHECK(limit >= 1);
    std::vector<std::thread> workers;
    for (int i = 0; i < limit + 3; ++i) {
        workers.emplace_back([] { selfvoy::crypto::hashPassword("pass1234"); });
    }
    for (auto& worker : workers) worker.join();
    CHECK(selfvoy::crypto::argon2Stats().peak <= limit);
    CHECK_EQ(selfvoy::crypto::argon2Stats().inFlight, 0);

    ::setenv("SELFVOY_REGISTER_PER_IP_HOUR", "2", 1);
    {
        Fixture fixture;
        auto session = fixture.makeSession("reg");
        Json reg = Json::object();
        reg.set("password", Json("selfvoy1234"));
        reg.set("email", Json("r1@example.com"));
        CHECK_EQ(fixture.call(session, "auth.register", reg).getString("type"), std::string("ok"));
        // Дубликат отсекается до хэширования и не расходует лимит регистраций.
        CHECK_EQ(fixture.call(session, "auth.register", reg).getString("code"), std::string("conflict"));
        reg.set("email", Json("r2@example.com"));
        CHECK_EQ(fixture.call(session, "auth.register", reg).getString("type"), std::string("ok"));
        // Третья регистрация с того же IP (другой порт) за час — отказ.
        auto other = std::make_shared<selfvoy::Session>(-1, "reg2", "127.0.0.1:5555", fixture.config);
        reg.set("email", Json("r3@example.com"));
        const Json third = fixture.call(other, "auth.register", reg);
        CHECK_EQ(third.getString("code"), std::string("forbidden"));
    }
    ::unsetenv("SELFVOY_REGISTER_PER_IP_HOUR");
}

TEST(audit_h5_verify_code_attempts) {
    Fixture fixture;
    auto session = fixture.makeSession("verify");
    Json reg = Json::object();
    reg.set("email", Json("squat@example.com"));
    reg.set("password", Json("selfvoy1234"));
    const std::string code = integration::payloadOf(fixture.call(session, "auth.register", reg)).getString("email_code");
    CHECK_EQ(code.size(), static_cast<std::size_t>(6));

    Json verify = Json::object();
    verify.set("email", Json("squat@example.com"));
    for (int i = 0; i < 5; ++i) {
        verify.set("code", Json(code == "111111" ? std::string("222222") : std::string("111111")));
        CHECK_EQ(fixture.call(session, "auth.verifyEmail", verify).getString("code"), std::string("bad_request"));
    }
    // После 5 неверных попыток настоящий код сгорел.
    verify.set("code", Json(code));
    CHECK_EQ(fixture.call(session, "auth.verifyEmail", verify).getString("code"), std::string("bad_request"));
    // Новый код (resendCode) работает.
    Json resend = Json::object();
    resend.set("email", Json("squat@example.com"));
    const std::string fresh = integration::payloadOf(fixture.call(session, "auth.resendCode", resend)).getString("email_code");
    CHECK_EQ(fresh.size(), static_cast<std::size_t>(6));
    verify.set("code", Json(fresh));
    CHECK(fixture.call(session, "auth.verifyEmail", verify).get("payload").getBool("verified"));
}

TEST(audit_h6_totp_bruteforce_and_ip_key) {
    CHECK_EQ(selfvoy::Session::ipOf("127.0.0.1:54321"), std::string("127.0.0.1"));
    // IPv6 — ключ лимитов по сети /64 (см. session_limit_key_ipv6_prefix).
    CHECK_EQ(selfvoy::Session::ipOf("2001:db8::1:443"), std::string("2001:db8:0:0::/64"));
    CHECK_EQ(selfvoy::Session::ipOf("10.0.0.1"), std::string("10.0.0.1"));

    Fixture fixture;
    auto owner = fixture.makeSession("erin");
    fixture.registerUser(owner, "erin@example.com", "Эрин");
    const std::string secret = integration::payloadOf(fixture.call(owner, "auth.setup2fa")).getString("secret");
    std::string raw;
    CHECK(selfvoy::totp::decodeSecretBase32(secret, raw));
    Json confirm = Json::object();
    confirm.set("code", Json(selfvoy::totp::codeNow(raw)));
    CHECK_EQ(fixture.call(owner, "auth.confirm2fa", confirm).getString("type"), std::string("ok"));

    // H7: повторный setup2fa при включённой 2FA — отказ, 2FA остаётся включённой.
    CHECK_EQ(fixture.call(owner, "auth.setup2fa").getString("code"), std::string("bad_request"));
    CHECK(fixture.call(owner, "auth.status2fa").get("payload").getBool("enabled"));

    // Перебор TOTP с переподключениями и «верным паролем» между попытками.
    const std::string valid = selfvoy::totp::codeNow(raw);
    Json attempt = audit::loginPayload("erin@example.com", "selfvoy1234");
    int wrong = 0;
    for (int i = 0; i < 5; ++i) {
        auto attacker = std::make_shared<selfvoy::Session>(-1, "atk", "127.0.0.1:" + std::to_string(40000 + i), fixture.config);
        CHECK_EQ(fixture.call(attacker, "auth.login", attempt).getString("code"), std::string("requires_2fa"));
        Json guess = attempt;
        guess.set("code", Json(valid == "000000" ? std::string("000001") : std::string("000000")));
        if (fixture.call(attacker, "auth.login2fa", guess).getString("code") == "unauthorized") ++wrong;
    }
    CHECK_EQ(wrong, 5);
    // Лимит исчерпан: даже верный код отклоняется до конца окна.
    auto late = std::make_shared<selfvoy::Session>(-1, "late", "127.0.0.1:49999", fixture.config);
    const std::int64_t next = (std::time(nullptr) / selfvoy::totp::kPeriod + 1) * selfvoy::totp::kPeriod;
    Json good = attempt;
    good.set("code", Json(selfvoy::totp::codeAt(raw, next)));
    CHECK_EQ(fixture.call(late, "auth.login2fa", good).getString("code"), std::string("forbidden"));
}

TEST(audit_h8_open_connections_follow_revocation) {
    Fixture fixture;
    auto first = fixture.makeSession("first");
    fixture.registerUser(first, "bob@example.com", "Боб");
    // Второе соединение не через makeSession (иначе закроется сокет первого).
    auto second = std::make_shared<selfvoy::Session>(-1, "second", "127.0.0.1:2", fixture.config);
    CHECK_EQ(fixture.call(second, "auth.login", audit::loginPayload("bob@example.com", "selfvoy1234")).getString("type"),
             std::string("ok"));
    CHECK_EQ(fixture.call(first, "auth.me").getString("type"), std::string("ok"));

    // sessions.revokeAll со второго: первое соединение выбито сразу.
    CHECK_EQ(fixture.call(second, "sessions.revokeAll").getString("type"), std::string("ok"));
    CHECK(!first->authenticated());
    CHECK_EQ(fixture.call(first, "auth.me").getString("code"), std::string("unauthorized"));
    Json task = Json::object();
    task.set("title", Json("still here"));
    CHECK_EQ(fixture.call(first, "tasks.create", task).getString("code"), std::string("unauthorized"));
    CHECK(second->authenticated());

    // Отзыв в обход сервера (другой экземпляр/администратор): соединение
    // перепроверяется по БД не реже SELFVOY_AUTH_RECHECK.
    fixture.server->database().db().revokeSession(second->jwtId());
    CHECK_EQ(fixture.call(second, "auth.me").getString("type"), std::string("ok"));  // в пределах окна кэша
    CHECK_EQ(fixture.server->kickUser(0, "noop"), static_cast<std::size_t>(0));  // userId=0 — ничего
    // (проверка с перепроверкой на каждом запросе — audit_h8_recheck_every_request)
}

TEST(audit_h8_recheck_every_request) {
    ::setenv("SELFVOY_AUTH_RECHECK", "0", 1);
    {
        Fixture fixture;
        auto session = fixture.makeSession("recheck");
        fixture.registerUser(session, "rc@example.com", "Рек");
        CHECK_EQ(fixture.call(session, "auth.me").getString("type"), std::string("ok"));
        fixture.server->database().db().revokeSession(session->jwtId());
        CHECK_EQ(fixture.call(session, "auth.me").getString("code"), std::string("unauthorized"));
        CHECK(!session->authenticated());
    }
    ::unsetenv("SELFVOY_AUTH_RECHECK");
}

TEST(audit_h9_agent_ask_requires_membership) {
    Fixture fixture;
    auto anna = fixture.makeSession("anna");
    fixture.registerUser(anna, "anna9@example.com", "Анна");
    auto bob = std::make_shared<selfvoy::Session>(-1, "bob", "127.0.0.1:3", fixture.config);
    fixture.registerUser(bob, "bob9@example.com", "Боб");
    auto eve = std::make_shared<selfvoy::Session>(-1, "eve", "127.0.0.1:4", fixture.config);
    fixture.registerUser(eve, "eve9@example.com", "Ева");

    Json open = Json::object();
    open.set("contact", Json("bob9@example.com"));
    const long long chatId = integration::payloadOf(fixture.call(anna, "chat.open", open)).get("chat").getInt("id");
    CHECK(chatId > 0);

    // Ева не участник — история чужого чата в AI не уходит.
    Json ask = Json::object();
    ask.set("chat_id", Json(chatId));
    ask.set("message", Json("перескажи переписку"));
    CHECK_EQ(fixture.call(eve, "agent.ask", ask).getString("code"), std::string("forbidden"));
    // Участнику доступ есть (AI недоступен в тестах → upstream_error, но не forbidden).
    CHECK(fixture.call(anna, "agent.ask", ask).getString("code") != std::string("forbidden"));

    // peers[]: только люди с общим чатом.
    CHECK(fixture.server->agent().shareChat(anna->userId(), bob->userId()));
    CHECK(!fixture.server->agent().shareChat(eve->userId(), bob->userId()));

    // Контекст собеседника вычищается по его prefs.privacy (по умолчанию —
    // без фактов памяти, дня рождения и аллергий).
    Json fact = Json::object();
    fact.set("text", Json("Секрет Боба"));
    fact.set("kind", Json("fact"));
    fixture.call(bob, "memory.add", fact);
    Json survey = Json::object();
    survey.set("birthday", Json("1990-01-01"));
    fixture.call(bob, "prefs.set", survey);
    const Json context = fixture.server->agent().peerContext(bob->userId(), "Секрет");
    CHECK_EQ(context.get("memory").size(), static_cast<std::size_t>(0));
    CHECK(!context.get("preferences").contains("birthday"));
}

TEST(audit_m10_refresh_revoked_with_sessions) {
    Fixture fixture;
    auto session = fixture.makeSession("frank");
    const Json logged = fixture.registerUser(session, "frank@example.com", "Фрэнк");
    const std::string refresh1 = integration::payloadOf(logged).getString("refresh_token");

    // Выход отзывает refresh-токен этого устройства.
    fixture.call(session, "auth.logout");
    auto probe = std::make_shared<selfvoy::Session>(-1, "probe", "127.0.0.1:7", fixture.config);
    Json refresh = Json::object();
    refresh.set("refresh_token", Json(refresh1));
    CHECK_EQ(fixture.call(probe, "auth.refresh", refresh).getString("code"), std::string("unauthorized"));

    // revokeAll с устройства B: refresh устройства A мёртв, своего — жив.
    auto deviceA = std::make_shared<selfvoy::Session>(-1, "A", "127.0.0.1:8", fixture.config);
    auto deviceB = std::make_shared<selfvoy::Session>(-1, "B", "127.0.0.1:9", fixture.config);
    const std::string refreshA = integration::payloadOf(
        fixture.call(deviceA, "auth.login", audit::loginPayload("frank@example.com", "selfvoy1234"))).getString("refresh_token");
    const std::string refreshB = integration::payloadOf(
        fixture.call(deviceB, "auth.login", audit::loginPayload("frank@example.com", "selfvoy1234"))).getString("refresh_token");
    CHECK_EQ(fixture.call(deviceB, "sessions.revokeAll").getString("type"), std::string("ok"));
    auto probe2 = std::make_shared<selfvoy::Session>(-1, "probe2", "127.0.0.1:10", fixture.config);
    refresh.set("refresh_token", Json(refreshA));
    CHECK_EQ(fixture.call(probe2, "auth.refresh", refresh).getString("code"), std::string("unauthorized"));
    refresh.set("refresh_token", Json(refreshB));
    const Json rotated = fixture.call(deviceB, "auth.refresh", refresh);
    CHECK_EQ(rotated.getString("type"), std::string("ok"));

    // sessions.revoke отзывает и refresh-семейство этой сессии.
    auto deviceC = std::make_shared<selfvoy::Session>(-1, "C", "127.0.0.1:11", fixture.config);
    const Json loginC = fixture.call(deviceC, "auth.login", audit::loginPayload("frank@example.com", "selfvoy1234"));
    const std::string refreshC = integration::payloadOf(loginC).getString("refresh_token");
    Json revoke = Json::object();
    revoke.set("session_id", Json(integration::payloadOf(loginC).getString("session_id")));
    CHECK(fixture.call(deviceB, "sessions.revoke", revoke).get("payload").getBool("revoked"));
    CHECK(!deviceC->authenticated());  // открытое соединение отозванной сессии закрыто
    refresh.set("refresh_token", Json(refreshC));
    CHECK_EQ(fixture.call(probe2, "auth.refresh", refresh).getString("code"), std::string("unauthorized"));
}

TEST(audit_m12_send_email_header_injection) {
    Fixture fixture;
    auto& tools = fixture.server->tools();
    Json args = Json::object();
    args.set("to", Json("friend@example.com\r\nBcc: evil@example.com"));
    args.set("body", Json("hi"));
    CHECK(!tools.sendEmail(1, args).ok);
    args.set("to", Json("friend@example.com, evil@example.com"));
    CHECK(!tools.sendEmail(1, args).ok);
    args.set("to", Json("friend@example.com"));
    args.set("subject", Json("тема\r\nBcc: evil@example.com"));
    CHECK(!tools.sendEmail(1, args).ok);
    args.set("subject", Json("Обычная тема"));
    CHECK(tools.sendEmail(1, args).ok);
}

TEST(audit_m13_mail_per_target_limit) {
    Fixture fixture;
    auto session = fixture.makeSession("mail");
    fixture.registerUser(session, "target@example.com", "Цель");
    Json forgot = Json::object();
    forgot.set("email", Json("target@example.com"));
    int issued = 0;
    for (int i = 0; i < 5; ++i) {
        // Каждый раз новое соединение (другой порт): лимит на адрес получателя.
        auto client = std::make_shared<selfvoy::Session>(-1, "m", "127.0.0.1:" + std::to_string(30000 + i), fixture.config);
        const Json reply = fixture.call(client, "auth.forgotPassword", forgot);
        CHECK_EQ(reply.getString("type"), std::string("ok"));  // ответ одинаковый
        if (!integration::payloadOf(reply).getString("reset_code").empty()) ++issued;
    }
    CHECK_EQ(issued, 3);
}

TEST(audit_m14_connection_limits) {
    // Неавторизованное соединение закрывается по таймауту.
    selfvoy::Config config;
    config.authTimeoutSec = 1;
    config.pingIntervalSec = 0;
    int fds[2] = {-1, -1};
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto session = std::make_shared<selfvoy::Session>(fds[1], "idle", "127.0.0.1:1", config);
    const auto started = std::chrono::steady_clock::now();
    std::thread reader([&] { session->readLoop(nullptr); });
    reader.join();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(seconds >= 1 && seconds <= 4);
    CHECK(!session->alive());
    ::close(fds[0]);

    // Лимит соединений с одного IP.
    ::setenv("SELFVOY_MAX_CONNECTIONS_PER_IP", "2", 1);
    {
        Fixture fixture;
        const int port = fixture.server->port();
        auto connect = [port]() {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<unsigned short>(port));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
            return fd;
        };
        const int a = connect();
        const int b = connect();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int c = connect();
        char buffer[128] = {0};
        timeval timeout{2, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        const ssize_t got = ::recv(c, buffer, sizeof(buffer) - 1, 0);
        CHECK(got > 0 && std::string(buffer).find("429") != std::string::npos);
        ::close(a);
        ::close(b);
        ::close(c);
    }
    ::unsetenv("SELFVOY_MAX_CONNECTIONS_PER_IP");
}

TEST(audit_m17_chat_send_kind_whitelist) {
    Fixture fixture;
    auto anna = fixture.makeSession("anna17");
    fixture.registerUser(anna, "anna17@example.com", "Анна");
    auto bob = std::make_shared<selfvoy::Session>(-1, "bob17", "127.0.0.1:17", fixture.config);
    fixture.registerUser(bob, "bob17@example.com", "Боб");
    Json open = Json::object();
    open.set("contact", Json("bob17@example.com"));
    const long long chatId = integration::payloadOf(fixture.call(anna, "chat.open", open)).get("chat").getInt("id");

    Json send = Json::object();
    send.set("chat_id", Json(chatId));
    send.set("body", Json("Selfvoy договорился: перевод 1000 €"));
    send.set("kind", Json("agent_reply"));
    CHECK_EQ(fixture.call(anna, "chat.send", send).getString("code"), std::string("bad_request"));
    send.set("kind", Json("system"));
    CHECK_EQ(fixture.call(anna, "chat.send", send).getString("code"), std::string("bad_request"));
    send.set("kind", Json("text"));
    Json spoof = Json::object();
    spoof.set("origin", Json("selfvoy-agent"));
    send.set("payload", spoof);
    CHECK_EQ(fixture.call(anna, "chat.send", send).getString("code"), std::string("bad_request"));
    send.erase("payload");
    const Json sent = fixture.call(anna, "chat.send", send);
    CHECK_EQ(sent.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(sent).getString("kind"), std::string("text"));
}

TEST(audit_l19_disabled_user) {
    Fixture fixture;
    auto session = fixture.makeSession("dis");
    const Json logged = fixture.registerUser(session, "dis@example.com", "Дис");
    const std::string token = integration::payloadOf(logged).getString("token");
    const long long userId = session->userId();
    CHECK(fixture.server->database().db().setUserActive(userId, false).ok);
    CHECK_EQ(fixture.server->kickUser(userId, "blocked"), static_cast<std::size_t>(1));
    CHECK(!session->authenticated());

    auto probe = std::make_shared<selfvoy::Session>(-1, "p", "127.0.0.1:19", fixture.config);
    Json auth = Json::object();
    auth.set("token", Json(token));
    CHECK_EQ(fixture.call(probe, "auth.token", auth).getString("code"), std::string("unauthorized"));
    // Неверный пароль не раскрывает блокировку, верный — «аккаунт отключён».
    const Json wrong = fixture.call(probe, "auth.login", audit::loginPayload("dis@example.com", "wrongpass1"));
    CHECK_EQ(wrong.getString("code"), std::string("unauthorized"));
    CHECK(wrong.getString("message").find("отключ") == std::string::npos);
    const Json right = fixture.call(probe, "auth.login", audit::loginPayload("dis@example.com", "selfvoy1234"));
    CHECK_EQ(right.getString("code"), std::string("forbidden"));
}

TEST(audit_l21_races_are_atomic) {
    Fixture fixture;
    auto session = fixture.makeSession("race");
    const Json logged = fixture.registerUser(session, "race@example.com", "Гонка");
    auto& db = fixture.server->database().db();
    const long long userId = session->userId();

    // Двойное подтверждение: исполняет только первый claim.
    selfvoy::PendingActionRecord pending;
    pending.userId = userId;
    pending.tool = "create_note";
    pending.args.set("text", Json("заметка"));
    const long long id = db.createPendingAction(pending);
    CHECK(db.claimPendingAction(id, userId, "approved"));
    CHECK(!db.claimPendingAction(id, userId, "approved"));
    const long long other = db.createPendingAction(pending);
    CHECK(!db.claimPendingAction(other, userId + 1000, "approved"));  // чужое

    // Одновременный refresh: второй отзыв того же токена не проходит.
    const std::string refresh = integration::payloadOf(logged).getString("refresh_token");
    const auto record = db.findRefreshToken(selfvoy::crypto::toHex(selfvoy::crypto::sha256(refresh)));
    CHECK(record.has_value());
    CHECK(db.revokeRefreshToken(record->id, "").ok);
    CHECK(!db.revokeRefreshToken(record->id, "").ok);

    // tasks.due обрабатывает только свои напоминания.
    auto other_user = std::make_shared<selfvoy::Session>(-1, "o", "127.0.0.1:21", fixture.config);
    fixture.registerUser(other_user, "race2@example.com", "Гонка2");
    Json task = Json::object();
    task.set("title", Json("чужое"));
    task.set("remind_at", Json("2020-01-01T00:00:00.000Z"));
    fixture.call(other_user, "tasks.create", task);
    CHECK_EQ(fixture.call(session, "tasks.due").get("payload").getInt("sent"), 0);
    CHECK_EQ(fixture.call(other_user, "tasks.due").get("payload").getInt("sent"), 1);
    CHECK_EQ(fixture.call(other_user, "tasks.due").get("payload").getInt("sent"), 0);
}

TEST(audit_l22_internal_errors_are_generic) {
    Fixture fixture;
    fixture.server->registerHandler("test.throw", [](std::shared_ptr<selfvoy::Session>, const selfvoy::protocol::Request&) -> Json {
        throw std::runtime_error("ERROR: relation \"users\" violates constraint users_email_key");
    });
    auto session = fixture.makeSession("throw");
    fixture.registerUser(session, "throw@example.com", "Бросок");
    const Json reply = fixture.call(session, "test.throw");
    CHECK_EQ(reply.getString("code"), std::string("internal_error"));
    CHECK(reply.getString("message").find("relation") == std::string::npos);
}

TEST(embedded_backfills_booking_price_snapshot) {
    // Файл встроенной БД до снимка цены: у записей нет price/currency.
    const std::string path = "/tmp/selfvoy-price-db-" + std::to_string(::getpid()) + ".json";
    {
        FILE* file = std::fopen(path.c_str(), "w");
        std::fputs("{\"sequences\": {},"
                   " \"booking_pages\": [{\"id\": 7, \"owner_id\": 1, \"price\": 250000, \"currency\": \"EUR\"}],"
                   " \"bookings\": [{\"id\": 1, \"page_id\": 7, \"owner_id\": 1},"
                   "                {\"id\": 2, \"page_id\": 7, \"owner_id\": 1, \"package_id\": 3},"
                   "                {\"id\": 3, \"page_id\": 7, \"owner_id\": 1, \"price\": 100, \"currency\": \"RUB\"}]}",
                   file);
        std::fclose(file);
    }
    auto db = selfvoy::makeEmbeddedDatabase(path);
    std::string error;
    CHECK(db->connect(error));
    const auto plain = db->findBooking(1);
    CHECK(plain.has_value());
    if (plain) {
        CHECK_EQ(plain->price, 250000LL);
        CHECK_EQ(plain->currency, std::string("EUR"));
    }
    const auto byPackage = db->findBooking(2);
    CHECK(byPackage.has_value());
    if (byPackage) CHECK_EQ(byPackage->price, 0LL);
    const auto snapshot = db->findBooking(3);  // уже есть снимок — не трогаем
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK_EQ(snapshot->price, 100LL);
        CHECK_EQ(snapshot->currency, std::string("RUB"));
    }
    std::remove(path.c_str());
}

TEST(audit_l23_embedded_file_safety) {
    const std::string path = "/tmp/selfvoy-audit-db-" + std::to_string(::getpid()) + ".json";
    {
        FILE* file = std::fopen(path.c_str(), "w");
        std::fputs("{\"users\": [ {\"id\": 1, ", file);  // обрезанный JSON
        std::fclose(file);
    }
    auto broken = selfvoy::makeEmbeddedDatabase(path);
    std::string error;
    CHECK(!broken->connect(error));
    CHECK(error.find("повреждён") != std::string::npos);
    // Файл не перезаписан пустой базой.
    {
        FILE* file = std::fopen(path.c_str(), "r");
        char buffer[64] = {0};
        std::fgets(buffer, sizeof buffer, file);
        std::fclose(file);
        CHECK(std::string(buffer).find("users") != std::string::npos);
    }
    std::remove(path.c_str());

    auto fresh = selfvoy::makeEmbeddedDatabase(path);
    error.clear();
    CHECK(fresh->connect(error));
    long long id = 0;
    CHECK(fresh->createUser("perm@example.com", "x", "Perm", id).ok);
    struct stat info {};
    CHECK_EQ(::stat(path.c_str(), &info), 0);
    CHECK_EQ(info.st_mode & 0777, static_cast<unsigned>(0600));
    std::remove(path.c_str());
}

TEST(audit_l24_attempts_table_bounded) {
    ::setenv("SELFVOY_LOGIN_WINDOW", "1", 1);
    ::setenv("SELFVOY_MAX_IP_ATTEMPTS", "0", 1);
    {
        Fixture fixture;
        auto& auth = fixture.server->auth();
        for (int i = 0; i < 5000; ++i) {
            auth.login("ghost" + std::to_string(i) + "@example.com", "wrongpass1", "t", "", "127.0.0.1:1");
        }
        CHECK(auth.attemptsSize() >= 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        for (int i = 0; i < 10; ++i) auth.login("late" + std::to_string(i) + "@example.com", "wrongpass1", "t", "", "127.0.0.1:1");
        CHECK(auth.attemptsSize() < 100);
    }
    ::unsetenv("SELFVOY_LOGIN_WINDOW");
    ::unsetenv("SELFVOY_MAX_IP_ATTEMPTS");
}

TEST(audit_password_unicode_rule) {
    CHECK(selfvoy::AuthManager::passwordError("пароль12").empty());
    CHECK(selfvoy::AuthManager::passwordError("Ελληνικά1").empty());
    CHECK(selfvoy::AuthManager::passwordError("Größe123").empty());
    CHECK(!selfvoy::AuthManager::passwordError("парол1").empty());     // 6 символов, хотя 11 байт
    CHECK(!selfvoy::AuthManager::passwordError("пароль-пароль").empty());  // нет цифр
    CHECK(!selfvoy::AuthManager::passwordError("12345678±").empty());  // нет букв
    CHECK(!selfvoy::AuthManager::passwordError(std::string("abc\xff" "12345")).empty());  // не UTF-8
    CHECK(!selfvoy::AuthManager::passwordError("a1" + std::string(255, 'b')).empty());   // > 256 байт
    CHECK(selfvoy::AuthManager::passwordError("a1" + std::string(254, 'b')).empty());    // ровно 256
}

TEST(audit_change_password_returns_token_pair) {
    Fixture fixture;
    auto session = fixture.makeSession("kai2");
    const Json logged = fixture.registerUser(session, "kai2@example.com", "Кай");
    const std::string oldRefresh = integration::payloadOf(logged).getString("refresh_token");
    auto other = std::make_shared<selfvoy::Session>(-1, "other", "127.0.0.1:31", fixture.config);
    fixture.call(other, "auth.login", audit::loginPayload("kai2@example.com", "selfvoy1234"));

    Json change = Json::object();
    change.set("old_password", Json("selfvoy1234"));
    change.set("new_password", Json("новыйпароль1"));
    const Json changed = fixture.call(session, "auth.changePassword", change);
    CHECK_EQ(changed.getString("type"), std::string("ok"));
    const Json payload = integration::payloadOf(changed);
    CHECK(payload.getBool("changed"));
    CHECK_EQ(payload.getString("token").size() > 20, true);
    CHECK(!payload.getString("refresh_token").empty());
    // Текущее соединение живо (под новой сессией), другое — выбито.
    CHECK(session->authenticated());
    CHECK_EQ(fixture.call(session, "auth.me").getString("type"), std::string("ok"));
    CHECK(!other->authenticated());
    // Старый refresh мёртв, новый — работает.
    auto probe = std::make_shared<selfvoy::Session>(-1, "probe", "127.0.0.1:32", fixture.config);
    Json refresh = Json::object();
    refresh.set("refresh_token", Json(oldRefresh));
    CHECK_EQ(fixture.call(probe, "auth.refresh", refresh).getString("code"), std::string("unauthorized"));
    refresh.set("refresh_token", Json(payload.getString("refresh_token")));
    CHECK_EQ(fixture.call(probe, "auth.refresh", refresh).getString("type"), std::string("ok"));
}

TEST(audit_reset_code_six_digits_with_limit) {
    Fixture fixture;
    auto session = fixture.makeSession("reset6");
    fixture.registerUser(session, "reset6@example.com", "Ресет");
    Json forgot = Json::object();
    forgot.set("email", Json("reset6@example.com"));
    const std::string code = integration::payloadOf(fixture.call(session, "auth.forgotPassword", forgot)).getString("reset_code");
    CHECK_EQ(code.size(), static_cast<std::size_t>(6));
    CHECK(std::all_of(code.begin(), code.end(), [](char ch) { return ch >= '0' && ch <= '9'; }));

    Json reset = Json::object();
    reset.set("email", Json("reset6@example.com"));
    reset.set("new_password", Json("newpass123"));
    // Слабый пароль отклоняется, не расходуя попытку и не подсказывая про код.
    reset.set("code", Json(code));
    reset.set("new_password", Json("weak"));
    CHECK_EQ(fixture.call(session, "auth.resetPassword", reset).getString("code"), std::string("bad_request"));
    reset.set("new_password", Json("newpass123"));
    for (int i = 0; i < 5; ++i) {
        reset.set("code", Json(code == "123456" ? std::string("654321") : std::string("123456")));
        CHECK_EQ(fixture.call(session, "auth.resetPassword", reset).getString("code"), std::string("bad_request"));
    }
    reset.set("code", Json(code));
    CHECK_EQ(fixture.call(session, "auth.resetPassword", reset).getString("code"), std::string("bad_request"));
    // Новый код работает.
    const std::string fresh = integration::payloadOf(fixture.call(session, "auth.forgotPassword", forgot)).getString("reset_code");
    reset.set("code", Json(fresh));
    CHECK(fixture.call(session, "auth.resetPassword", reset).get("payload").getBool("reset"));
}

// M15: после рестарта/обрыва PostgreSQL сервер переподключается сам.
// Запускается, только если сервер собран с libpq и задан SELFVOY_TEST_PG_URL.
TEST(audit_m15_postgres_reconnect) {
#ifdef SELFVOY_WITH_LIBPQ
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    const std::string base = url;
    const std::string appUrl = base + (base.find('?') == std::string::npos ? "?" : "&") +
                               "application_name=selfvoy-reconnect-test";
    auto database = selfvoy::makePostgresDatabase(appUrl);
    std::string error;
    CHECK(database && database->connect(error));
    if (!database) return;
    CHECK(database->findUserById(1).has_value() || true);
    // Обрываем соединение сервера со стороны PostgreSQL.
    PGconn* admin = PQconnectdb(base.c_str());
    CHECK(PQstatus(admin) == CONNECTION_OK);
    PGresult* killed = PQexec(admin,
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
        "WHERE application_name = 'selfvoy-reconnect-test' AND pid <> pg_backend_pid()");
    CHECK(PQresultStatus(killed) == PGRES_TUPLES_OK && PQntuples(killed) >= 1);
    PQclear(killed);
    PQfinish(admin);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Первый же запрос после обрыва проходит (переподключение + повтор).
    const auto users = database->searchUsers("zz-no-such-user-zz", 1);
    CHECK(users.empty());
    CHECK(database->healthy());
    long long id = 0;
    const std::string email = "reconnect-" + std::to_string(::getpid()) + "@example.com";
    CHECK(database->createUser(email, "x", "Reconnect", id).ok);
    CHECK(database->findUserByEmail(email).has_value());
    // Ошибка БД наружу — обобщённая, без текста PostgreSQL.
    long long dup = 0;
    const auto duplicate = database->createUser(email, "x", "Reconnect", dup);
    CHECK(!duplicate.ok);
    CHECK(duplicate.message.find("duplicate") == std::string::npos);
    CHECK(duplicate.message.find("users") == std::string::npos);
#else
    std::cout << "  (пропущен: сборка без libpq)\n";
#endif
}

TEST(audit_a11_external_tools_get_no_ai_token) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    CHECK_EQ(::listen(listener, 1), 0);
    socklen_t len = sizeof addr;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const int port = ntohs(addr.sin_port);

    std::string request;
    std::thread fake([&] {
        const int fd = ::accept(listener, nullptr, nullptr);
        char chunk[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            request.append(chunk, static_cast<std::size_t>(n));
        }
        const std::string body = "{\"results\":[]}";
        const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        ::send(fd, reply.data(), reply.size(), 0);
        ::close(fd);
    });

    Fixture fixture;
    selfvoy::Config config = fixture.config;
    config.toolsMode = "http";
    config.mapsApiUrl = "http://127.0.0.1:" + std::to_string(port) + "/places";
    config.aiServiceToken = "super-secret-ai-token";
    selfvoy::ToolManager tools(config, fixture.server->database(), fixture.server->chats());
    Json args = Json::object();
    args.set("city", Json("Херлен"));
    CHECK(tools.findCafe(1, args).ok);
    fake.join();
    ::close(listener);
    CHECK(request.find("POST /places") != std::string::npos);
    CHECK(request.find("super-secret-ai-token") == std::string::npos);
    CHECK(request.find("X-Selfvoy-Token") == std::string::npos);
}

// ------------------------------------------------------- админ-панель (admin.*)
namespace admin_test {

// Сессия на собственном socketpair: Fixture::makeSession закрывает конец
// предыдущей сессии, а здесь нужно, чтобы несколько соединений были «живыми»
// (иначе нечего проверять при выбивании соединений).
struct Sockets {
    std::vector<int> fds;
    ~Sockets() {
        for (const int fd : fds) ::close(fd);
    }
    std::shared_ptr<selfvoy::Session> open(Fixture& fixture, const std::string& id) {
        int pair[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return nullptr;
        fds.push_back(pair[0]);
        fds.push_back(pair[1]);
        return std::make_shared<selfvoy::Session>(pair[1], id, "127.0.0.1:0", fixture.config);
    }
};

long long userIdOf(const Json& loginResponse) {
    return loginResponse.get("payload").get("user").getInt("id");
}

Json byId(long long userId) {
    Json payload = Json::object();
    payload.set("user_id", Json(userId));
    return payload;
}

Json setActivePayload(long long userId, bool active, const std::string& reason) {
    Json payload = byId(userId);
    payload.set("active", Json(active));
    payload.set("reason", Json(reason));
    return payload;
}

Json setRolePayload(long long userId, const std::string& role) {
    Json payload = byId(userId);
    payload.set("role", Json(role));
    return payload;
}

Json loginPayload(const std::string& email) {
    Json payload = Json::object();
    payload.set("email", Json(email));
    payload.set("password", Json("selfvoy1234"));
    return payload;
}

}  // namespace admin_test

TEST(admin_role_in_auth_payloads) {
    Fixture fixture;
    auto session = fixture.makeSession("role-user");
    const Json login = fixture.registerUser(session, "plain@example.com", "Плейн");
    CHECK_EQ(login.get("payload").get("user").getString("role"), std::string("user"));
    CHECK_EQ(integration::payloadOf(fixture.call(session, "auth.me")).getString("role"), std::string("user"));
    // Обычный пользователь в админ-панель не попадает.
    CHECK_EQ(fixture.call(session, "admin.stats").getString("code"), std::string("forbidden"));
    CHECK_EQ(fixture.call(session, "admin.users").getString("code"), std::string("forbidden"));
    CHECK_EQ(fixture.call(session, "admin.audit").getString("code"), std::string("forbidden"));
    // Без входа — unauthorized: методы admin.* не публичные.
    auto anonymous = fixture.makeSession("role-anon");
    CHECK_EQ(fixture.call(anonymous, "admin.stats").getString("code"), std::string("unauthorized"));
}

TEST(admin_bootstrap_from_env_verified_only) {
    ::setenv("SELFVOY_ADMIN_EMAILS", "Boss@Example.com, ghost@example.com;late@example.com", 1);
    Fixture fixture;
    ::unsetenv("SELFVOY_ADMIN_EMAILS");
    CHECK_EQ(fixture.config.adminEmails.empty(), false);

    // Подтверждённый адрес из списка — admin сразу после verifyEmail; ответ входа уже с ролью.
    auto boss = fixture.makeSession("boss");
    const Json login = fixture.registerUser(boss, "boss@example.com", "Босс");
    CHECK_EQ(login.get("payload").get("user").getString("role"), std::string("admin"));
    CHECK_EQ(integration::payloadOf(fixture.call(boss, "auth.me")).getString("role"), std::string("admin"));
    CHECK_EQ(fixture.call(boss, "admin.stats").getString("type"), std::string("ok"));

    // Неподтверждённый адрес из списка роль не получает (иначе её взял бы тот,
    // кто первым зарегистрировал чужой email).
    auto ghost = fixture.makeSession("ghost");
    Json reg = Json::object();
    reg.set("email", Json("ghost@example.com"));
    reg.set("password", Json("selfvoy1234"));
    reg.set("display_name", Json("Призрак"));
    CHECK_EQ(fixture.call(ghost, "auth.register", reg).getString("type"), std::string("ok"));
    auto& db = fixture.server->database().db();
    CHECK_EQ(db.findUserByEmail("ghost@example.com")->role, std::string("user"));
    CHECK_EQ(fixture.call(ghost, "auth.login", admin_test::loginPayload("ghost@example.com")).getString("code"),
             std::string("email_not_verified"));
    CHECK_EQ(db.findUserByEmail("ghost@example.com")->role, std::string("user"));

    // Адрес не из списка — обычный пользователь.
    auto other = fixture.makeSession("other");
    fixture.registerUser(other, "other@example.com", "Другой");
    CHECK_EQ(db.findUserByEmail("other@example.com")->role, std::string("user"));

    // bootstrapAll при старте: пользователь подтвердился раньше, чем адрес попал в список.
    fixture.config.adminEmails = "other@example.com";
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    CHECK(fixture.server->start(error));
    auto& restarted = fixture.server->database().db();
    CHECK_EQ(restarted.findUserByEmail("other@example.com")->role, std::string("admin"));
    bool bootstrapAudited = false;
    const long long otherId = restarted.findUserByEmail("other@example.com")->id;
    for (const auto& entry : restarted.listAuditPage(otherId, 0, 10)) {
        if (entry.kind == "role_bootstrap") bootstrapAudited = true;
    }
    CHECK(bootstrapAudited);
}

TEST(admin_moderator_vs_admin_permissions) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto adminSession = sockets.open(fixture, "perm-admin");
    auto modSession = sockets.open(fixture, "perm-mod");
    auto userSession = sockets.open(fixture, "perm-user");
    auto peerSession = sockets.open(fixture, "perm-peer");
    const long long adminId = admin_test::userIdOf(fixture.registerUser(adminSession, "padmin@example.com", "Админ"));
    const long long modId = admin_test::userIdOf(fixture.registerUser(modSession, "pmod@example.com", "Модератор"));
    const long long userId = admin_test::userIdOf(fixture.registerUser(userSession, "puser@example.com", "Юзер"));
    const long long peerId = admin_test::userIdOf(fixture.registerUser(peerSession, "ppeer@example.com", "Пир"));
    auto& db = fixture.server->database().db();
    CHECK(db.setUserRole(adminId, "admin").ok);

    // Администратор назначает модератора; роль видна в ответе.
    const Json promoted = fixture.call(adminSession, "admin.setRole", admin_test::setRolePayload(modId, "moderator"));
    CHECK_EQ(promoted.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(promoted).get("user").getString("role"), std::string("moderator"));
    CHECK_EQ(fixture.call(adminSession, "admin.setRole", admin_test::setRolePayload(modId, "root")).getString("code"),
             std::string("bad_request"));

    // Модератор: обзор и список — да; роли и журнал — нет.
    CHECK_EQ(fixture.call(modSession, "admin.stats").getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(modSession, "admin.users").getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(modSession, "admin.audit").getString("code"), std::string("forbidden"));
    CHECK_EQ(fixture.call(modSession, "admin.setRole", admin_test::setRolePayload(userId, "moderator")).getString("code"),
             std::string("forbidden"));
    // Модератор не трогает администраторов, модераторов и себя.
    CHECK_EQ(fixture.call(modSession, "admin.setActive", admin_test::setActivePayload(adminId, false, "тест"))
                 .getString("code"),
             std::string("forbidden"));
    CHECK_EQ(fixture.call(modSession, "admin.revokeSessions", admin_test::byId(adminId)).getString("code"),
             std::string("forbidden"));
    CHECK_EQ(fixture.call(modSession, "admin.setActive", admin_test::setActivePayload(modId, false, "тест"))
                 .getString("code"),
             std::string("forbidden"));
    // Причина блокировки обязательна.
    CHECK_EQ(fixture.call(modSession, "admin.setActive", admin_test::setActivePayload(userId, false, "  "))
                 .getString("code"),
             std::string("bad_request"));
    // Обычного пользователя модератор блокирует и разблокирует.
    const Json blocked = fixture.call(modSession, "admin.setActive", admin_test::setActivePayload(userId, false, "спам"));
    CHECK_EQ(blocked.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(blocked).get("user").getBool("is_active", true), false);
    CHECK_EQ(fixture.call(modSession, "admin.setActive", admin_test::setActivePayload(userId, true, ""))
                 .getString("type"),
             std::string("ok"));

    // Карточка пользователя: модератор видит события, но без IP-адресов.
    const Json modView = integration::payloadOf(fixture.call(modSession, "admin.user", admin_test::byId(userId)));
    CHECK_EQ(modView.get("user").getString("email"), std::string("puser@example.com"));
    CHECK(modView.get("audit").items().size() > 0);
    for (const auto& entry : modView.get("audit").items()) CHECK(entry.getString("remote_addr").empty());
    const Json adminView = integration::payloadOf(fixture.call(adminSession, "admin.user", admin_test::byId(userId)));
    bool anyAddress = false;
    for (const auto& entry : adminView.get("audit").items()) anyAddress |= !entry.getString("remote_addr").empty();
    CHECK(anyAddress);
    CHECK_EQ(fixture.call(modSession, "admin.user", admin_test::byId(999999)).getString("code"),
             std::string("not_found"));

    // Свою роль и статус не меняет никто, в том числе единственный администратор
    // (так последний администратор не может понизить сам себя).
    CHECK_EQ(fixture.call(adminSession, "admin.setRole", admin_test::setRolePayload(adminId, "user")).getString("code"),
             std::string("forbidden"));
    CHECK_EQ(fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(adminId, false, "я"))
                 .getString("code"),
             std::string("forbidden"));
    CHECK_EQ(db.findUserById(adminId)->role, std::string("admin"));

    // Второй администратор может понизить первого; тогда последний — он сам, и себя не понизит.
    CHECK_EQ(fixture.call(adminSession, "admin.setRole", admin_test::setRolePayload(peerId, "admin")).getString("type"),
             std::string("ok"));
    CHECK_EQ(fixture.call(peerSession, "admin.setRole", admin_test::setRolePayload(adminId, "user")).getString("type"),
             std::string("ok"));
    CHECK_EQ(fixture.call(peerSession, "admin.setRole", admin_test::setRolePayload(peerId, "moderator")).getString("code"),
             std::string("forbidden"));
    // Понижение действует сразу: бывший администратор без перевхода теряет доступ.
    CHECK_EQ(fixture.call(adminSession, "admin.stats").getString("code"), std::string("forbidden"));

    // Роль модератора/админа — только активному пользователю с подтверждённым email.
    Json reg = Json::object();
    reg.set("email", Json("pnew@example.com"));
    reg.set("password", Json("selfvoy1234"));
    reg.set("display_name", Json("Новый"));
    fixture.call(fixture.makeSession("perm-new"), "auth.register", reg);
    const long long freshId = db.findUserByEmail("pnew@example.com")->id;
    CHECK_EQ(fixture.call(peerSession, "admin.setRole", admin_test::setRolePayload(freshId, "moderator")).getString("code"),
             std::string("bad_request"));

    // Журнал: смена роли с old_role и актором.
    bool roleAudited = false;
    for (const auto& entry : db.listAuditPage(adminId, 0, 20)) {
        if (entry.kind == "admin_role_changed" && entry.detail.getString("old_role") == "admin" &&
            entry.detail.getString("role") == "user" && entry.detail.getString("actor_email") == "ppeer@example.com") {
            roleAudited = true;
        }
    }
    CHECK(roleAudited);
}

TEST(admin_disable_kicks_and_blocks_login) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto adminSession = sockets.open(fixture, "dis-admin");
    auto victim = sockets.open(fixture, "dis-victim");
    auto victimSecond = sockets.open(fixture, "dis-victim-2");
    const long long adminId = admin_test::userIdOf(fixture.registerUser(adminSession, "dadmin@example.com", "Админ"));
    const Json victimLogin = fixture.registerUser(victim, "dvictim@example.com", "Нарушитель");
    const long long victimId = admin_test::userIdOf(victimLogin);
    const std::string victimToken = victimLogin.get("payload").getString("token");
    const std::string victimRefresh = victimLogin.get("payload").getString("refresh_token");
    CHECK_EQ(fixture.call(victimSecond, "auth.login", admin_test::loginPayload("dvictim@example.com")).getString("type"),
             std::string("ok"));
    auto& db = fixture.server->database().db();
    CHECK(db.setUserRole(adminId, "admin").ok);
    CHECK_EQ(fixture.server->connections().forUser(victimId).size(), std::size_t(2));

    const Json stats = integration::payloadOf(fixture.call(adminSession, "admin.stats"));
    CHECK_EQ(stats.getInt("online_now"), 2);  // два разных пользователя, у нарушителя два соединения

    const Json disabled =
        fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(victimId, false, "мошенничество"));
    CHECK_EQ(disabled.getString("type"), std::string("ok"));
    // Все открытые соединения выбиты, сессии и refresh-токены отозваны.
    CHECK_EQ(victim->alive(), false);
    CHECK_EQ(victimSecond->alive(), false);
    CHECK_EQ(fixture.server->connections().forUser(victimId).size(), std::size_t(0));
    CHECK_EQ(integration::payloadOf(fixture.call(adminSession, "admin.user", admin_test::byId(victimId)))
                 .getInt("sessions_active", -1),
             0);

    // Вход, старый токен и refresh больше не работают.
    auto retry = fixture.makeSession("dis-retry");
    CHECK_EQ(fixture.call(retry, "auth.login", admin_test::loginPayload("dvictim@example.com")).getString("code"),
             std::string("forbidden"));
    Json tokenPayload = Json::object();
    tokenPayload.set("token", Json(victimToken));
    CHECK_EQ(fixture.call(retry, "auth.token", tokenPayload).getString("code"), std::string("unauthorized"));
    CHECK(!retry->authenticated());
    Json refreshPayload = Json::object();
    refreshPayload.set("refresh_token", Json(victimRefresh));
    CHECK_EQ(fixture.call(retry, "auth.refresh", refreshPayload).getString("code"), std::string("unauthorized"));

    // Журнал: причина, актор и число завершённых сессий.
    bool audited = false;
    for (const auto& entry : db.listAuditPage(victimId, 0, 20)) {
        if (entry.kind == "admin_user_disabled" && entry.detail.getString("reason") == "мошенничество" &&
            entry.detail.getString("actor_email") == "dadmin@example.com" && entry.detail.getInt("revoked") == 2) {
            audited = true;
        }
    }
    CHECK(audited);

    // Даже живая сессия в БД (созданная в обход) не пускает заблокированного по токену.
    std::string jwtId;
    const std::string forged = fixture.server->auth().issueToken(victimId, "dvictim@example.com", jwtId);
    selfvoy::SessionRecord record;
    record.id = "00000000000000000000000000000001";
    record.userId = victimId;
    record.jwtId = jwtId;
    record.expiresAt = "2999-01-01T00:00:00Z";
    CHECK(db.createSession(record).ok);
    auto forgedSession = fixture.makeSession("dis-forged");
    Json forgedPayload = Json::object();
    forgedPayload.set("token", Json(forged));
    CHECK_EQ(fixture.call(forgedSession, "auth.token", forgedPayload).getString("code"), std::string("unauthorized"));
    db.revokeAllSessions(victimId, "");

    // Разблокировка: вход снова работает.
    CHECK_EQ(fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(victimId, true, ""))
                 .getString("type"),
             std::string("ok"));
    auto back = fixture.makeSession("dis-back");
    CHECK_EQ(fixture.call(back, "auth.login", admin_test::loginPayload("dvictim@example.com")).getString("type"),
             std::string("ok"));
}

TEST(admin_revoke_sessions_kicks) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto adminSession = sockets.open(fixture, "rev-admin");
    auto target = sockets.open(fixture, "rev-target");
    const long long adminId = admin_test::userIdOf(fixture.registerUser(adminSession, "radmin@example.com", "Админ"));
    const Json targetLogin = fixture.registerUser(target, "rtarget@example.com", "Цель");
    const long long targetId = admin_test::userIdOf(targetLogin);
    CHECK(fixture.server->database().db().setUserRole(adminId, "admin").ok);

    const Json revoked = fixture.call(adminSession, "admin.revokeSessions", admin_test::byId(targetId));
    CHECK_EQ(revoked.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(revoked).getInt("revoked"), 1);
    CHECK_EQ(target->alive(), false);
    auto retry = fixture.makeSession("rev-retry");
    Json tokenPayload = Json::object();
    tokenPayload.set("token", Json(targetLogin.get("payload").getString("token")));
    CHECK_EQ(fixture.call(retry, "auth.token", tokenPayload).getString("code"), std::string("unauthorized"));
    // Аккаунт не заблокирован: войти заново можно.
    CHECK_EQ(fixture.call(retry, "auth.login", admin_test::loginPayload("rtarget@example.com")).getString("type"),
             std::string("ok"));
    CHECK_EQ(fixture.call(adminSession, "admin.revokeSessions", admin_test::byId(adminId)).getString("code"),
             std::string("forbidden"));
    bool audited = false;
    for (const auto& entry : fixture.server->database().db().listAuditPage(targetId, 0, 20)) {
        if (entry.kind == "admin_sessions_revoked" && entry.detail.getInt("revoked") == 1) audited = true;
    }
    CHECK(audited);
}

TEST(admin_users_filters_and_stats) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto adminSession = sockets.open(fixture, "flt-admin");
    const long long adminId = admin_test::userIdOf(fixture.registerUser(adminSession, "fadmin@example.com", "Админ"));
    auto& db = fixture.server->database().db();
    CHECK(db.setUserRole(adminId, "admin").ok);
    for (const char* name : {"ivan", "maria", "petr", "olga"}) {
        auto session = fixture.makeSession(std::string("flt-") + name);
        fixture.registerUser(session, std::string(name) + "@example.com", std::string("Имя ") + name);
    }
    // Неподтверждённый пользователь.
    Json reg = Json::object();
    reg.set("email", Json("unverified@example.com"));
    reg.set("password", Json("selfvoy1234"));
    reg.set("display_name", Json("Ivan Unverified"));
    fixture.call(fixture.makeSession("flt-unv"), "auth.register", reg);
    // Модератор, заблокированный и пользователь с 2FA.
    const long long ivanId = db.findUserByEmail("ivan@example.com")->id;
    const long long mariaId = db.findUserByEmail("maria@example.com")->id;
    const long long petrId = db.findUserByEmail("petr@example.com")->id;
    CHECK(db.setUserRole(mariaId, "moderator").ok);
    CHECK_EQ(fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(petrId, false, "спам"))
                 .getString("type"),
             std::string("ok"));
    selfvoy::TwoFactorRecord twoFactor;
    twoFactor.userId = ivanId;
    twoFactor.secretEncrypted = "x";
    twoFactor.enabled = true;
    CHECK(db.upsertTwoFactor(twoFactor).ok);

    const auto list = [&](const std::string& query, const std::string& role, const std::string& status, int limit,
                          int offset) {
        Json payload = Json::object();
        payload.set("query", Json(query));
        payload.set("role", Json(role));
        payload.set("status", Json(status));
        payload.set("limit", Json(limit));
        payload.set("offset", Json(offset));
        return integration::payloadOf(fixture.call(adminSession, "admin.users", payload));
    };
    const Json all = list("", "", "", 50, 0);
    CHECK_EQ(all.getInt("total"), 6);
    CHECK_EQ(all.get("users").items().size(), std::size_t(6));
    CHECK(all.get("users").at(0).getInt("id") > all.get("users").at(1).getInt("id"));  // новые первыми
    const Json first = all.get("users").at(0);
    for (const char* key : {"id", "email", "display_name", "avatar_url", "role", "is_active", "email_verified",
                            "two_factor", "created_at", "last_seen_at"}) {
        CHECK(first.contains(key));
    }
    CHECK(!first.contains("password_hash"));

    // Поиск по email и имени без учёта регистра.
    CHECK_EQ(list("IVAN", "", "", 50, 0).getInt("total"), 2);  // ivan@ и «Ivan Unverified»
    CHECK_EQ(list("ivan", "", "unverified", 50, 0).getInt("total"), 1);
    CHECK_EQ(list("", "", "unverified", 50, 0).get("users").at(0).getString("email"),
             std::string("unverified@example.com"));
    CHECK_EQ(list("", "", "disabled", 50, 0).get("users").at(0).getInt("id"), petrId);
    CHECK_EQ(list("", "", "active", 50, 0).getInt("total"), 5);
    CHECK_EQ(list("", "moderator", "", 50, 0).get("users").at(0).getInt("id"), mariaId);
    CHECK_EQ(list("", "admin", "", 50, 0).getInt("total"), 1);
    CHECK_EQ(list("ivan@", "", "", 50, 0).get("users").at(0).getBool("two_factor"), true);
    CHECK_EQ(list("100%", "", "", 50, 0).getInt("total"), 0);  // спецсимволы LIKE — буквально
    // Страницы: total не зависит от limit/offset.
    const Json page1 = list("", "", "", 4, 0);
    const Json page2 = list("", "", "", 4, 4);
    CHECK_EQ(page1.get("users").items().size(), std::size_t(4));
    CHECK_EQ(page2.get("users").items().size(), std::size_t(2));
    CHECK_EQ(page2.getInt("total"), 6);
    CHECK(page1.get("users").at(3).getInt("id") > page2.get("users").at(0).getInt("id"));
    // Неверные фильтры.
    Json badStatus = Json::object();
    badStatus.set("status", Json("banned"));
    CHECK_EQ(fixture.call(adminSession, "admin.users", badStatus).getString("code"), std::string("bad_request"));
    Json badRole = Json::object();
    badRole.set("role", Json("owner"));
    CHECK_EQ(fixture.call(adminSession, "admin.users", badRole).getString("code"), std::string("bad_request"));

    const Json stats = integration::payloadOf(fixture.call(adminSession, "admin.stats"));
    CHECK_EQ(stats.getInt("users_total"), 6);
    CHECK_EQ(stats.getInt("users_verified"), 5);
    CHECK_EQ(stats.getInt("users_disabled"), 1);
    CHECK_EQ(stats.getInt("users_2fa"), 1);
    CHECK_EQ(stats.getInt("users_new_7d"), 6);
    CHECK_EQ(stats.getInt("moderators"), 1);
    CHECK_EQ(stats.getInt("admins"), 1);
    CHECK_EQ(stats.getInt("sessions_active"), 4);  // 5 входов, сессия petr отозвана блокировкой
    CHECK(stats.contains("online_now"));
}

TEST(admin_audit_paging) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto adminSession = sockets.open(fixture, "aud-admin");
    auto userSession = sockets.open(fixture, "aud-user");
    const long long adminId = admin_test::userIdOf(fixture.registerUser(adminSession, "aadmin@example.com", "Админ"));
    const long long userId = admin_test::userIdOf(fixture.registerUser(userSession, "auser@example.com", "Юзер"));
    CHECK(fixture.server->database().db().setUserRole(adminId, "admin").ok);
    for (int i = 0; i < 3; ++i) {
        fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(userId, false, "раз"));
        fixture.call(adminSession, "admin.setActive", admin_test::setActivePayload(userId, true, ""));
    }

    const auto page = [&](long long user, long long before, int limit) {
        Json payload = Json::object();
        payload.set("user_id", Json(user));
        payload.set("before_id", Json(before));
        payload.set("limit", Json(limit));
        return integration::payloadOf(fixture.call(adminSession, "admin.audit", payload)).get("entries");
    };
    const Json first = page(0, 0, 5);
    CHECK_EQ(first.items().size(), std::size_t(5));
    for (std::size_t i = 1; i < first.items().size(); ++i) {
        CHECK(first.at(i - 1).getInt("id") > first.at(i).getInt("id"));
    }
    const long long lastId = first.at(4).getInt("id");
    const Json second = page(0, lastId, 5);
    CHECK(second.items().size() > 0);
    for (const auto& entry : second.items()) CHECK(entry.getInt("id") < lastId);
    // Фильтр по пользователю + email в каждой записи.
    const Json onlyUser = page(userId, 0, 200);
    CHECK(onlyUser.items().size() >= 6);
    for (const auto& entry : onlyUser.items()) {
        CHECK_EQ(entry.getInt("user_id"), userId);
        CHECK_EQ(entry.getString("user_email"), std::string("auser@example.com"));
    }
    // Дошли до конца — пустая страница.
    const long long oldest = page(0, 0, 200).items().back().getInt("id");
    CHECK_EQ(page(0, oldest, 10).items().size(), std::size_t(0));
}

// ------------------------------------------------- приглашения по ссылке (invite.*)
namespace invite_test {

// Поддельный AI-сервис: отвечает на каждый POST телом из reply(); запросы
// складываются в requests. Живёт до деструктора (закрывает слушатель).
struct FakeAi {
    int listener = -1;
    int port = 0;
    std::function<std::string()> reply;
    std::vector<std::string> requests;
    std::mutex mutex;
    std::thread thread;
    bool stopping = false;

    explicit FakeAi(std::function<std::string()> body) : reply(std::move(body)) {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(listener, 8);
        socklen_t len = sizeof addr;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread = std::thread([this] {
            for (;;) {
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) return;
                std::string request;
                char chunk[4096];
                std::size_t need = std::string::npos;
                while (need == std::string::npos || request.size() < need) {
                    const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
                    if (n <= 0) break;
                    request.append(chunk, static_cast<std::size_t>(n));
                    const std::size_t head = request.find("\r\n\r\n");
                    if (head != std::string::npos && need == std::string::npos) {
                        std::size_t length = 0;
                        const std::size_t cl = request.find("Content-Length: ");
                        if (cl != std::string::npos && cl < head) length = std::stoul(request.substr(cl + 16));
                        need = head + 4 + length;
                    }
                }
                std::string body;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) {
                        ::close(fd);
                        return;
                    }
                    requests.push_back(request);
                    body = reply();
                }
                const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                ::send(fd, answer.data(), answer.size(), 0);
                ::close(fd);
            }
        });
    }

    ~FakeAi() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

// Час, кратный получасу, через hours часов от текущего момента (UTC).
std::time_t future(int hours) {
    const std::time_t now = std::time(nullptr);
    return (now / 1800 + 1) * 1800 + static_cast<std::time_t>(hours) * 3600;
}

Json slot(std::time_t start, int minutes) {
    Json json = Json::object();
    json.set("start", Json(selfvoy::isoUtc(start)));
    json.set("end", Json(selfvoy::isoUtc(start + minutes * 60)));
    json.set("score", Json(0.9));
    json.set("reason", Json("удобное время"));
    return json;
}

// Перезапуск сервера фикстуры с адресом поддельного AI (БД-файл сохраняется).
void useAi(Fixture& fixture, const std::string& url, const std::string& publicUrl = "") {
    fixture.config.aiServiceUrl = url;
    fixture.config.publicUrl = publicUrl;
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    if (!fixture.server->start(error)) std::cout << "  не удалось перезапустить сервер: " << error << "\n";
}

Json createPayload(const std::string& topic, long long duration = 60) {
    Json payload = Json::object();
    payload.set("topic", Json(topic));
    payload.set("duration_minutes", Json(duration));
    payload.set("window_hours", Json(168LL));
    return payload;
}

Json respondPayload(const std::string& token, const std::string& action, const std::string& start,
                    const std::string& name) {
    Json payload = Json::object();
    payload.set("token", Json(token));
    payload.set("action", Json(action));
    if (!start.empty()) payload.set("start", Json(start));
    payload.set("guest_name", Json(name));
    return payload;
}

Json tokenPayload(const std::string& token) {
    Json payload = Json::object();
    payload.set("token", Json(token));
    return payload;
}

}  // namespace invite_test

TEST(invite_accept_flow) {
    using namespace invite_test;
    const std::time_t first = future(26);
    const std::time_t second = future(50);
    const std::time_t third = future(74);
    // AI предлагает и лишнее: прошлое время, чужую длительность и пересечение —
    // сервер это отбрасывает (LLM/AI не источник истины).
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(slot(third, 60));
        slots.push(slot(first, 60));
        slots.push(slot(std::time(nullptr) - 7200, 60));
        slots.push(slot(second, 45));
        slots.push(slot(second, 60));
        slots.push(slot(first + 1800, 60));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    useAi(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "invite-owner");
    fixture.registerUser(owner, "host@example.com", "Иван");

    const Json created = fixture.call(owner, "invite.create", createPayload("Кофе и план"));
    CHECK_EQ(created.getString("type"), std::string("ok"));
    if (created.getString("type") != "ok") {
        std::cout << "  invite.create: " << created.dump() << "\n";
        return;
    }
    const Json createdPayload = integration::payloadOf(created);
    const std::string token = createdPayload.getString("token");
    CHECK_EQ(token.size(), std::size_t(43));
    CHECK_EQ(createdPayload.getString("url"), "https://selfvoy.example.com/#/i/" + token);
    const Json invite = createdPayload.get("invite");
    CHECK_EQ(invite.getString("status"), std::string("open"));
    CHECK_EQ(invite.get("slots").items().size(), std::size_t(3));
    CHECK_EQ(invite.get("slots").items().front().getString("start"), selfvoy::isoUtc(first));
    CHECK_EQ(invite.getString("expires_at"), selfvoy::isoUtc(third));
    {
        std::lock_guard<std::mutex> lock(ai.mutex);
        // Первый запрос — проверка /healthz при старте сервера.
        const auto slotsRequest = std::find_if(ai.requests.begin(), ai.requests.end(), [](const std::string& r) {
            return r.find("POST /v1/agent/slots") != std::string::npos;
        });
        CHECK(slotsRequest != ai.requests.end());
        if (slotsRequest != ai.requests.end()) {
            CHECK(slotsRequest->find("\"duration_minutes\":60") != std::string::npos);
            CHECK(slotsRequest->find("\"context\"") != std::string::npos);
        }
    }
    // В БД — только хэш токена; в списке владельца токена нет.
    auto& db = fixture.server->database().db();
    const auto stored = db.findInvite(invite.getInt("id"));
    CHECK(stored.has_value());
    CHECK(stored->tokenHash != token);
    CHECK_EQ(stored->tokenHash, selfvoy::InviteManager::tokenHash(token));
    const Json listed = integration::payloadOf(fixture.call(owner, "invite.list"));
    CHECK_EQ(listed.get("invites").items().size(), std::size_t(1));
    CHECK(listed.dump().find(token) == std::string::npos);

    // Гость без входа видит приглашение, но не email владельца.
    auto guest = sockets.open(fixture, "invite-guest");
    const Json seen = fixture.call(guest, "invite.get", tokenPayload(token));
    CHECK_EQ(seen.getString("type"), std::string("ok"));
    const Json guestView = integration::payloadOf(seen).get("invite");
    CHECK_EQ(guestView.getString("owner_name"), std::string("Иван"));
    CHECK_EQ(guestView.getString("topic"), std::string("Кофе и план"));
    CHECK_EQ(guestView.get("slots").items().size(), std::size_t(3));
    CHECK(seen.dump().find("host@example.com") == std::string::npos);
    CHECK_EQ(fixture.call(guest, "invite.get", tokenPayload(std::string(43, 'A'))).getString("code"),
             std::string("not_found"));
    CHECK_EQ(fixture.call(guest, "invite.get", tokenPayload("short")).getString("code"), std::string("not_found"));

    // Ошибки ввода гостя.
    CHECK_EQ(fixture.call(guest, "invite.respond", respondPayload(token, "accept", selfvoy::isoUtc(first + 600), "Мария"))
                 .getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fixture.call(guest, "invite.respond", respondPayload(token, "accept", selfvoy::isoUtc(first), "  "))
                 .getString("code"),
             std::string("bad_request"));
    Json badEmail = respondPayload(token, "accept", selfvoy::isoUtc(first), "Мария");
    badEmail.set("guest_email", Json("not an email"));
    CHECK_EQ(fixture.call(guest, "invite.respond", badEmail).getString("code"), std::string("bad_request"));

    // Выбор времени: ответ гостя закрывает приглашение.
    Json accept = respondPayload(token, "accept", selfvoy::isoUtc(second), "Мария");
    accept.set("guest_email", Json("maria@example.com"));
    accept.set("comment", Json("Возьму ноутбук"));
    const Json accepted = fixture.call(guest, "invite.respond", accept);
    CHECK_EQ(accepted.getString("type"), std::string("ok"));
    const Json acceptedView = integration::payloadOf(accepted).get("invite");
    CHECK_EQ(acceptedView.getString("status"), std::string("accepted"));
    CHECK_EQ(acceptedView.get("chosen").getString("start"), selfvoy::isoUtc(second));
    CHECK_EQ(acceptedView.get("chosen").getString("end"), selfvoy::isoUtc(second + 3600));
    CHECK_EQ(acceptedView.get("slots").items().size(), std::size_t(0));
    // Повторный ответ (в т.ч. отказ) — conflict.
    CHECK_EQ(fixture.call(guest, "invite.respond", respondPayload(token, "decline", "", "Мария")).getString("code"),
             std::string("conflict"));

    // У владельца: статус, задача с напоминанием, встреча в расписании, уведомление.
    const Json invitesAfter = integration::payloadOf(fixture.call(owner, "invite.list")).get("invites");
    CHECK_EQ(invitesAfter.items().size(), std::size_t(1));
    if (invitesAfter.items().empty()) return;
    const Json after = invitesAfter.items().front();
    CHECK_EQ(after.getString("status"), std::string("accepted"));
    CHECK_EQ(after.getString("guest_name"), std::string("Мария"));
    CHECK_EQ(after.getString("guest_email"), std::string("maria@example.com"));
    CHECK_EQ(after.getString("guest_comment"), std::string("Возьму ноутбук"));
    bool taskFound = false;
    const Json tasks = integration::payloadOf(fixture.call(owner, "tasks.list")).get("tasks");
    for (const auto& task : tasks.items()) {
        if (task.getString("title") != "Встреча: Кофе и план — Мария") continue;
        taskFound = true;
        // Сравниваем моменты, а не строки: PostgreSQL и встроенная БД пишут «.000Z».
        std::time_t due = 0, remind = 0;
        CHECK(selfvoy::parseIsoUtc(task.getString("due_at"), due));
        CHECK(selfvoy::parseIsoUtc(task.getString("remind_at"), remind));
        CHECK_EQ(due, second);
        CHECK_EQ(remind, second - 900);
        CHECK(task.getString("notes").find("maria@example.com") != std::string::npos);
    }
    CHECK(taskFound);
    const long long ownerId = db.findUserByEmail("host@example.com")->id;
    bool busy = false;
    for (const auto& [start, end] : fixture.server->memory().busy(ownerId)) {
        if (start == second && end == second + 3600) busy = true;
    }
    CHECK(busy);
    bool notified = false;
    const Json notifications = integration::payloadOf(fixture.call(owner, "notifications.list")).get("notifications");
    for (const auto& item : notifications.items()) {
        if (item.getString("kind") == "invite.accepted") {
            notified = true;
            CHECK_EQ(item.get("payload").getString("start"), selfvoy::isoUtc(second));
        }
    }
    CHECK(notified);
    // Страница гостя после выбора: время видно, слотов нет.
    const Json reopened = integration::payloadOf(fixture.call(guest, "invite.get", tokenPayload(token))).get("invite");
    CHECK_EQ(reopened.getString("status"), std::string("accepted"));
    CHECK_EQ(reopened.get("chosen").getString("start"), selfvoy::isoUtc(second));
}

TEST(invite_busy_cancel_and_decline) {
    using namespace invite_test;
    const std::time_t first = future(30);
    const std::time_t second = future(54);
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(slot(first, 60));
        slots.push(slot(second, 60));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    useAi(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "busy-owner");
    fixture.registerUser(owner, "busy@example.com", "Ольга");
    const Json a = integration::payloadOf(fixture.call(owner, "invite.create", createPayload("Созвон")));
    const Json b = integration::payloadOf(fixture.call(owner, "invite.create", createPayload("Обед")));
    const Json c = integration::payloadOf(fixture.call(owner, "invite.create", createPayload("Прогулка")));
    CHECK_EQ(a.getString("url"), std::string());  // SELFVOY_PUBLIC_URL не задан
    auto guest = sockets.open(fixture, "busy-guest");

    // Время, занятое гостем приглашения A, исчезает из B и не принимается в B.
    CHECK_EQ(fixture.call(guest, "invite.respond",
                          respondPayload(a.getString("token"), "accept", selfvoy::isoUtc(first), "Пётр"))
                 .getString("type"),
             std::string("ok"));
    const Json bView = integration::payloadOf(fixture.call(guest, "invite.get", tokenPayload(b.getString("token"))));
    CHECK_EQ(bView.get("invite").get("slots").items().size(), std::size_t(1));
    CHECK_EQ(bView.get("invite").get("slots").items().front().getString("start"), selfvoy::isoUtc(second));
    const Json taken = fixture.call(guest, "invite.respond",
                                    respondPayload(b.getString("token"), "accept", selfvoy::isoUtc(first), "Анна"));
    CHECK_EQ(taken.getString("code"), std::string("conflict"));
    CHECK(taken.getString("message").find("занято") != std::string::npos);

    // Отказ: приглашение закрыто, владельцу — уведомление invite.declined.
    Json decline = respondPayload(b.getString("token"), "decline", "", "Анна");
    decline.set("comment", Json("Уезжаю"));
    const Json declined = fixture.call(guest, "invite.respond", decline);
    CHECK_EQ(integration::payloadOf(declined).get("invite").getString("status"), std::string("declined"));
    bool declineNotified = false;
    const Json notifications = integration::payloadOf(fixture.call(owner, "notifications.list")).get("notifications");
    for (const auto& item : notifications.items()) {
        if (item.getString("kind") == "invite.declined") {
            declineNotified = true;
            CHECK_EQ(item.get("payload").getString("comment"), std::string("Уезжаю"));
        }
    }
    CHECK(declineNotified);

    // Отмена: только владелец и только открытое.
    const long long cId = c.get("invite").getInt("id");
    Json byId = Json::object();
    byId.set("invite_id", Json(cId));
    auto stranger = sockets.open(fixture, "busy-stranger");
    fixture.registerUser(stranger, "stranger@example.com", "Чужой");
    CHECK_EQ(fixture.call(stranger, "invite.cancel", byId).getString("code"), std::string("not_found"));
    const Json cancelled = fixture.call(owner, "invite.cancel", byId);
    CHECK_EQ(integration::payloadOf(cancelled).get("invite").getString("status"), std::string("cancelled"));
    CHECK_EQ(fixture.call(owner, "invite.cancel", byId).getString("code"), std::string("conflict"));
    CHECK_EQ(fixture.call(guest, "invite.respond",
                          respondPayload(c.getString("token"), "accept", selfvoy::isoUtc(second), "Пётр"))
                 .getString("code"),
             std::string("conflict"));
    CHECK_EQ(integration::payloadOf(fixture.call(guest, "invite.get", tokenPayload(c.getString("token"))))
                 .get("invite")
                 .getString("status"),
             std::string("cancelled"));
    // Чужие приглашения в список не попадают.
    CHECK_EQ(integration::payloadOf(fixture.call(stranger, "invite.list")).get("invites").items().size(),
             std::size_t(0));
}

TEST(invite_validation_limits_and_upstream) {
    using namespace invite_test;
    Fixture fixture;  // AI недоступен
    admin_test::Sockets sockets;
    auto anonymous = sockets.open(fixture, "inv-anon");
    CHECK_EQ(fixture.call(anonymous, "invite.create", createPayload("Кофе")).getString("code"),
             std::string("unauthorized"));
    CHECK_EQ(fixture.call(anonymous, "invite.list").getString("code"), std::string("unauthorized"));
    auto owner = sockets.open(fixture, "inv-owner");
    fixture.registerUser(owner, "limits@example.com", "Лимит");
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload("  ")).getString("code"), std::string("bad_request"));
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload("Строка\nс переводом")).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload(std::string(121, 'x'))).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload("Кофе", 10)).getString("code"),
             std::string("bad_request"));
    Json wide = createPayload("Кофе");
    wide.set("window_hours", Json(400LL));
    CHECK_EQ(fixture.call(owner, "invite.create", wide).getString("code"), std::string("bad_request"));
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload("Кофе")).getString("code"),
             std::string("upstream_error"));

    // Пустой ответ AI — conflict «нет свободных окон».
    FakeAi empty([] { return std::string("{\"slots\":[]}"); });
    useAi(fixture, empty.url());
    auto again = sockets.open(fixture, "inv-owner-2");
    Json login = Json::object();
    login.set("email", Json("limits@example.com"));
    login.set("password", Json("selfvoy1234"));
    CHECK_EQ(fixture.call(again, "auth.login", login).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(again, "invite.create", createPayload("Кофе")).getString("code"), std::string("conflict"));

    // Лимит ответов гостя с одного IP.
    std::string lastCode;
    for (int i = 0; i <= selfvoy::InviteManager::kRespondPerMinute; ++i) {
        lastCode = fixture.call(anonymous, "invite.respond",
                                respondPayload(std::string(43, 'B'), "accept", selfvoy::isoUtc(future(30)), "Гость"))
                       .getString("code");
    }
    CHECK_EQ(lastCode, std::string("forbidden"));
}

TEST(invite_effective_status_and_open_limit) {
    using namespace invite_test;
    selfvoy::InviteRecord record;
    record.status = "open";
    record.expiresAt = selfvoy::isoUtc(std::time(nullptr) - 60);
    CHECK_EQ(selfvoy::InviteManager::effectiveStatus(record, std::time(nullptr)), std::string("expired"));
    record.expiresAt = selfvoy::isoUtc(std::time(nullptr) + 3600);
    CHECK_EQ(selfvoy::InviteManager::effectiveStatus(record, std::time(nullptr)), std::string("open"));
    record.status = "declined";
    CHECK_EQ(selfvoy::InviteManager::effectiveStatus(record, std::time(nullptr)), std::string("declined"));
    CHECK_EQ(selfvoy::InviteManager::tokenHash("bad token with spaces and 43 symbols long.."), std::string());

    // Не больше kMaxOpenInvites открытых приглашений у владельца.
    const std::time_t start = future(30);
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(slot(start, 30));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    useAi(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "open-limit");
    fixture.registerUser(owner, "many@example.com", "Много");
    for (int i = 0; i < selfvoy::InviteManager::kMaxOpenInvites; ++i) {
        CHECK_EQ(fixture.call(owner, "invite.create", createPayload("Встреча " + std::to_string(i), 30))
                     .getString("type"),
                 std::string("ok"));
    }
    CHECK_EQ(fixture.call(owner, "invite.create", createPayload("Лишняя", 30)).getString("code"),
             std::string("forbidden"));
}

TEST(session_limit_key_ipv6_prefix) {
    // IPv6 — сеть /64 (смена адреса внутри своей сети не обходит лимиты).
    CHECK_EQ(selfvoy::Session::limitKey("2001:db8:1:2:aaaa:bbbb:cccc:dddd"), std::string("2001:db8:1:2::/64"));
    CHECK_EQ(selfvoy::Session::limitKey("2001:db8:1:2::1"), std::string("2001:db8:1:2::/64"));
    CHECK_EQ(selfvoy::Session::ipOf("2001:db8:1:2::ffff:55000"), std::string("2001:db8:1:2::/64"));
    // IPv4 и IPv4-mapped — как адрес IPv4.
    CHECK_EQ(selfvoy::Session::limitKey("203.0.113.7"), std::string("203.0.113.7"));
    CHECK_EQ(selfvoy::Session::limitKey("::ffff:203.0.113.7"), std::string("203.0.113.7"));
    CHECK_EQ(selfvoy::Session::ipOf("203.0.113.7:4242"), std::string("203.0.113.7"));
    // Не адрес — без изменений.
    CHECK_EQ(selfvoy::Session::limitKey("unix-socket"), std::string("unix-socket"));
}

TEST(booking_mailbox_key_normalized) {
    using selfvoy::BookingManager;
    CHECK_EQ(BookingManager::mailboxKey("Anna.Smith+tutor@GMail.com"), std::string("annasmith@gmail.com"));
    CHECK_EQ(BookingManager::mailboxKey("a.b@googlemail.com"), std::string("ab@gmail.com"));
    CHECK_EQ(BookingManager::mailboxKey("Ivan+x@Yandex.ru"), std::string("ivan@yandex.ru"));
    CHECK_EQ(BookingManager::mailboxKey("i.van@yandex.ru"), std::string("i.van@yandex.ru"));
}

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

// ------------------------------------------------------ страницы записи (booking.*)
namespace booking_test {

using invite_test::FakeAi;
using invite_test::future;

// Слот AI с местной датой владельца day.
Json daySlot(std::time_t start, int minutes, const std::string& day) {
    Json json = invite_test::slot(start, minutes);
    json.set("day", Json(day));
    return json;
}

Json pagePayload(const std::string& title) {
    Json payload = Json::object();
    payload.set("title", Json(title));
    payload.set("description", Json("Разбор вашего проекта"));
    payload.set("duration_minutes", Json(60LL));
    payload.set("days_ahead", Json(14LL));
    payload.set("max_per_day", Json(2LL));
    payload.set("notice_hours", Json(1LL));
    payload.set("max_series", Json(3LL));
    return payload;
}

Json slugPayload(const std::string& slug) {
    Json payload = Json::object();
    payload.set("slug", Json(slug));
    return payload;
}

Json bookPayload(const std::string& slug, std::time_t start, const std::string& name, long long count = 1) {
    Json payload = slugPayload(slug);
    payload.set("start", Json(selfvoy::isoUtc(start)));
    payload.set("guest_name", Json(name));
    if (count != 1) payload.set("count", Json(count));
    return payload;
}

Json tokenPayload(const std::string& token) { return invite_test::tokenPayload(token); }

// Письма гостям уходят из фоновой очереди — ждём, пока ящик dev перестанет меняться.
std::vector<selfvoy::MailNotice> settledOutbox() {
    std::size_t last = selfvoy::devOutbox().size();
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::size_t now = selfvoy::devOutbox().size();
        if (now == last && i >= 2) break;
        last = now;
    }
    return selfvoy::devOutbox();
}

// Ответ поддельного AI на POST /v1/agent/series: повторы ровно через неделю
// (пояс UTC), местное время — как в UTC.
std::string seriesBody(const std::string& request) {
    const Json body = Json::parse(request.substr(request.find("\r\n\r\n") + 4));
    std::time_t start = 0;
    selfvoy::parseIsoUtc(body.getString("start"), start);
    const long long minutes = body.getInt("duration_minutes", 60);
    Json occurrences = Json::array();
    for (long long k = 0; k < body.getInt("count", 1); ++k) {
        const std::time_t at = start + k * 7 * 86400;
        Json item = Json::object();
        item.set("start", Json(selfvoy::isoUtc(at)));
        item.set("end", Json(selfvoy::isoUtc(at + minutes * 60)));
        item.set("day", Json(selfvoy::isoUtc(at).substr(0, 10)));
        item.set("local", Json(selfvoy::isoUtc(at).substr(0, 16)));
        occurrences.push(item);
    }
    Json answer = Json::object();
    answer.set("occurrences", occurrences);
    return answer.dump();
}

bool hasSlot(const Json& page, std::time_t start) {
    const Json slots = page.get("slots");  // get() возвращает копию: держим её до конца цикла
    for (const auto& slot : slots.items()) {
        if (slot.getString("start") == selfvoy::isoUtc(start)) return true;
    }
    return false;
}

bool isBusy(Fixture& fixture, long long ownerId, std::time_t start) {
    for (const auto& [from, to] : fixture.server->memory().busy(ownerId)) {
        if (from == start && to == start + 3600) return true;
    }
    return false;
}

int countTasks(Fixture& fixture, const std::shared_ptr<selfvoy::Session>& owner, const std::string& title) {
    int count = 0;
    const Json tasks = integration::payloadOf(fixture.call(owner, "tasks.list")).get("tasks");
    for (const auto& task : tasks.items()) {
        if (task.getString("title") == title) ++count;
    }
    return count;
}

int countNotifications(Fixture& fixture, const std::shared_ptr<selfvoy::Session>& owner, const std::string& kind) {
    int count = 0;
    const Json list = integration::payloadOf(fixture.call(owner, "notifications.list")).get("notifications");
    for (const auto& item : list.items()) {
        if (item.getString("kind") == kind) ++count;
    }
    return count;
}

}  // namespace booking_test

TEST(booking_page_book_reschedule_cancel) {
    using namespace booking_test;
    const std::time_t first = future(26);
    const std::time_t second = first + 2 * 3600;
    const std::time_t third = first + 4 * 3600;
    const std::string day = selfvoy::isoUtc(first).substr(0, 10);
    // AI предлагает и лишнее (прошлое, чужая длительность, пересечение) — сервер отбрасывает.
    FakeAi* self = nullptr;
    FakeAi ai([&] {
        // Серия — отдельный эндпоинт; запрос уже записан в requests (под тем же мьютексом).
        if (self && self->requests.back().find("POST /v1/agent/series") != std::string::npos) {
            return seriesBody(self->requests.back());
        }
        Json slots = Json::array();
        slots.push(daySlot(third, 60, day));
        slots.push(daySlot(first, 60, day));
        slots.push(daySlot(std::time(nullptr) - 7200, 60, day));
        slots.push(daySlot(second, 45, day));
        slots.push(daySlot(second, 60, day));
        slots.push(daySlot(first + 1800, 60, day));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    self = &ai;
    Fixture fixture;
    invite_test::useAi(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "booking-owner");
    fixture.registerUser(owner, "coach@example.com", "Ольга");
    const long long ownerId = fixture.server->database().db().findUserByEmail("coach@example.com")->id;

    // Проверка полей страницы.
    Json bad = pagePayload("");
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", bad).getString("code"), std::string("bad_request"));
    bad = pagePayload("Консультация");
    bad.set("duration_minutes", Json(10LL));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", bad).getString("code"), std::string("bad_request"));
    bad = pagePayload("Консультация");
    bad.set("max_series", Json(13LL));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", bad).getString("code"), std::string("bad_request"));
    bad = pagePayload("Консультация");
    bad.set("weekends", Json("да"));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", bad).getString("code"), std::string("bad_request"));

    const Json created = fixture.call(owner, "booking.pageCreate", pagePayload("Консультация"));
    CHECK_EQ(created.getString("type"), std::string("ok"));
    const Json page = integration::payloadOf(created).get("page");
    const std::string slug = page.getString("slug");
    CHECK(selfvoy::BookingManager::validSlug(slug));
    CHECK_EQ(page.getString("url"), "https://selfvoy.example.com/#/b/" + slug);
    CHECK_EQ(page.getString("status"), std::string("active"));
    CHECK_EQ(page.getInt("max_series"), 3LL);

    // Гость без входа видит свободное время, но не email владельца.
    auto guest = sockets.open(fixture, "booking-guest");
    CHECK_EQ(fixture.call(guest, "booking.get", slugPayload("unknown000")).getString("code"),
             std::string("not_found"));
    CHECK_EQ(fixture.call(guest, "booking.get", slugPayload("../etc")).getString("code"), std::string("not_found"));
    const Json seen = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page");
    CHECK_EQ(seen.getString("owner_name"), std::string("Ольга"));
    CHECK_EQ(seen.getString("title"), std::string("Консультация"));
    CHECK(seen.dump().find("coach@example.com") == std::string::npos);
    CHECK_EQ(seen.get("slots").items().size(), std::size_t(3));
    CHECK(hasSlot(seen, first) && hasSlot(seen, second) && hasSlot(seen, third));
    CHECK_EQ(seen.getString("ref").size(), std::size_t(8));
    {
        std::lock_guard<std::mutex> lock(ai.mutex);
        const auto request = std::find_if(ai.requests.begin(), ai.requests.end(), [](const std::string& r) {
            return r.find("POST /v1/agent/slots") != std::string::npos;
        });
        CHECK(request != ai.requests.end());
        if (request != ai.requests.end()) {
            CHECK(request->find("\"window_hours\":336") != std::string::npos);
            CHECK(request->find("\"lead_minutes\":60") != std::string::npos);
            CHECK(request->find("\"weekends\":false") != std::string::npos);
        }
    }

    // Запись: неверные данные, затем серия из двух встреч.
    // Время не из списка — тот же conflict, что и «занято»: по ответу нельзя
    // узнать личное расписание владельца.
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, first + 600, "Пётр")).getString("code"),
             std::string("conflict"));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, first, " ")).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, first, "Пётр", 4)).getString("code"),
             std::string("bad_request"));
    Json withEmail = bookPayload(slug, first, "Пётр", 2);
    withEmail.set("guest_email", Json("petr@example.com"));
    withEmail.set("comment", Json("Про стартап"));
    const Json booked = fixture.call(guest, "booking.book", withEmail);
    CHECK_EQ(booked.getString("type"), std::string("ok"));
    if (booked.getString("type") != "ok") {
        std::cout << "  booking.book: " << booked.dump() << "\n";
        return;
    }
    const std::string token = integration::payloadOf(booked).getString("token");
    CHECK_EQ(token.size(), std::size_t(43));
    CHECK_EQ(integration::payloadOf(booked).getString("url"), "https://selfvoy.example.com/#/m/" + token);
    const Json bookedView = integration::payloadOf(booked).get("booking");
    CHECK_EQ(bookedView.get("occurrences").items().size(), std::size_t(2));
    CHECK_EQ(bookedView.get("occurrences").at(1).getString("start"), selfvoy::isoUtc(first + 7 * 86400));
    CHECK_EQ(bookedView.get("occurrences").at(1).getString("local"), selfvoy::isoUtc(first + 7 * 86400).substr(0, 16));
    CHECK(isBusy(fixture, ownerId, first));
    CHECK(isBusy(fixture, ownerId, first + 7 * 86400));
    CHECK_EQ(countTasks(fixture, owner, "Запись: Консультация — Пётр"), 2);
    CHECK_EQ(countNotifications(fixture, owner, "booking.created"), 1);
    // В БД только хэш токена; повторная запись на занятое время — conflict.
    const auto stored = fixture.server->database().db().findBookingByToken(selfvoy::BookingManager::tokenHash(token));
    CHECK(stored.has_value());
    CHECK(stored && stored->manageHash != token);
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, first, "Анна")).getString("code"),
             std::string("conflict"));

    // Второй гость занимает second — лимит 2 записи в день скрывает third.
    const Json other = fixture.call(guest, "booking.book", bookPayload(slug, second, "Анна"));
    CHECK_EQ(other.getString("type"), std::string("ok"));
    const Json afterLimit = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page");
    CHECK_EQ(afterLimit.get("slots").items().size(), std::size_t(0));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, third, "Иван")).getString("code"),
             std::string("conflict"));

    // Ссылка управления: перенос ближайшей встречи на third (свой день не мешает лимиту).
    CHECK_EQ(fixture.call(guest, "booking.manage", tokenPayload(std::string(43, 'A'))).getString("code"),
             std::string("not_found"));
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token))).get("booking");
    CHECK_EQ(managed.getString("guest_name"), std::string("Пётр"));
    CHECK(managed.getBool("can_change"));
    CHECK(managed.getBool("can_cancel"));
    CHECK(managed.getBool("can_reschedule"));
    CHECK_EQ(managed.getInt("reschedules_left"), 3LL);
    CHECK(hasSlot(managed, third));
    Json move = tokenPayload(token);
    move.set("start", Json(selfvoy::isoUtc(third)));
    const Json moved = fixture.call(guest, "booking.reschedule", move);
    CHECK_EQ(moved.getString("type"), std::string("ok"));
    const Json movedView = integration::payloadOf(moved).get("booking");
    CHECK_EQ(movedView.get("occurrences").at(0).getString("start"), selfvoy::isoUtc(third));
    CHECK_EQ(movedView.getInt("reschedules_left"), 2LL);
    CHECK(!isBusy(fixture, ownerId, first));
    CHECK(isBusy(fixture, ownerId, third));
    CHECK_EQ(countTasks(fixture, owner, "Запись: Консультация — Пётр"), 2);
    CHECK_EQ(countNotifications(fixture, owner, "booking.rescheduled"), 1);
    // На занятое время перенести нельзя.
    move.set("start", Json(selfvoy::isoUtc(second)));
    CHECK_EQ(fixture.call(guest, "booking.reschedule", move).getString("code"), std::string("conflict"));

    // Отмена второй встречи серии, затем всех оставшихся.
    Json cancelOne = tokenPayload(token);
    cancelOne.set("occurrence", Json(selfvoy::isoUtc(first + 7 * 86400)));
    const Json oneCancelled = fixture.call(guest, "booking.cancel", cancelOne);
    CHECK_EQ(oneCancelled.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(oneCancelled).get("booking").getString("status"), std::string("active"));
    CHECK(!isBusy(fixture, ownerId, first + 7 * 86400));
    CHECK_EQ(countTasks(fixture, owner, "Запись: Консультация — Пётр"), 1);
    Json cancelAll = tokenPayload(token);
    cancelAll.set("reason", Json("заболел"));
    const Json allCancelled = fixture.call(guest, "booking.cancel", cancelAll);
    const Json cancelledView = integration::payloadOf(allCancelled).get("booking");
    CHECK_EQ(cancelledView.getString("status"), std::string("cancelled"));
    CHECK_EQ(cancelledView.getString("cancelled_by"), std::string("guest"));
    CHECK(!cancelledView.getBool("can_change"));
    CHECK(!isBusy(fixture, ownerId, third));
    CHECK_EQ(countTasks(fixture, owner, "Запись: Консультация — Пётр"), 0);
    CHECK_EQ(countNotifications(fixture, owner, "booking.cancelled"), 2);
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(token)).getString("code"), std::string("conflict"));

    // Письма гостю: подтверждение со ссылкой управления (имя гостя в письмо не попадает).
    {
        int created = 0, cancelledMail = 0, rescheduledMail = 0;
        for (const auto& notice : settledOutbox()) {
            if (notice.to != "petr@example.com") continue;
            if (notice.kind == "booking.created") {
                ++created;
                CHECK_EQ(notice.linkUrl, "https://selfvoy.example.com/#/m/" + token);
                CHECK_EQ(notice.lines.size(), std::size_t(2));
                // Тема — только текст сервера; название страницы — в теле письма.
                CHECK(notice.subject.find("Консультация") == std::string::npos);
                CHECK(notice.note.find("Консультация") != std::string::npos);
                CHECK(notice.title.find("Пётр") == std::string::npos && notice.lead.find("Пётр") == std::string::npos);
            }
            if (notice.kind == "booking.rescheduled") ++rescheduledMail;
            if (notice.kind == "booking.cancelled") ++cancelledMail;
        }
        CHECK_EQ(created, 1);
        CHECK_EQ(rescheduledMail, 1);
        CHECK_EQ(cancelledMail, 2);
    }

    // Владелец: список записей и отмена записи Анны.
    const Json bookings = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    CHECK_EQ(bookings.items().size(), std::size_t(2));
    long long annaId = 0;
    for (const auto& item : bookings.items()) {
        CHECK_EQ(item.getString("page_title"), std::string("Консультация"));
        if (item.getString("guest_name") == "Анна") annaId = item.getInt("id");
        if (item.getString("guest_name") == "Пётр") {
            CHECK_EQ(item.getString("guest_email"), std::string("petr@example.com"));
        }
    }
    auto stranger = sockets.open(fixture, "booking-stranger");
    fixture.registerUser(stranger, "stranger@example.com", "Чужой");
    Json byBooking = Json::object();
    byBooking.set("booking_id", Json(annaId));
    CHECK_EQ(fixture.call(stranger, "booking.ownerCancel", byBooking).getString("code"), std::string("not_found"));
    const Json ownerCancelled = fixture.call(owner, "booking.ownerCancel", byBooking);
    CHECK_EQ(integration::payloadOf(ownerCancelled).get("booking").getString("cancelled_by"), std::string("owner"));
    CHECK(!isBusy(fixture, ownerId, second));

    // Потерянная ссылка: новая уходит письмом, старая перестаёт работать; чужой email —
    // тот же ответ, но без письма.
    Json withMail = bookPayload(slug, third, "Ира");
    withMail.set("guest_email", Json("ira@example.com"));
    const std::string iraToken = integration::payloadOf(fixture.call(guest, "booking.book", withMail)).getString("token");
    CHECK_EQ(iraToken.size(), std::size_t(43));
    Json resend = slugPayload(slug);
    resend.set("email", Json("IRA@example.com"));
    CHECK(integration::payloadOf(fixture.call(guest, "booking.resendLink", resend)).getBool("sent"));
    std::string newLink;
    for (const auto& notice : settledOutbox()) {
        if (notice.to == "ira@example.com" && notice.kind == "booking.link") newLink = notice.linkUrl;
    }
    CHECK(!newLink.empty());
    const std::string newToken = newLink.substr(newLink.rfind('/') + 1);
    CHECK(newToken != iraToken);
    CHECK_EQ(fixture.call(guest, "booking.manage", tokenPayload(iraToken)).getString("code"), std::string("not_found"));
    CHECK_EQ(fixture.call(guest, "booking.manage", tokenPayload(newToken)).getString("type"), std::string("ok"));
    resend.set("email", Json("nobody@example.com"));
    const std::size_t before = settledOutbox().size();
    CHECK(integration::payloadOf(fixture.call(guest, "booking.resendLink", resend)).getBool("sent"));
    CHECK_EQ(settledOutbox().size(), before);
    resend.set("email", Json("не-почта"));
    CHECK_EQ(fixture.call(guest, "booking.resendLink", resend).getString("code"), std::string("bad_request"));

    // Пауза: страница видна, но записаться нельзя; удаление — ссылка перестаёт работать.
    Json pause = Json::object();
    pause.set("page_id", Json(page.getInt("id")));
    pause.set("status", Json("paused"));
    CHECK_EQ(fixture.call(stranger, "booking.pageUpdate", pause).getString("code"), std::string("not_found"));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", pause).getString("type"), std::string("ok"));
    const Json paused = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page");
    CHECK_EQ(paused.getString("status"), std::string("paused"));
    CHECK_EQ(paused.get("slots").items().size(), std::size_t(0));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, third, "Иван")).getString("code"),
             std::string("conflict"));
    // На паузе гость не переносит, но отменить может.
    {
        const Json view = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(newToken))).get("booking");
        CHECK(view.getBool("can_cancel"));
        CHECK(!view.getBool("can_reschedule"));
        CHECK(!view.getBool("can_change"));
    }
    Json remove = Json::object();
    remove.set("page_id", Json(page.getInt("id")));
    CHECK_EQ(fixture.call(owner, "booking.pageDelete", remove).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(guest, "booking.get", slugPayload(slug)).getString("code"), std::string("not_found"));
    // Страница удалена — сделанная запись остаётся, отменить её гость по-прежнему может.
    {
        const Json view = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(newToken))).get("booking");
        CHECK(view.getBool("can_cancel"));
        CHECK(!view.getBool("can_reschedule"));
        CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(newToken)).getString("type"), std::string("ok"));
    }
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "booking.pageList")).get("pages").items().size(),
             std::size_t(0));
    // Методы владельца без входа недоступны.
    CHECK_EQ(fixture.call(guest, "booking.pageList").getString("code"), std::string("unauthorized"));
}

TEST(booking_ai_unavailable) {
    using namespace booking_test;
    Fixture fixture;  // AI-сервис недоступен (порт 1)
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "booking-owner");
    fixture.registerUser(owner, "coach2@example.com", "Ольга");
    const Json created = fixture.call(owner, "booking.pageCreate", pagePayload("Созвон"));
    const std::string slug = integration::payloadOf(created).get("page").getString("slug");
    CHECK_EQ(integration::payloadOf(created).get("page").getString("url"), std::string());
    auto guest = sockets.open(fixture, "booking-guest");
    const Json seen = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page");
    CHECK(seen.getBool("slots_unavailable"));
    CHECK_EQ(seen.get("slots").items().size(), std::size_t(0));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, future(30), "Пётр")).getString("code"),
             std::string("upstream_error"));
    // Заблокированный владелец гостей не принимает (страница не служит каналом рассылки).
    const long long ownerId = fixture.server->database().db().findUserByEmail("coach2@example.com")->id;
    CHECK(fixture.server->database().db().setUserActive(ownerId, false).ok);
    CHECK_EQ(fixture.call(guest, "booking.get", slugPayload(slug)).getString("code"), std::string("not_found"));
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, future(30), "Пётр")).getString("code"),
             std::string("not_found"));
    CHECK(fixture.server->database().db().setUserActive(ownerId, true).ok);
    CHECK_EQ(fixture.call(guest, "booking.get", slugPayload(slug)).getString("type"), std::string("ok"));
}

// За доверенным прокси адрес клиента берётся из X-Forwarded-For (правый не-прокси),
// от остальных заголовок игнорируется — иначе лимиты гостей обходились бы подделкой.
TEST(session_forwarded_for_trusted_proxy) {
    const auto shake = [](const std::string& peer, const std::string& trusted, const std::string& xff) {
        selfvoy::Config config;
        config.trustedProxies = trusted;
        int fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        selfvoy::Session session(fds[1], "xff", peer, config);
        const std::string request = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                                    "X-Forwarded-For: " + xff + "\r\n\r\n";
        CHECK(::send(fds[0], request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
        std::string error;
        CHECK(session.performHandshake(error));
        ::close(fds[0]);
        return session.remoteIp();
    };
    CHECK_EQ(shake("10.0.0.5:4000", "10.0.0.5", "1.2.3.4, 203.0.113.7"), std::string("203.0.113.7"));
    CHECK_EQ(shake("10.0.0.5:4000", "10.0.0.5, 10.0.0.6", "203.0.113.7, 10.0.0.6"), std::string("203.0.113.7"));
    CHECK_EQ(shake("198.51.100.1:4000", "10.0.0.5", "203.0.113.7"), std::string("198.51.100.1"));  // не прокси
    CHECK_EQ(shake("10.0.0.5:4000", "", "203.0.113.7"), std::string("10.0.0.5"));  // прокси не задан
    CHECK_EQ(shake("10.0.0.5:4000", "10.0.0.5", "<script>"), std::string("10.0.0.5"));  // мусор
}

// Одноразовые секреты: второе использование резервного кода 2FA и OAuth state не проходит
// (иначе два параллельных запроса с одним кодом оба считались бы верными).
TEST(one_time_secrets_single_use) {
    auto db = selfvoy::makeEmbeddedDatabase("");
    long long userId = 0;
    CHECK(db->createUser("once@example.com", "argon2id$x", "Раз", userId).ok);
    selfvoy::RecoveryCodeRecord code;
    code.id = "11111111-1111-1111-1111-111111111111";
    code.userId = userId;
    code.codeHash = std::string(64, 'a');
    CHECK(db->replaceRecoveryCodes(userId, {code}).ok);
    CHECK(db->useRecoveryCode(code.id).ok);
    CHECK(!db->useRecoveryCode(code.id).ok);
    selfvoy::OauthStateRecord state;
    state.state = "state-once";
    state.userId = userId;
    state.provider = "google";
    state.expiresAt = "2999-01-01T00:00:00Z";
    CHECK(db->createOauthState(state).ok);
    CHECK(db->useOauthState("state-once").ok);
    CHECK(!db->useOauthState("state-once").ok);
}

// Встроенная БД: id не переиспользуются — иначе task_id старой записи указал бы
// на задачу другого пользователя (и отмена записи удалила бы чужую задачу).
TEST(embedded_ids_not_reused) {
    auto db = selfvoy::makeEmbeddedDatabase("");
    long long anna = 0, boris = 0;
    CHECK(db->createUser("ids-anna@example.com", "argon2id$x", "Анна", anna).ok);
    CHECK(db->createUser("ids-boris@example.com", "argon2id$x", "Борис", boris).ok);
    selfvoy::TaskRecord task;
    task.userId = anna;
    task.title = "встреча";
    long long first = 0;
    CHECK(db->createTask(task, first).ok);
    CHECK(db->deleteTask(first).ok);
    task.userId = boris;
    long long second = 0;
    CHECK(db->createTask(task, second).ok);
    CHECK(second > first);
}

// Блокировка расписания — на владельца: чужой владелец не ждёт, тот же — ждёт release().
TEST(schedule_lock_per_owner) {
    selfvoy::DatabaseManager database(selfvoy::makeEmbeddedDatabase(""));
    selfvoy::ScheduleLock locks(database);
    auto held = locks.acquire(1);
    CHECK(held.ok());
    const auto started = std::chrono::steady_clock::now();
    std::thread other([&] { auto guard = locks.acquire(2); CHECK(guard.ok()); });
    other.join();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    std::atomic<bool> got{false};
    std::thread same([&] { auto guard = locks.acquire(1); got = guard.ok(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!got.load());
    held.release();
    same.join();
    CHECK(got.load());
}

// Ссылку, которая приходит только письмом, нельзя менять, если письма не уходят.
TEST(mail_provider_delivers) {
    selfvoy::Config config;
    config.mailDriver = "log";
    CHECK(!selfvoy::makeMailProvider(config)->delivers());
    config.mailDriver = "dev";
    config.devMode = true;
    CHECK(selfvoy::makeMailProvider(config)->delivers());
    config.mailDriver = "smtp";
    CHECK(selfvoy::makeMailProvider(config)->delivers());
}

TEST(referral_register_and_verify) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto host = sockets.open(fixture, "ref-host");
    fixture.registerUser(host, "host@example.com", "Ирина");
    const Json info = integration::payloadOf(fixture.call(host, "referral.get"));
    const std::string code = info.getString("code");
    CHECK_EQ(code.size(), std::size_t(8));
    CHECK_EQ(info.getInt("joined"), 0LL);
    // Код стабилен между запросами.
    CHECK_EQ(integration::payloadOf(fixture.call(host, "referral.get")).getString("code"), code);

    auto newcomer = sockets.open(fixture, "ref-new");
    Json payload = Json::object();
    payload.set("email", Json("friend@example.com"));
    payload.set("password", Json("selfvoy1234"));
    payload.set("display_name", Json("Саша"));
    std::string lower = code;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    payload.set("ref", Json(lower));
    payload.set("ref_source", Json("booking"));
    const Json registered = fixture.call(newcomer, "auth.register", payload);
    CHECK_EQ(registered.getString("type"), std::string("ok"));
    Json before = integration::payloadOf(fixture.call(host, "referral.get"));
    CHECK_EQ(before.getInt("joined"), 1LL);
    CHECK_EQ(before.getInt("verified"), 0LL);
    CHECK_EQ(before.get("referrals").at(0).getString("display_name"), std::string("Саша"));
    CHECK_EQ(before.get("referrals").at(0).getString("source"), std::string("booking"));
    CHECK(before.dump().find("friend@example.com") == std::string::npos);

    Json verify = Json::object();
    verify.set("email", Json("friend@example.com"));
    verify.set("code", Json(integration::payloadOf(registered).getString("email_code")));
    CHECK_EQ(fixture.call(newcomer, "auth.verifyEmail", verify).getString("type"), std::string("ok"));
    const Json after = integration::payloadOf(fixture.call(host, "referral.get"));
    CHECK_EQ(after.getInt("verified"), 1LL);
    CHECK(after.get("referrals").at(0).getBool("verified"));
    CHECK_EQ(booking_test::countNotifications(fixture, host, "referral.joined"), 1);

    // Неверный код регистрацию не ломает и никому не засчитывается.
    auto other = sockets.open(fixture, "ref-other");
    payload.set("email", Json("other@example.com"));
    payload.set("ref", Json("ZZZZZZZZ"));
    CHECK_EQ(fixture.call(other, "auth.register", payload).getString("type"), std::string("ok"));
    payload.set("email", Json("third@example.com"));
    payload.set("ref", Json("<script>"));
    CHECK_EQ(fixture.call(other, "auth.register", payload).getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(fixture.call(host, "referral.get")).getInt("joined"), 1LL);
    CHECK_EQ(fixture.call(other, "referral.get").getString("code"), std::string("unauthorized"));
}

// ============================================================ рост (S1a)
// https, тариф Pro, служебное состояние, шифр токена ссылки управления, Telegram-бот.
namespace growth_test {

#ifdef SELFVOY_HAVE_OPENSSL
// Локальный TLS-сервер с самоподписанным сертификатом на 127.0.0.1 (SAN IP):
// принимает accepts соединений, на запрос отвечает «200 ok» и рвёт соединение
// без close_notify (клиент должен остановиться по Content-Length).
struct TlsServer {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    SSL_CTX* ctx = nullptr;
    int listener = -1;
    int port = 0;
    std::string certPath;
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> requests;

    explicit TlsServer(int accepts) {
        key = EVP_EC_gen("P-256");
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("selfvoy-test"),
                                   -1, -1, 0);
        X509_set_issuer_name(cert, name);
        X509V3_CTX v3{};
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        for (const auto& [nid, value] : {std::make_pair(NID_subject_alt_name, "IP:127.0.0.1"),
                                         std::make_pair(NID_basic_constraints, "critical,CA:TRUE")}) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, value);
            X509_add_ext(cert, ext, -1);
            X509_EXTENSION_free(ext);
        }
        X509_sign(cert, key, EVP_sha256());
        certPath = "/tmp/selfvoy-test-ca-" + std::to_string(::getpid()) + ".pem";
        if (FILE* file = std::fopen(certPath.c_str(), "w")) {
            PEM_write_X509(file, cert);
            std::fclose(file);
        }
        ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(ctx, cert);
        SSL_CTX_use_PrivateKey(ctx, key);

        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(listener, 4);
        socklen_t len = sizeof addr;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread = std::thread([this, accepts] {
            for (int i = 0; i < accepts; ++i) {
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) return;
                SSL* ssl = SSL_new(ctx);
                SSL_set_fd(ssl, fd);
                if (SSL_accept(ssl) == 1) {
                    std::string request;
                    char chunk[2048];
                    while (request.find("\r\n\r\n") == std::string::npos) {
                        const int n = SSL_read(ssl, chunk, sizeof chunk);
                        if (n <= 0) break;
                        request.append(chunk, static_cast<std::size_t>(n));
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        requests.push_back(request);
                    }
                    const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
                    SSL_write(ssl, answer.data(), static_cast<int>(answer.size()));
                }
                SSL_free(ssl);  // без SSL_shutdown: соединение рвётся без close_notify
                ::close(fd);
            }
        });
    }

    ~TlsServer() {
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        if (thread.joinable()) thread.join();
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(key);
        std::remove(certPath.c_str());
    }
};
#endif

// Поддельный Bot API: записывает вызовы, отдаёт апдейты, 403 для «заблокировавших» чатов.
struct FakeTelegram : selfvoy::TelegramBot::Transport {
    struct Call {
        std::string method;
        Json body;
    };
    std::mutex mutex;
    std::vector<Call> calls;
    std::vector<long long> blocked;
    Json updates = Json::array();

    selfvoy::net::HttpResponse call(const std::string& method, const Json& body, int, const std::atomic<bool>*,
                                    std::string&) override {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back({method, body});
        selfvoy::net::HttpResponse response;
        response.status = 200;
        if (method == "sendMessage" &&
            std::find(blocked.begin(), blocked.end(), body.getInt("chat_id")) != blocked.end()) {
            response.status = 403;
            response.body = R"({"ok":false,"error_code":403,"description":"Forbidden: bot was blocked by the user"})";
            return response;
        }
        if (method == "getUpdates") {
            Json answer = Json::object();
            answer.set("ok", Json(true));
            answer.set("result", updates);
            updates = Json::array();
            response.body = answer.dump();
            return response;
        }
        response.body = R"({"ok":true,"result":true})";
        return response;
    }

    // Тексты sendMessage в чат (по порядку).
    std::vector<std::string> texts(long long chatId) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const auto& item : calls) {
            if (item.method == "sendMessage" && item.body.getInt("chat_id") == chatId) {
                out.push_back(item.body.getString("text"));
            }
        }
        return out;
    }
    std::string last(long long chatId) {
        const auto all = texts(chatId);
        return all.empty() ? std::string() : all.back();
    }
    std::vector<Call> of(const std::string& method) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Call> out;
        for (const auto& item : calls) {
            if (item.method == method) out.push_back(item);
        }
        return out;
    }
};

Json textUpdate(long long chatId, const std::string& text, const std::string& type = "private",
                const std::string& username = "tg_user") {
    Json chat = Json::object();
    chat.set("id", Json(chatId));
    chat.set("type", Json(type));
    Json from = Json::object();
    from.set("id", Json(chatId));
    from.set("username", Json(username));
    Json message = Json::object();
    message.set("message_id", Json(1LL));
    message.set("chat", chat);
    message.set("from", from);
    message.set("text", Json(text));
    Json update = Json::object();
    update.set("update_id", Json(1LL));
    update.set("message", message);
    return update;
}

Json callbackUpdate(long long chatId, const std::string& data, long long messageId = 77) {
    Json chat = Json::object();
    chat.set("id", Json(chatId));
    chat.set("type", Json("private"));
    Json message = Json::object();
    message.set("message_id", Json(messageId));
    message.set("chat", chat);
    Json callback = Json::object();
    callback.set("id", Json("cb-" + std::to_string(chatId)));
    callback.set("data", Json(data));
    callback.set("message", message);
    Json update = Json::object();
    update.set("update_id", Json(2LL));
    update.set("callback_query", callback);
    return update;
}

selfvoy::Config botConfig() {
    selfvoy::Config config;
    config.jwtSecret = "telegram-test-secret-0123456789abcdef";
    config.telegramBotToken = "1:test-token";
    config.telegramBotUsername = "selfvoy_test_bot";
    config.telegramPoll = false;
    return config;
}

std::string newToken() {
    std::string token = selfvoy::crypto::base64UrlEncode(selfvoy::crypto::randomBytes(32));
    while (!token.empty() && token.back() == '=') token.pop_back();
    return token;
}

// Запись с одной встречей через day суток (встроенная БД).
long long makeBooking(selfvoy::IDatabase& db, long long ownerId, const std::string& token, double days = 1.0) {
    selfvoy::BookingPageRecord page;
    page.ownerId = ownerId;
    page.slug = "tgpage" + std::to_string(ownerId % 10000);
    while (page.slug.size() < 10) page.slug += "0";
    page.title = "Разбор";
    long long pageId = 0;
    db.createBookingPage(page, pageId);
    const std::time_t start = std::time(nullptr) + static_cast<std::time_t>(days * 86400);
    Json occurrence = Json::object();
    occurrence.set("start", Json(selfvoy::isoUtc(start)));
    occurrence.set("end", Json(selfvoy::isoUtc(start + 3600)));
    occurrence.set("day", Json(selfvoy::isoUtc(start).substr(0, 10)));
    occurrence.set("local", Json(selfvoy::isoUtc(start).substr(0, 16)));
    occurrence.set("status", Json("active"));
    Json occurrences = Json::array();
    occurrences.push(occurrence);
    selfvoy::BookingRecord booking;
    booking.pageId = pageId;
    booking.ownerId = ownerId;
    booking.manageHash = selfvoy::BookingManager::tokenHash(token);
    booking.guestName = "Гость";
    booking.occurrences = occurrences;
    long long id = 0;
    db.createBooking(booking, id);
    return id;
}

bool hasChat(selfvoy::IDatabase& db, long long bookingId, long long chatId) {
    const auto chats = db.bookingTelegramChats(bookingId);
    return std::find(chats.begin(), chats.end(), chatId) != chats.end();
}

// Сервер Fixture с включённым ботом (без опроса) и поддельным Bot API.
FakeTelegram* enableBot(Fixture& fixture, const std::string& aiUrl = "", const std::string& publicUrl = "") {
    fixture.config.telegramBotToken = "1:test-token";
    fixture.config.telegramBotUsername = "selfvoy_test_bot";
    fixture.config.telegramPoll = false;
    if (!aiUrl.empty()) fixture.config.aiServiceUrl = aiUrl;
    fixture.config.publicUrl = publicUrl;
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    if (!fixture.server->start(error)) std::cout << "  не удалось перезапустить сервер: " << error << "\n";
    auto fake = std::make_unique<FakeTelegram>();
    FakeTelegram* raw = fake.get();
    fixture.server->telegram().setTransport(std::move(fake));
    return raw;
}

// payload из ссылки https://t.me/<бот>?start=<payload>.
std::string startPayload(const std::string& url) {
    const auto at = url.find("?start=");
    return at == std::string::npos ? std::string() : url.substr(at + 7);
}

}  // namespace growth_test

TEST(net_https_local_tls_and_ca) {
#ifdef SELFVOY_HAVE_OPENSSL
    CHECK(selfvoy::tls::available());
    growth_test::TlsServer server(2);
    const std::string url = "https://127.0.0.1:" + std::to_string(server.port) + "/v1/ping";
    // Свой корневой сертификат (SELFVOY_CA_FILE): сертификат и IP сверяются, ответ читается
    // по Content-Length, хотя сервер не прислал close_notify.
    selfvoy::net::setCaFile(server.certPath);
    std::string error;
    const auto ok = selfvoy::net::httpRequest("GET", url, {}, "", 5000, error);
    CHECK_EQ(error, std::string());
    CHECK_EQ(ok.status, 200);
    CHECK_EQ(ok.body, std::string("ok"));
    // Без своего CA самоподписанный сертификат не принимается.
    selfvoy::net::setCaFile("");
    error.clear();
    const auto rejected = selfvoy::net::httpRequest("GET", url, {}, "", 5000, error);
    CHECK_EQ(rejected.status, 0);
    CHECK(error.find("сертификат") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        CHECK_EQ(server.requests.size(), std::size_t(1));
        if (!server.requests.empty()) {
            CHECK(server.requests[0].find("GET /v1/ping HTTP/1.1") == 0);
            CHECK(server.requests[0].find("Host: 127.0.0.1:" + std::to_string(server.port)) != std::string::npos);
        }
    }
    // Нечитаемый файл CA — понятная ошибка до рукопожатия.
    selfvoy::net::setCaFile("/nonexistent/ca.pem");
    error.clear();
    selfvoy::net::httpRequest("GET", url, {}, "", 2000, error);
    CHECK(error.find("SELFVOY_CA_FILE") != std::string::npos);
    selfvoy::net::setCaFile("");
#else
    // Сборка без TLS: https-запрос — понятная ошибка, а не открытый текст.
    CHECK(!selfvoy::tls::available());
    std::string error;
    const auto response = selfvoy::net::httpRequest("GET", "https://127.0.0.1:1/", {}, "", 1000, error);
    CHECK_EQ(response.status, 0);
    CHECK(error.find("сборка без TLS") != std::string::npos);
#endif
}

// Долгий запрос (long polling) прерывается флагом остановки, а не ждёт таймаута.
TEST(net_request_cancel) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::listen(listener, 4);  // соединение примет ядро; ответа не будет
    socklen_t len = sizeof addr;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    std::atomic<bool> cancel{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const auto response = selfvoy::net::httpRequest(
        "POST", "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/getUpdates", {}, "{}", 20000, error,
        &cancel);
    stopper.join();
    CHECK_EQ(response.status, 0);
    CHECK_EQ(error, std::string("запрос прерван"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    ::close(listener);
}

TEST(telegram_signed_payloads) {
    selfvoy::Config config = growth_test::botConfig();
    selfvoy::DatabaseManager database(selfvoy::makeEmbeddedDatabase(""));
    selfvoy::TelegramBot bot(database, config, std::make_unique<growth_test::FakeTelegram>());
    const std::time_t now = std::time(nullptr);
    const auto safe = [](const std::string& payload) {
        return payload.size() <= 64 && std::all_of(payload.begin(), payload.end(), [](unsigned char ch) {
                   return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
               });
    };

    // Владелец: подпись, срок 15 минут, подмена любой части.
    const std::string owner = bot.ownerPayload(123456789012LL, now + 900);
    CHECK(safe(owner));
    CHECK_EQ(bot.sign("x").size(), std::size_t(22));
    long long userId = 0;
    CHECK(bot.verifyOwner(owner, now, userId));
    CHECK_EQ(userId, 123456789012LL);
    CHECK(!bot.verifyOwner(owner, now + 901, userId));  // истекла
    CHECK_EQ(userId, 0LL);
    CHECK(!bot.verifyOwner(bot.ownerPayload(5, now + 7200), now, userId));  // срок дальше, чем выдаёт сервер
    std::string tampered = owner;
    tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
    CHECK(!bot.verifyOwner(tampered, now, userId));
    std::string otherUser = owner;
    otherUser.replace(2, 1, otherUser[2] == '1' ? "2" : "1");
    CHECK(!bot.verifyOwner(otherUser, now, userId));
    const std::string longer = owner.substr(0, owner.rfind('_')) + "0" + owner.substr(owner.rfind('_'));
    CHECK(!bot.verifyOwner(longer, now, userId));  // продлённый срок — другая подпись
    CHECK(!bot.verifyOwner("o_1_2_short", now, userId));
    CHECK(!bot.verifyOwner("g_" + owner.substr(2), now, userId));
    CHECK(!bot.verifyOwner("", now, userId));

    // Гость: подпись над manage_hash — перевыпуск ссылки управления её отменяет.
    const std::string hash(64, 'a');
    const std::string guest = bot.guestPayload(42, hash);
    CHECK(safe(guest));
    CHECK(bot.verifyGuest(guest, 42, hash));
    CHECK(!bot.verifyGuest(guest, 43, hash));
    CHECK(!bot.verifyGuest(guest, 42, std::string(64, 'b')));
    CHECK(!bot.verifyGuest(guest, 42, ""));
    long long bookingId = 0;
    std::string sig;
    CHECK(selfvoy::TelegramBot::parseGuest(guest, bookingId, sig));
    CHECK_EQ(bookingId, 42LL);
    CHECK(!selfvoy::TelegramBot::parseGuest("g_042_" + sig, bookingId, sig));
    CHECK(!selfvoy::TelegramBot::parseGuest("g_-1_" + std::string(22, 'a'), bookingId, sig));
    CHECK(!selfvoy::TelegramBot::parseGuest("g_1_" + std::string(21, 'a') + "!", bookingId, sig));
    CHECK_EQ(bot.linkUrl(guest), "https://t.me/selfvoy_test_bot?start=" + guest);

    // Другой секрет сервера — подписи не совпадают.
    selfvoy::Config other = config;
    other.jwtSecret = "another-secret-0123456789abcdef0123";
    selfvoy::TelegramBot stranger(database, other, std::make_unique<growth_test::FakeTelegram>());
    CHECK(!stranger.verifyOwner(owner, now, userId));
    CHECK(!stranger.verifyGuest(guest, 42, hash));
}

TEST(telegram_bot_updates_with_fake_transport) {
    using namespace growth_test;
    selfvoy::Config config = botConfig();
    selfvoy::DatabaseManager database(selfvoy::makeEmbeddedDatabase(""));
    auto& db = database.db();
    auto fakeOwned = std::make_unique<FakeTelegram>();
    FakeTelegram* fake = fakeOwned.get();
    selfvoy::TelegramBot bot(database, config, std::move(fakeOwned));
    long long anna = 0, boris = 0;
    CHECK(db.createUser("tg-anna@example.com", "argon2id$x", "Анна", anna).ok);
    CHECK(db.createUser("tg-boris@example.com", "argon2id$x", "Борис", boris).ok);
    const std::time_t now = std::time(nullptr);

    // Владелец: /start o_… → чат привязан, ответ с подсказкой /stop.
    bot.handleUpdate(textUpdate(1001, "/start " + bot.ownerPayload(anna, now + 900), "private", "anna_tg"));
    bot.flush();
    CHECK_EQ(db.findUserById(anna)->telegramChatId, 1001LL);
    CHECK_EQ(db.findUserById(anna)->telegramUsername, std::string("anna_tg"));
    CHECK(fake->last(1001).find("Готово: сюда будут приходить новые записи") == 0);
    // Тот же чат другим аккаунтом — перепривязка к новому.
    bot.handleUpdate(textUpdate(1001, "/start@selfvoy_test_bot " + bot.ownerPayload(boris, now + 900)));
    bot.flush();
    CHECK_EQ(db.findUserById(anna)->telegramChatId, 0LL);
    CHECK_EQ(db.findUserById(boris)->telegramChatId, 1001LL);
    // Просроченная/поддельная ссылка и группа — не привязывают.
    bot.handleUpdate(textUpdate(1002, "/start " + bot.ownerPayload(anna, now - 1)));
    bot.handleUpdate(textUpdate(1003, "/start " + bot.ownerPayload(anna, now + 900), "group"));
    std::string forged = bot.ownerPayload(anna, now + 900);
    forged.back() = forged.back() == 'x' ? 'y' : 'x';
    bot.handleUpdate(textUpdate(1004, "/start " + forged));
    bot.flush();
    CHECK_EQ(db.findUserById(anna)->telegramChatId, 0LL);
    CHECK(fake->last(1002).find("недействительна") != std::string::npos);
    CHECK(fake->texts(1003).empty());
    CHECK(fake->last(1004).find("недействительна") != std::string::npos);
    // Любой другой текст — справка.
    bot.handleUpdate(textUpdate(1005, "привет"));
    bot.flush();
    CHECK(fake->last(1005).find("/stop") != std::string::npos);

    // Гость: /start g_… → чат записи; не больше 3 чатов.
    const std::string token = newToken();
    const long long booking = makeBooking(db, anna, token);
    const auto record = db.findBooking(booking);
    CHECK(record.has_value());
    const std::string guest = bot.guestPayload(booking, record->manageHash);
    CHECK_EQ(bot.guestUrl(*record), "https://t.me/selfvoy_test_bot?start=" + guest);
    bot.handleUpdate(textUpdate(2002, "/start " + guest));
    bot.handleUpdate(textUpdate(2003, "/start " + bot.guestPayload(booking, std::string(64, 'f'))));
    bot.flush();
    CHECK(hasChat(db, booking, 2002));
    CHECK(!hasChat(db, booking, 2003));
    CHECK(fake->last(2002).find("Готово") == 0);
    CHECK(fake->last(2003).find("недействительна") != std::string::npos);
    bot.handleUpdate(textUpdate(2004, "/start " + guest));
    bot.handleUpdate(textUpdate(2005, "/start " + guest));
    bot.handleUpdate(textUpdate(2006, "/start " + guest));
    bot.handleUpdate(textUpdate(2002, "/start " + guest));  // повтор тем же чатом — не новый
    bot.flush();
    CHECK_EQ(db.bookingTelegramChats(booking).size(), std::size_t(3));
    CHECK(!hasChat(db, booking, 2006));
    CHECK(fake->last(2006).find("уже подключено 3 чата") != std::string::npos);

    // «Приду»: из чужого чата — отказ; из привязанного — хук BookingManager (здесь нет — «недоступно»).
    const std::string cf = "cf:" + std::to_string(booking) + ":" + std::to_string(static_cast<long long>(now + 86400));
    bot.handleUpdate(callbackUpdate(3003, cf));
    bot.handleUpdate(callbackUpdate(2004, cf));
    bot.flush();
    const auto answers = fake->of("answerCallbackQuery");
    CHECK_EQ(answers.size(), std::size_t(2));
    if (answers.size() == 2) {
        CHECK(answers[0].body.getString("text").find("не для этого чата") != std::string::npos);
        CHECK(answers[1].body.getString("text").find("недоступно") != std::string::npos);
    }
    CHECK(fake->of("editMessageReplyMarkup").empty());

    // /stop — чат отвязан от записи (и от пользователя).
    bot.handleUpdate(textUpdate(2002, "/stop"));
    bot.handleUpdate(textUpdate(1001, "/STOP"));
    bot.flush();
    CHECK(!hasChat(db, booking, 2002));
    CHECK_EQ(db.findUserById(boris)->telegramChatId, 0LL);
    CHECK(fake->last(2002).find("больше не пишу") != std::string::npos);

    // 403 (бот заблокирован) — чат отвязывается везде.
    {
        std::lock_guard<std::mutex> lock(fake->mutex);
        fake->blocked.push_back(2004);
    }
    bot.sendMessage(2004, "напоминание");
    bot.flush();
    CHECK(!hasChat(db, booking, 2004));
    CHECK(hasChat(db, booking, 2005));

    // Опрос: апдейты обрабатываются, смещение — в server_state.
    {
        std::lock_guard<std::mutex> lock(fake->mutex);
        Json update = textUpdate(2005, "/stop");
        update.set("update_id", Json(41LL));
        fake->updates.push(update);
    }
    CHECK(bot.pollOnce(0));
    bot.flush();
    CHECK_EQ(db.getState(selfvoy::TelegramBot::kOffsetKey), std::string("42"));
    CHECK(!hasChat(db, booking, 2005));
    CHECK(bot.pollOnce(0));
    const auto polls = fake->of("getUpdates");
    CHECK_EQ(polls.size(), std::size_t(2));
    if (polls.size() == 2) {
        CHECK_EQ(polls[1].body.getInt("offset"), 42LL);
        CHECK_EQ(polls[1].body.get("allowed_updates").size(), std::size_t(2));
    }
    bot.stop();
    bot.sendMessage(2005, "после остановки");  // не принимается и не падает
}

TEST(server_state_and_manage_token_field) {
    auto db = selfvoy::makeEmbeddedDatabase("");
    CHECK_EQ(db->getState("telegram.offset"), std::string());
    CHECK(db->setState("telegram.offset", "10").ok);
    CHECK(db->setState("telegram.offset", "11").ok);
    CHECK(db->setState("other", "x").ok);
    CHECK_EQ(db->getState("telegram.offset"), std::string("11"));
    CHECK_EQ(db->getState("other"), std::string("x"));
    CHECK(db->tryLeaderLock(selfvoy::TelegramBot::kLeaderLockKey));

    // Шифр поля: тот же ключ и AAD — исходный текст; иначе — отказ.
    const std::string key = selfvoy::crypto::sha256("k");
    const std::string sealed = selfvoy::crypto::sealField(key, "token-value", "selfvoy-manage-token");
    CHECK(!sealed.empty());
    std::string plain;
    CHECK(selfvoy::crypto::openField(key, sealed, "selfvoy-manage-token", plain));
    CHECK_EQ(plain, std::string("token-value"));
    CHECK(!selfvoy::crypto::openField(key, sealed, "selfvoy-2fa", plain));
    CHECK(!selfvoy::crypto::openField(selfvoy::crypto::sha256("other"), sealed, "selfvoy-manage-token", plain));
    CHECK(!selfvoy::crypto::openField(key, "", "selfvoy-manage-token", plain));
}

TEST(plan_computation_and_admin_set_plan) {
    selfvoy::UserRecord user;
    const std::time_t now = std::time(nullptr);
    CHECK(!selfvoy::plan::isPro(user, now));
    user.planUntil = selfvoy::isoUtc(now + 3600);
    CHECK(selfvoy::plan::isPro(user, now));
    CHECK(!selfvoy::plan::isPro(user, now + 7200));
    user.planUntil = "мусор";
    CHECK(!selfvoy::plan::isPro(user, now));

    ::setenv("SELFVOY_ADMIN_EMAILS", "planadmin@example.com", 1);
    Fixture fixture;
    ::unsetenv("SELFVOY_ADMIN_EMAILS");
    admin_test::Sockets sockets;
    auto admin = sockets.open(fixture, "plan-admin");
    fixture.registerUser(admin, "planadmin@example.com", "Админ");
    auto member = sockets.open(fixture, "plan-user");
    const Json login = fixture.registerUser(member, "planuser@example.com", "Вера");
    const long long memberId = admin_test::userIdOf(login);
    // В каждом user — plan; бета (enforced=false): Pro-функции открыты и без Pro.
    const Json plan = login.get("payload").get("user").get("plan");
    CHECK_EQ(plan.getString("name"), std::string("free"));
    CHECK_EQ(plan.getString("until"), std::string());
    CHECK_EQ(plan.getBool("enforced", true), false);
    CHECK(fixture.server->proAllowed(memberId));

    Json payload = admin_test::byId(memberId);
    payload.set("days", Json(30LL));
    CHECK_EQ(fixture.call(member, "admin.setPlan", payload).getString("code"), std::string("forbidden"));
    const Json granted = fixture.call(admin, "admin.setPlan", payload);
    CHECK_EQ(granted.getString("type"), std::string("ok"));
    const Json grantedPlan = integration::payloadOf(granted).get("user").get("plan");
    CHECK_EQ(grantedPlan.getString("name"), std::string("pro"));
    std::time_t until = 0;
    CHECK(selfvoy::parseIsoUtc(grantedPlan.getString("until"), until));
    CHECK(std::llabs(static_cast<long long>(until - (now + 30 * 86400))) < 120);
    CHECK_EQ(integration::payloadOf(fixture.call(member, "auth.me")).get("plan").getString("name"), std::string("pro"));

    for (const Json& bad : {Json(-1LL), Json(3651LL), Json(1.5), Json("30")}) {
        Json wrong = admin_test::byId(memberId);
        wrong.set("days", bad);
        CHECK_EQ(fixture.call(admin, "admin.setPlan", wrong).getString("code"), std::string("bad_request"));
    }
    Json missing = admin_test::byId(999999);
    missing.set("days", Json(1LL));
    CHECK_EQ(fixture.call(admin, "admin.setPlan", missing).getString("code"), std::string("not_found"));
    bool audited = false;
    for (const auto& entry : fixture.server->database().db().listAudit(memberId, 20)) {
        if (entry.kind == "admin_plan_changed" && entry.detail.getInt("days") == 30) audited = true;
    }
    CHECK(audited);

    // Принудительные тарифы: без Pro — нельзя, с Pro — можно; enforced виден в user.plan.
    fixture.config.plansEnforced = true;
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    CHECK(fixture.server->start(error));
    CHECK(fixture.server->proAllowed(memberId));
    const long long adminId = fixture.server->database().db().findUserByEmail("planadmin@example.com")->id;
    CHECK(!fixture.server->proAllowed(adminId));
    auto relogged = sockets.open(fixture, "plan-relogin");
    const Json again = fixture.call(relogged, "auth.login", admin_test::loginPayload("planuser@example.com"));
    CHECK_EQ(again.get("payload").get("user").get("plan").getBool("enforced"), true);
    // Сброс (days = 0) — снова Free.
    auto adminAgain = sockets.open(fixture, "plan-admin2");
    fixture.call(adminAgain, "auth.login", admin_test::loginPayload("planadmin@example.com"));
    Json reset = admin_test::byId(memberId);
    reset.set("days", Json(0LL));
    const Json cleared = integration::payloadOf(fixture.call(adminAgain, "admin.setPlan", reset)).get("user");
    CHECK_EQ(cleared.get("plan").getString("name"), std::string("free"));
    CHECK_EQ(cleared.get("plan").getString("until"), std::string());
    CHECK(!fixture.server->proAllowed(memberId));
    CHECK_EQ(std::string(selfvoy::protocol::code::kProRequired), std::string("pro_required"));
    selfvoy::plan::setEnforced(false);
}

TEST(telegram_protocol_and_owner_notifications) {
    using namespace growth_test;
    Fixture fixture;
    admin_test::Sockets sockets;
    // Бот не настроен: статус выключен, ссылка — bad_request.
    auto plain = sockets.open(fixture, "tg-plain");
    fixture.registerUser(plain, "tgplain@example.com", "Пётр");
    const Json off = integration::payloadOf(fixture.call(plain, "telegram.status"));
    CHECK_EQ(off.getBool("enabled", true), false);
    const Json noBot = fixture.call(plain, "telegram.link");
    CHECK_EQ(noBot.getString("code"), std::string("bad_request"));
    CHECK_EQ(noBot.getString("message"), std::string("Telegram-бот не настроен на сервере"));
    CHECK_EQ(integration::payloadOf(fixture.call(plain, "server.info")).getString("telegram_bot"), std::string());

    FakeTelegram* fake = enableBot(fixture);
    auto owner = sockets.open(fixture, "tg-owner");
    fixture.registerUser(owner, "tgowner@example.com", "Ольга");
    const long long ownerId = fixture.server->database().db().findUserByEmail("tgowner@example.com")->id;
    auto anonymous = sockets.open(fixture, "tg-anon");
    CHECK_EQ(integration::payloadOf(fixture.call(anonymous, "server.info")).getString("telegram_bot"),
             std::string("selfvoy_test_bot"));
    CHECK_EQ(fixture.call(anonymous, "telegram.status").getString("code"), std::string("unauthorized"));
    const Json status = integration::payloadOf(fixture.call(owner, "telegram.status"));
    CHECK(status.getBool("enabled"));
    CHECK_EQ(status.getString("bot_username"), std::string("selfvoy_test_bot"));
    CHECK_EQ(status.getBool("linked", true), false);
    const Json link = integration::payloadOf(fixture.call(owner, "telegram.link"));
    const std::string url = link.getString("url");
    CHECK(url.find("https://t.me/selfvoy_test_bot?start=o_" + std::to_string(ownerId) + "_") == 0);
    std::time_t expires = 0;
    CHECK(selfvoy::parseIsoUtc(link.getString("expires_at"), expires));
    CHECK(expires > std::time(nullptr) + 800 && expires <= std::time(nullptr) + 900);

    auto& bot = fixture.server->telegram();
    bot.handleUpdate(textUpdate(7001, "/start " + startPayload(url), "private", "olga"));
    bot.flush();
    const Json linked = integration::payloadOf(fixture.call(owner, "telegram.status"));
    CHECK(linked.getBool("linked"));
    CHECK_EQ(linked.getString("username"), std::string("olga"));

    // Уведомления владельцу: booking.*, task.due — в чат; прочие виды — нет.
    auto& notifications = fixture.server->notifications();
    notifications.create(ownerId, "booking.created", "Анна записалась на встречу", "Консультация");
    notifications.create(ownerId, "login.new", "Новый вход в Selfvoy", "127.0.0.1");
    notifications.create(ownerId, "task.due", "Напоминание", "Позвонить");
    bot.flush();
    auto texts = fake->texts(7001);
    CHECK_EQ(texts.size(), std::size_t(3));  // ответ на /start + два уведомления
    if (texts.size() == 3) {
        CHECK_EQ(texts[1], std::string("Анна записалась на встречу\nКонсультация"));
        CHECK_EQ(texts[2], std::string("Напоминание\nПозвонить"));
    }
    // Отключённый вид и prefs.notifications.telegram = false — не уходят.
    Json prefs = Json::object();
    Json notify = Json::object();
    Json muted = Json::array();
    muted.push(Json("booking.cancelled"));
    notify.set("muted_kinds", muted);
    prefs.set("notifications", notify);
    CHECK_EQ(fixture.call(owner, "prefs.set", prefs).getString("type"), std::string("ok"));
    notifications.create(ownerId, "booking.cancelled", "Отмена", "Консультация");
    notify.set("telegram", Json(false));
    prefs.set("notifications", notify);
    CHECK_EQ(fixture.call(owner, "prefs.set", prefs).getString("type"), std::string("ok"));
    notifications.create(ownerId, "booking.rescheduled", "Перенос", "Консультация");
    bot.flush();
    CHECK_EQ(fake->texts(7001).size(), std::size_t(3));

    const Json unlinked = integration::payloadOf(fixture.call(owner, "telegram.unlink"));
    CHECK_EQ(unlinked.getBool("linked", true), false);
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "telegram.status")).getBool("linked", true), false);
}

// Запись гостя: ссылка Telegram в ответе, письме и виде гостя; шифр токена ссылки
// управления; извещения гостю уходят и в привязанный чат.
TEST(telegram_booking_guest_channel) {
    using namespace booking_test;
    using namespace growth_test;
    const std::time_t first = future(30);
    const std::string day = selfvoy::isoUtc(first).substr(0, 10);
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(daySlot(first, 60, day));
        slots.push(daySlot(first + 7200, 60, day));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    FakeTelegram* fake = enableBot(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "tgb-owner");
    fixture.registerUser(owner, "tgcoach@example.com", "Ольга");
    const std::string slug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", pagePayload("Разбор"))).get("page").getString("slug");
    auto guest = sockets.open(fixture, "tgb-guest");
    Json payload = bookPayload(slug, first, "Пётр");
    payload.set("guest_email", Json("tg-guest@example.com"));
    selfvoy::devOutbox();
    const Json booked = integration::payloadOf(fixture.call(guest, "booking.book", payload));
    const std::string token = booked.getString("token");
    const std::string telegramUrl = booked.getString("telegram_url");
    CHECK(telegramUrl.find("https://t.me/selfvoy_test_bot?start=g_") == 0);
    CHECK_EQ(booked.get("booking").getString("telegram_url"), telegramUrl);
    CHECK_EQ(booked.get("booking").getBool("telegram_linked", true), false);

    // Письмо-подтверждение — со строкой «Напоминания в Telegram: <url>».
    bool mailed = false;
    for (const auto& mail : settledOutbox()) {
        if (mail.to != "tg-guest@example.com" || mail.kind != "booking.created") continue;
        mailed = std::find(mail.footer.begin(), mail.footer.end(), "Напоминания в Telegram: " + telegramUrl) !=
                 mail.footer.end();
    }
    CHECK(mailed);

    // Токен ссылки управления хранится шифром; хэш остаётся ключом поиска.
    auto& db = fixture.server->database().db();
    const auto stored = db.findBookingByToken(selfvoy::BookingManager::tokenHash(token));
    CHECK(stored.has_value());
    if (!stored) return;
    CHECK(!stored->manageTokenEnc.empty());
    CHECK(stored->manageTokenEnc.find(token) == std::string::npos);
    CHECK_EQ(fixture.server->bookings().manageTokenOf(*stored), token);
    selfvoy::BookingRecord legacy = *stored;
    legacy.manageTokenEnc.clear();
    CHECK_EQ(fixture.server->bookings().manageTokenOf(legacy), std::string());

    // Гость подключает Telegram по ссылке — вид гостя это показывает.
    auto& bot = fixture.server->telegram();
    bot.handleUpdate(textUpdate(8001, "/start " + startPayload(telegramUrl)));
    bot.flush();
    CHECK(fake->last(8001).find("Готово: напоминания о записи «Разбор» (Ольга)") == 0);
    CHECK(fake->last(8001).find("Ближайшая встреча:") != std::string::npos);
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token))).get("booking");
    CHECK(managed.getBool("telegram_linked"));
    CHECK_EQ(managed.getString("telegram_url"), telegramUrl);

    // Перевыпуск ссылки управления: шифр — новый токен, прежняя Telegram-ссылка недействительна.
    Json resend = slugPayload(slug);
    resend.set("email", Json("tg-guest@example.com"));
    CHECK_EQ(fixture.call(guest, "booking.resendLink", resend).getString("type"), std::string("ok"));
    fixture.server->bookings().flushMail();
    const auto reissued = db.findBooking(stored->id);
    CHECK(reissued && reissued->manageHash != stored->manageHash);
    if (reissued) {
        const std::string fresh = fixture.server->bookings().manageTokenOf(*reissued);
        CHECK_EQ(selfvoy::BookingManager::tokenHash(fresh), reissued->manageHash);
        CHECK(!bot.verifyGuest(startPayload(telegramUrl), reissued->id, reissued->manageHash));
        // Новая ссылка ушла и в Telegram-чат гостя (url-кнопкой: адрес https).
        bot.flush();
        const auto sent = fake->of("sendMessage");
        bool button = false;
        for (const auto& item : sent) {
            if (item.body.getInt("chat_id") != 8001) continue;
            const Json keyboard = item.body.get("reply_markup").get("inline_keyboard");
            if (keyboard.size() == 1 && keyboard.at(0).at(0).getString("url") ==
                                            "https://selfvoy.example.com/#/m/" + fresh) {
                button = true;
            }
        }
        CHECK(button);
        // Гость отменяет — извещение в чат.
        CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(fresh)).getString("type"), std::string("ok"));
        fixture.server->bookings().flushMail();
        bot.flush();
        CHECK(fake->last(8001).find("Запись отменена") == 0);
        CHECK(fake->last(8001).find("«Разбор» — Ольга") != std::string::npos);
        CHECK(fake->last(8001).find("Пётр") == std::string::npos);  // имя гостя в извещения не попадает
    }
}

#ifdef SELFVOY_WITH_LIBPQ
// PostgreSQL: состояние, тариф, Telegram-привязки и лидерство опроса (SELFVOY_TEST_PG_URL).
TEST(pg_growth_methods) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto first = selfvoy::makePostgresDatabase(url);
    auto second = selfvoy::makePostgresDatabase(url);
    std::string error;
    CHECK(first && first->connect(error));
    CHECK(second && second->connect(error));
    if (!first || !second) return;
    const std::string key = "test." + selfvoy::crypto::randomHex(4);
    CHECK_EQ(first->getState(key), std::string());
    CHECK(first->setState(key, "1").ok);
    CHECK(first->setState(key, "2").ok);
    CHECK_EQ(second->getState(key), std::string("2"));

    // Лидер: повторный вызов у держателя — true; другой процесс (соединение) — false,
    // после release — может взять.
    const long long lockKey = 0x53564F59544C00LL + (std::time(nullptr) % 200);
    CHECK(first->tryLeaderLock(lockKey));
    CHECK(first->tryLeaderLock(lockKey));
    CHECK(!second->tryLeaderLock(lockKey));
    first->releaseLeaderLock(lockKey);
    CHECK(second->tryLeaderLock(lockKey));
    CHECK(!first->tryLeaderLock(lockKey));
    second->releaseLeaderLock(lockKey);

    long long a = 0, b = 0;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    CHECK(first->createUser("pg-tg-a-" + suffix + "@example.com", "argon2id$x", "А", a).ok);
    CHECK(first->createUser("pg-tg-b-" + suffix + "@example.com", "argon2id$x", "Б", b).ok);
    const long long chat = 900000000000LL + (std::time(nullptr) % 100000);
    CHECK(first->setUserTelegram(a, chat, "anna").ok);
    CHECK_EQ(first->findUserById(a)->telegramChatId, chat);
    CHECK(first->setUserTelegram(b, chat, "boris").ok);  // перепривязка
    CHECK_EQ(first->findUserById(a)->telegramChatId, 0LL);
    CHECK_EQ(first->findUserById(b)->telegramUsername, std::string("boris"));
    CHECK(first->setUserPlanDays(b, 10).ok);
    CHECK(selfvoy::plan::isPro(*first->findUserById(b)));
    CHECK(first->setUserPlanDays(b, 0).ok);
    CHECK_EQ(first->findUserById(b)->planUntil, std::string());

    const std::string token = growth_test::newToken();
    const long long booking = growth_test::makeBooking(*first, a, token);
    CHECK(booking > 0);
    CHECK(first->linkBookingTelegram(booking, chat, 3));
    CHECK(first->linkBookingTelegram(booking, chat, 3));
    CHECK(first->linkBookingTelegram(booking, chat + 1, 3));
    CHECK(first->linkBookingTelegram(booking, chat + 2, 3));
    CHECK(!first->linkBookingTelegram(booking, chat + 3, 3));
    CHECK_EQ(first->bookingTelegramChats(booking).size(), std::size_t(3));
    CHECK(first->unlinkTelegramChat(chat).ok);
    CHECK_EQ(first->bookingTelegramChats(booking).size(), std::size_t(2));
    CHECK_EQ(first->findUserById(b)->telegramChatId, 0LL);

    auto record = first->findBooking(booking);
    CHECK(record && record->manageTokenEnc.empty());
    if (record) {
        record->manageTokenEnc = "sealed-value";
        CHECK(first->updateBooking(*record).ok);
        CHECK_EQ(first->findBooking(booking)->manageTokenEnc, std::string("sealed-value"));
        record->manageTokenEnc.clear();  // пустое не затирает
        CHECK(first->updateBooking(*record).ok);
        CHECK_EQ(first->findBooking(booking)->manageTokenEnc, std::string("sealed-value"));
    }
}

// PostgreSQL (S1b): поля страницы, напоминания (атомарный захват двумя соединениями),
// продление тарифа и награда за приглашённого с лимитом (SELFVOY_TEST_PG_URL).
TEST(pg_reminders_and_reward) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto first = selfvoy::makePostgresDatabase(url);
    auto second = selfvoy::makePostgresDatabase(url);
    std::string error;
    CHECK(first && first->connect(error));
    CHECK(second && second->connect(error));
    if (!first || !second) return;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    long long owner = 0;
    CHECK(first->createUser("pg-s1b-" + suffix + "@example.com", "argon2id$x", "Влад", owner).ok);

    // Поля страницы guest_reminders / hide_branding.
    selfvoy::BookingPageRecord page;
    page.ownerId = owner;
    page.slug = ("s1b" + suffix + "0000000").substr(0, 10);
    page.title = "Разбор";
    page.guestReminders = false;
    page.hideBranding = true;
    long long pageId = 0;
    CHECK(first->createBookingPage(page, pageId).ok);
    auto storedPage = first->findBookingPage(pageId);
    CHECK(storedPage && !storedPage->guestReminders && storedPage->hideBranding);
    if (storedPage) {
        storedPage->guestReminders = true;
        storedPage->hideBranding = false;
        CHECK(first->updateBookingPage(*storedPage).ok);
        storedPage = first->findBookingPage(pageId);
        CHECK(storedPage && storedPage->guestReminders && !storedPage->hideBranding);
    }

    // Напоминания: повтор не дублируется, удаление — только неотправленных.
    const long long booking = growth_test::makeBooking(*first, owner, growth_test::newToken());
    const std::time_t base = 946684800 + static_cast<std::time_t>(std::time(nullptr) % 100000) * 60;  // 2000 г.
    std::vector<selfvoy::BookingReminderRecord> rows;
    for (int i = 0; i < 20; ++i) {
        selfvoy::BookingReminderRecord row;
        row.bookingId = booking;
        row.ownerId = owner;
        row.occurrenceStart = selfvoy::isoUtc(base + i * 3600);
        row.kind = "2h";
        row.dueAt = selfvoy::isoUtc(base + i * 3600 - 7200);
        rows.push_back(row);
    }
    CHECK(first->addBookingReminders(rows).ok);
    CHECK(first->addBookingReminders(rows).ok);  // ON CONFLICT DO NOTHING
    CHECK_EQ(first->listBookingReminders(booking).size(), std::size_t(20));
    CHECK_EQ(first->listBookingReminders(booking).front().occurrenceStart, selfvoy::isoUtc(base));

    // Захват двумя соединениями параллельно: каждая строка — ровно одному.
    std::vector<long long> claimedA;
    std::vector<long long> claimedB;
    const std::string until = selfvoy::isoUtc(base + 20 * 3600);
    std::thread worker([&] {
        for (int i = 0; i < 20; ++i) {
            for (const auto& item : second->claimDueReminders(until, 2)) claimedB.push_back(item.id);
        }
    });
    for (int i = 0; i < 20; ++i) {
        for (const auto& item : first->claimDueReminders(until, 2)) claimedA.push_back(item.id);
    }
    worker.join();
    std::vector<long long> all = claimedA;
    all.insert(all.end(), claimedB.begin(), claimedB.end());
    std::sort(all.begin(), all.end());
    CHECK_EQ(all.size(), std::size_t(20));
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    CHECK(first->claimDueReminders(until, 100).empty());
    for (const auto& reminder : first->listBookingReminders(booking)) CHECK(!reminder.sentAt.empty());
    selfvoy::BookingReminderRecord fresh = rows.front();
    fresh.kind = "24h";
    fresh.dueAt = selfvoy::isoUtc(base - 86400);
    CHECK(first->addBookingReminders({fresh}).ok);
    CHECK_EQ(first->listBookingReminders(booking).size(), std::size_t(21));
    CHECK(first->deleteBookingReminders(booking, selfvoy::isoUtc(base)).ok);  // отправленная 2h остаётся
    CHECK_EQ(first->listBookingReminders(booking).size(), std::size_t(20));
    CHECK(first->deleteBookingReminders(booking, "").ok);
    CHECK_EQ(first->listBookingReminders(booking).size(), std::size_t(20));

    // Продление тарифа: greatest(now, plan_until) + days.
    const std::time_t now = std::time(nullptr);
    const auto untilOf = [&](long long id) {
        std::time_t at = 0;
        selfvoy::parseIsoUtc(first->findUserById(id)->planUntil, at);
        return at;
    };
    CHECK(first->extendUserPlanDays(owner, 10).ok);
    CHECK(std::llabs(static_cast<long long>(untilOf(owner) - (now + 10 * 86400))) < 120);
    CHECK(first->extendUserPlanDays(owner, 5).ok);
    CHECK(std::llabs(static_cast<long long>(untilOf(owner) - (now + 15 * 86400))) < 120);

    // Награда: один раз, пригласившему — не больше cap (здесь 2), приглашённым — всегда.
    long long referrer = 0;
    CHECK(first->createUser("pg-s1b-ref-" + suffix + "@example.com", "argon2id$x", "Рита", referrer).ok);
    int toReferrer = 0;
    for (int i = 0; i < 3; ++i) {
        long long friendId = 0;
        CHECK(first->createUser("pg-s1b-f" + std::to_string(i) + "-" + suffix + "@example.com", "argon2id$x", "Друг",
                                friendId).ok);
        CHECK(first->createReferral(referrer, friendId, "link").ok);
        bool rewarded = true;
        CHECK_EQ(first->rewardReferral(friendId, 30, 2, rewarded), 0LL);  // ещё не подтверждён
        CHECK_EQ(first->markReferralVerified(friendId), referrer);
        CHECK_EQ(first->rewardReferral(friendId, 30, 2, rewarded), referrer);
        if (rewarded) ++toReferrer;
        CHECK_EQ(second->rewardReferral(friendId, 30, 2, rewarded), 0LL);  // повтор
        CHECK(std::llabs(static_cast<long long>(untilOf(friendId) - (now + 30 * 86400))) < 120);
    }
    CHECK_EQ(toReferrer, 2);
    CHECK(std::llabs(static_cast<long long>(untilOf(referrer) - (now + 60 * 86400))) < 120);
    std::size_t rewardedRows = 0;
    for (const auto& referral : first->listReferrals(referrer, 10)) rewardedRows += referral.rewardedAt.empty() ? 0 : 1;
    CHECK_EQ(rewardedRows, std::size_t(3));
}
#endif

// ------------------------------------------- F6: пост «свободные окна» и ICS-подписка
namespace feed_test {

// Склеить сложенные строки RFC 5545 обратно (CRLF + пробел — продолжение).
std::string unfold(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 3, "\r\n ") == 0) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// Каждая физическая строка не длиннее 75 октетов, заканчивается CRLF и не начинается
// с продолжающего байта UTF-8 (символ не разорван).
bool foldedWell(const std::string& text) {
    if (text.size() < 2 || text.compare(text.size() - 2, 2, "\r\n") != 0) return false;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find("\r\n", start);
        if (end == std::string::npos) return false;
        if (end - start > 75) return false;
        if (text.find('\n', start) < end || text.find('\r', start) < end) return false;
        std::size_t first = start;
        if (start != 0 && first < end && text[first] == ' ') ++first;
        if (first < end && (static_cast<unsigned char>(text[first]) & 0xC0) == 0x80) return false;
        start = end + 2;
    }
    return true;
}

// Слот AI с местным временем владельца (UTC+3, как Europe/Moscow).
Json localSlot(std::time_t start, int minutes) {
    Json json = invite_test::slot(start, minutes);
    const std::string local = selfvoy::isoUtc(start + 3 * 3600).substr(0, 16);
    json.set("day", Json(local.substr(0, 10)));
    json.set("local", Json(local));
    return json;
}

std::string httpGet(Fixture& fixture, const std::string& path, selfvoy::net::HttpResponse& response,
                    const std::string& method = "GET") {
    std::string error;
    response = selfvoy::net::httpRequest(method, "http://127.0.0.1:" + std::to_string(fixture.server->port()) + path,
                                         {}, "", 3000, error);
    return error;
}

}  // namespace feed_test

TEST(ics_escape_fold_and_stamp) {
    using selfvoy::ics::escapeText;
    using selfvoy::ics::foldLine;
    CHECK_EQ(escapeText("a\\b;c,d"), std::string("a\\\\b\\;c\\,d"));
    CHECK_EQ(escapeText("строка 1\nстрока 2\r\nстрока 3\rконец"),
             std::string("строка 1\\nстрока 2\\nстрока 3\\nконец"));
    CHECK_EQ(escapeText(std::string("x\x01y\x7Fz\tw")), std::string("xyz\tw"));
    CHECK_EQ(escapeText("Иван: «привет»"), std::string("Иван: «привет»"));  // «:» в TEXT не экранируется

    CHECK_EQ(foldLine("SUMMARY:коротко"), std::string("SUMMARY:коротко\r\n"));
    CHECK_EQ(foldLine(""), std::string("\r\n"));
    const std::string ascii = "DESCRIPTION:" + std::string(200, 'a');
    const std::string foldedAscii = foldLine(ascii);
    CHECK(feed_test::foldedWell(foldedAscii));
    CHECK_EQ(foldedAscii.substr(0, 77), ascii.substr(0, 75) + "\r\n");
    CHECK_EQ(foldedAscii.substr(77, 1), std::string(" "));
    CHECK_EQ(feed_test::unfold(foldedAscii), ascii + "\r\n");
    // Кириллица (2 октета), «€» (3), эмодзи (4) и сдвиг на нечётную позицию: граница
    // строки не попадает внутрь символа, склейка возвращает исходную строку.
    for (const std::string& unit : {std::string("я"), std::string("€"), std::string("\xF0\x9F\x99\x82")}) {
        for (int shift = 0; shift < 4; ++shift) {
            std::string line = "SUMMARY:" + std::string(static_cast<std::size_t>(shift), 'x');
            for (int i = 0; i < 60; ++i) line += unit;
            const std::string folded = foldLine(line);
            CHECK(feed_test::foldedWell(folded));
            CHECK_EQ(feed_test::unfold(folded), line + "\r\n");
            CHECK(folded.find("\r\n ") != std::string::npos);
        }
    }
    CHECK_EQ(selfvoy::ics::utcStamp(0), std::string("19700101T000000Z"));
    std::time_t moment = 0;
    CHECK(selfvoy::parseIsoUtc("2026-10-21T07:30:05Z", moment));
    CHECK_EQ(selfvoy::ics::utcStamp(moment), std::string("20261021T073005Z"));
    CHECK_EQ(selfvoy::CalendarFeed::tokenHashFromPath("/ics/short.ics"), std::string());
    CHECK_EQ(selfvoy::CalendarFeed::tokenHashFromPath("/ics/" + std::string(43, '/') + ".ics"), std::string());
    CHECK_EQ(selfvoy::CalendarFeed::tokenHashFromPath("/ics/" + std::string(43, 'A') + ".ics").size(), std::size_t(64));
    CHECK_EQ(selfvoy::CalendarFeed::tokenHashFromPath("/ics/" + std::string(43, 'A') + ".ics?x=1"),
             selfvoy::CalendarFeed::tokenHashFromPath("/ics/" + std::string(43, 'A') + ".ics"));
}

TEST(slotspost_render_text) {
    namespace sp = selfvoy::slotspost;
    CHECK_EQ(sp::dayLabel("2026-10-19"), std::string("Пн, 19 окт"));
    CHECK_EQ(sp::dayLabel("2026-12-31"), std::string("Чт, 31 дек"));
    CHECK_EQ(sp::dayLabel("2026-13-01"), std::string());
    CHECK_EQ(sp::durationLabel(50), std::string("50 мин"));
    CHECK_EQ(sp::durationLabel(60), std::string("1 ч"));
    CHECK_EQ(sp::durationLabel(90), std::string("1 ч 30 мин"));
    CHECK_EQ(sp::moneyLabel(250000, "RUB"), std::string("2 500 ₽"));
    CHECK_EQ(sp::moneyLabel(123456789, "USD"), std::string("1 234 567,89 $"));
    CHECK_EQ(sp::moneyLabel(50000, "EUR"), std::string("500 €"));
    CHECK_EQ(sp::moneyLabel(0, "RUB"), std::string());

    sp::Input input;
    input.title = "Консультация";
    input.durationMinutes = 50;
    input.price = 250000;
    input.locals = {"2026-10-19T10:00", "2026-10-19T11:30", "2026-10-19T15:00", "2026-10-20T12:00"};
    input.url = "https://selfvoy.example.com/#/b/abcdefghij";
    CHECK_EQ(sp::render(input), std::string("Свободные окна: Консультация (50 мин, 2 500 ₽)\n"
                                            "\n"
                                            "Пн, 19 окт: 10:00, 11:30, 15:00\n"
                                            "Вт, 20 окт: 12:00\n"
                                            "\n"
                                            "Записаться: https://selfvoy.example.com/#/b/abcdefghij"));
    input.price = 0;  // цена не задана — в заголовке только длительность
    input.format = "vk";
    CHECK_EQ(sp::render(input), std::string("Свободные окна: Консультация (50 мин)\n"
                                            "\n"
                                            "Пн, 19 окт: 10:00, 11:30, 15:00\n"
                                            "Вт, 20 окт: 12:00\n"
                                            "\n"
                                            "Запись онлайн: https://selfvoy.example.com/#/b/abcdefghij"));
    input.format = "plain";
    CHECK_EQ(sp::render(input), std::string("Свободные окна: Консультация (50 мин)\n"
                                            "Пн, 19 окт: 10:00, 11:30, 15:00\n"
                                            "Вт, 20 окт: 12:00\n"
                                            "Записаться: https://selfvoy.example.com/#/b/abcdefghij"));
    // Больше 8 времён в дне — «и ещё N».
    input.format = "telegram";
    input.locals.clear();
    for (int hour = 8; hour < 19; ++hour) {
        input.locals.push_back("2026-10-19T" + std::to_string(hour / 10) + std::to_string(hour % 10) + ":00");
    }
    const std::string many = sp::render(input);
    CHECK(many.find("Пн, 19 окт: 08:00, 09:00, 10:00, 11:00, 12:00, 13:00, 14:00, 15:00 и ещё 3\n") !=
          std::string::npos);
    // Окон нет.
    input.locals.clear();
    CHECK_EQ(sp::render(input), std::string("Свободные окна: Консультация (50 мин)\n"
                                            "\n"
                                            "На ближайшие 7 дней свободных окон нет\n"
                                            "\n"
                                            "Записаться: https://selfvoy.example.com/#/b/abcdefghij"));
    input.days = 3;
    CHECK(sp::render(input).find("На ближайшие 3 дня свободных окон нет") != std::string::npos);
    input.days = 1;
    CHECK(sp::render(input).find("На ближайший день свободных окон нет") != std::string::npos);
    input.days = 11;
    input.url.clear();
    CHECK_EQ(sp::render(input), std::string("Свободные окна: Консультация (50 мин)\n\n"
                                            "На ближайшие 11 дней свободных окон нет"));
}

TEST(booking_slots_post_flow) {
    using namespace booking_test;
    const std::time_t first = future(26);
    const std::time_t second = first + 2 * 3600;
    const std::time_t far = first + 10 * 86400;  // за пределами days=7
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(feed_test::localSlot(first, 60));
        slots.push(feed_test::localSlot(second, 60));
        slots.push(feed_test::localSlot(far, 60));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    invite_test::useAi(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "post-owner");
    fixture.registerUser(owner, "post@example.com", "Ольга");
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pagePayload("Консультация")))
                          .get("page");
    const long long pageId = page.getInt("id");
    const std::string slug = page.getString("slug");

    Json request = Json::object();
    request.set("page_id", Json(pageId));
    const Json posted = fixture.call(owner, "booking.slotsPost", request);
    CHECK_EQ(posted.getString("type"), std::string("ok"));
    const Json post = integration::payloadOf(posted);
    const std::string text = post.getString("text");
    CHECK_EQ(post.getInt("slots"), 2LL);
    CHECK_EQ(post.getString("url"), "https://selfvoy.example.com/#/b/" + slug);
    CHECK(text.rfind("Свободные окна: Консультация (1 ч)\n\n", 0) == 0);
    CHECK(text.find("Записаться: https://selfvoy.example.com/#/b/" + slug) != std::string::npos);
    const std::string firstLocal = selfvoy::isoUtc(first + 3 * 3600);
    const std::string secondLocal = selfvoy::isoUtc(second + 3 * 3600);
    CHECK(text.find(selfvoy::slotspost::dayLabel(firstLocal.substr(0, 10)) + ": " + firstLocal.substr(11, 5)) !=
          std::string::npos);
    CHECK(text.find(selfvoy::slotspost::dayLabel(selfvoy::isoUtc(far + 3 * 3600).substr(0, 10))) ==
          std::string::npos);

    // Занятое гостем окно в пост не попадает (та же логика, что booking.get).
    auto guest = sockets.open(fixture, "post-guest");
    CHECK_EQ(fixture.call(guest, "booking.book", bookPayload(slug, second, "Пётр")).getString("type"),
             std::string("ok"));
    const Json afterBook = integration::payloadOf(fixture.call(owner, "booking.slotsPost", request));
    CHECK_EQ(afterBook.getInt("slots"), 1LL);
    CHECK(afterBook.getString("text").find(secondLocal.substr(11, 5)) == std::string::npos ||
          secondLocal.substr(11, 5) == firstLocal.substr(11, 5));
    request.set("days", Json(14LL));
    request.set("format", Json("plain"));
    const Json wide = integration::payloadOf(fixture.call(owner, "booking.slotsPost", request));
    CHECK_EQ(wide.getInt("slots"), 2LL);
    CHECK(wide.getString("text").find("\n\n") == std::string::npos);

    // Проверка ввода и владельца.
    Json bad = request;
    bad.set("days", Json(15LL));
    CHECK_EQ(fixture.call(owner, "booking.slotsPost", bad).getString("code"), std::string("bad_request"));
    bad.set("days", Json(2.5));
    CHECK_EQ(fixture.call(owner, "booking.slotsPost", bad).getString("code"), std::string("bad_request"));
    bad = request;
    bad.set("format", Json("html"));
    CHECK_EQ(fixture.call(owner, "booking.slotsPost", bad).getString("code"), std::string("bad_request"));
    CHECK_EQ(fixture.call(guest, "booking.slotsPost", request).getString("code"), std::string("unauthorized"));
    auto stranger = sockets.open(fixture, "post-stranger");
    fixture.registerUser(stranger, "stranger@example.com", "Чужой");
    CHECK_EQ(fixture.call(stranger, "booking.slotsPost", request).getString("code"), std::string("not_found"));

    // Лимит — 30 постов в час на владельца.
    int limitedAt = 0;
    for (int i = 0; i < 40 && limitedAt == 0; ++i) {
        if (fixture.call(owner, "booking.slotsPost", request).getString("code") == "forbidden") limitedAt = i + 1;
    }
    CHECK(limitedAt > 0 && limitedAt <= selfvoy::BookingManager::kSlotsPostPerHour);
}

TEST(booking_slots_post_ai_unavailable) {
    using namespace booking_test;
    Fixture fixture;  // AI-сервис недоступен (порт 1)
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "post-owner");
    fixture.registerUser(owner, "post2@example.com", "Ольга");
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pagePayload("Созвон")))
                          .get("page");
    Json request = Json::object();
    request.set("page_id", Json(page.getInt("id")));
    CHECK_EQ(fixture.call(owner, "booking.slotsPost", request).getString("code"), std::string("upstream_error"));
}

TEST(calendar_feed_ics_over_http) {
    using namespace booking_test;
    const std::time_t first = future(26);
    FakeAi ai([&] {
        Json slots = Json::array();
        slots.push(feed_test::localSlot(first, 60));
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    fixture.config.serverPublicUrl = "https://api.example.com";
    invite_test::useAi(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "feed-owner");
    fixture.registerUser(owner, "feed@example.com", "Ольга");
    const long long ownerId = fixture.server->database().db().findUserByEmail("feed@example.com")->id;
    const std::string slug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", pagePayload("Разбор; проекта, v2")))
            .get("page")
            .getString("slug");
    auto guest = sockets.open(fixture, "feed-guest");
    Json book = bookPayload(slug, first, "Пётр");
    book.set("comment", Json("Про стартап, деньги; планы\nи команду"));
    const Json booked = fixture.call(guest, "booking.book", book);
    CHECK_EQ(booked.getString("type"), std::string("ok"));
    const auto ownBookings = fixture.server->database().db().listBookings(ownerId, 0, true, 10);
    CHECK_EQ(ownBookings.size(), std::size_t(1));
    const long long bookingId = ownBookings.empty() ? 0 : ownBookings.front().id;

    // Принятое приглашение тоже попадает в календарь, открытое — нет.
    selfvoy::InviteRecord invite;
    invite.ownerId = ownerId;
    invite.tokenHash = std::string(64, 'c');
    invite.topic = "Созвон";
    invite.expiresAt = selfvoy::isoUtc(std::time(nullptr) + 86400);
    long long inviteId = 0;
    CHECK(fixture.server->database().db().createInvite(invite, inviteId).ok);
    selfvoy::InviteRecord response;
    response.guestName = "Анна";
    response.chosen = Json::object();
    response.chosen.set("start", Json(selfvoy::isoUtc(first + 86400)));
    response.chosen.set("end", Json(selfvoy::isoUtc(first + 86400 + 1800)));
    CHECK(fixture.server->database().db().closeInvite(inviteId, "accepted", response));
    invite.tokenHash = std::string(64, 'd');
    long long openId = 0;
    CHECK(fixture.server->database().db().createInvite(invite, openId).ok);

    // До первой ссылки подписка выключена.
    Json feed = integration::payloadOf(fixture.call(owner, "calendar.feed"));
    CHECK(!feed.getBool("enabled", true));
    CHECK_EQ(feed.getString("path"), std::string());
    CHECK_EQ(fixture.call(guest, "calendar.feed").getString("code"), std::string("unauthorized"));

    const Json reset = integration::payloadOf(fixture.call(owner, "calendar.feedReset"));
    CHECK(reset.getBool("enabled"));
    const std::string path = reset.getString("path");
    CHECK_EQ(path.size(), std::string("/ics/").size() + 43 + 4);
    CHECK(path.rfind("/ics/", 0) == 0);
    CHECK_EQ(reset.getString("url"), "https://api.example.com" + path);
    // В БД — только хэш токена; повторный calendar.feed ссылку не показывает.
    const std::string token = path.substr(5, 43);
    CHECK_EQ(fixture.server->database().db().icsTokenHash(ownerId),
             selfvoy::crypto::toHex(selfvoy::crypto::sha256(token)));
    feed = integration::payloadOf(fixture.call(owner, "calendar.feed"));
    CHECK(feed.getBool("enabled"));
    CHECK_EQ(feed.getString("url"), std::string());
    CHECK_EQ(feed.getString("path"), std::string());

    selfvoy::net::HttpResponse http;
    CHECK_EQ(feed_test::httpGet(fixture, path, http), std::string());
    CHECK_EQ(http.status, 200);
    CHECK_EQ(http.header("content-type"), std::string("text/calendar; charset=utf-8"));
    CHECK_EQ(http.header("cache-control"), std::string("no-store"));
    const std::string body = http.body;
    CHECK(body.rfind("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n", 0) == 0);
    CHECK(body.find("X-WR-CALNAME:Selfvoy\r\n") != std::string::npos);
    CHECK(body.size() >= 15 && body.compare(body.size() - 15, 15, "END:VCALENDAR\r\n") == 0);
    CHECK(feed_test::foldedWell(body));
    const std::string ics = feed_test::unfold(body);
    CHECK(ics.find("UID:booking-" + std::to_string(bookingId) + "-" + std::to_string(first) + "@selfvoy\r\n") !=
          std::string::npos);
    CHECK(ics.find("DTSTART:" + selfvoy::ics::utcStamp(first) + "\r\n") != std::string::npos);
    CHECK(ics.find("DTEND:" + selfvoy::ics::utcStamp(first + 3600) + "\r\n") != std::string::npos);
    CHECK(ics.find("SUMMARY:Разбор\\; проекта\\, v2: Пётр\r\n") != std::string::npos);
    CHECK(ics.find("DESCRIPTION:Про стартап\\, деньги\\; планы\\nи команду\r\n") != std::string::npos);
    CHECK(ics.find("STATUS:CONFIRMED\r\n") != std::string::npos);
    CHECK(ics.find("DTSTAMP:") != std::string::npos);
    CHECK(ics.find("UID:invite-" + std::to_string(inviteId) + "-" + std::to_string(first + 86400) + "@selfvoy") !=
          std::string::npos);
    CHECK(ics.find("SUMMARY:Созвон: Анна\r\n") != std::string::npos);
    CHECK(ics.find("invite-" + std::to_string(openId) + "-") == std::string::npos);
    CHECK(ics.find("feed@example.com") == std::string::npos);

    // Отменённая встреча из календаря пропадает.
    Json cancel = Json::object();
    cancel.set("booking_id", Json(bookingId));
    CHECK_EQ(fixture.call(owner, "booking.ownerCancel", cancel).getString("type"), std::string("ok"));
    CHECK(fixture.server->calendarFeed().render(ownerId, std::time(nullptr)).find("UID:booking-") ==
          std::string::npos);

    // Только GET; неизвестный и старый токен — 404; /healthz отвечает.
    CHECK_EQ(feed_test::httpGet(fixture, path, http, "POST"), std::string());
    CHECK_EQ(http.status, 405);
    CHECK_EQ(http.header("allow"), std::string("GET"));
    CHECK_EQ(feed_test::httpGet(fixture, "/ics/" + std::string(43, 'A') + ".ics", http), std::string());
    CHECK_EQ(http.status, 404);
    CHECK_EQ(http.header("cache-control"), std::string("no-store"));
    CHECK_EQ(feed_test::httpGet(fixture, "/ics/../../etc/passwd", http), std::string());
    CHECK_EQ(http.status, 404);
    const std::string newPath = integration::payloadOf(fixture.call(owner, "calendar.feedReset")).getString("path");
    CHECK(newPath != path);
    CHECK_EQ(feed_test::httpGet(fixture, path, http), std::string());
    CHECK_EQ(http.status, 404);
    CHECK_EQ(feed_test::httpGet(fixture, newPath, http), std::string());
    CHECK_EQ(http.status, 200);
    CHECK_EQ(feed_test::httpGet(fixture, "/healthz", http), std::string());
    CHECK_EQ(http.status, 200);
    CHECK_EQ(http.body, std::string("{\"status\":\"ok\"}"));
    feed_test::httpGet(fixture, "/other", http);
    CHECK_EQ(http.status, 400);  // прочие HTTP-запросы — как раньше

    // Заблокированный владелец — 404; выключенная подписка — 404.
    CHECK(fixture.server->database().db().setUserActive(ownerId, false).ok);
    CHECK_EQ(feed_test::httpGet(fixture, newPath, http), std::string());
    CHECK_EQ(http.status, 404);
    CHECK(fixture.server->database().db().setUserActive(ownerId, true).ok);
    const Json disabled = integration::payloadOf(fixture.call(owner, "calendar.feedDisable"));
    CHECK(!disabled.getBool("enabled", true));
    CHECK(!integration::payloadOf(fixture.call(owner, "calendar.feed")).getBool("enabled", true));
    CHECK_EQ(feed_test::httpGet(fixture, newPath, http), std::string());
    CHECK_EQ(http.status, 404);

    // Лимит по IP: 60 запросов в минуту, дальше 429 (до поиска токена).
    auto& calendar = fixture.server->calendarFeed();
    const std::string probe = "/ics/" + std::string(43, 'B') + ".ics";
    std::string last;
    for (int i = 0; i < selfvoy::CalendarFeed::kFetchPerMinute; ++i) last = calendar.serveHttp("GET", probe, "203.0.113.9");
    CHECK(last.rfind("HTTP/1.1 404", 0) == 0);
    last = calendar.serveHttp("GET", probe, "203.0.113.9");
    CHECK(last.rfind("HTTP/1.1 429", 0) == 0);
    CHECK(last.find("Cache-Control: no-store\r\n") != std::string::npos);
    CHECK(calendar.serveHttp("GET", probe, "203.0.113.10").rfind("HTTP/1.1 404", 0) == 0);
    CHECK_EQ(calendar.serveHttp("GET", "/healthz", "203.0.113.9"), std::string());  // не маршрут ICS
}

// ------------------------------------------- рост (S1b): напоминания, подпись, награда
namespace growth_s1b {

using booking_test::bookPayload;
using booking_test::daySlot;
using booking_test::pagePayload;
using booking_test::settledOutbox;
using booking_test::slugPayload;
using booking_test::tokenPayload;

// Напоминания гостю (письма из ящика dev) после since: только kind booking.reminder на адрес to.
std::vector<selfvoy::MailNotice> reminderMails(const std::string& to) {
    std::vector<selfvoy::MailNotice> out;
    for (const auto& mail : settledOutbox()) {
        if (mail.to == to && mail.kind == "booking.reminder") out.push_back(mail);
    }
    return out;
}

// Напоминания записи из вида владельца (booking.list).
Json ownerReminders(Fixture& fixture, const std::shared_ptr<selfvoy::Session>& owner, long long bookingId) {
    const Json list = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    for (const auto& item : list.items()) {
        if (item.getInt("id") == bookingId) return item.get("reminders");
    }
    return Json::array();
}

bool hasReminder(const Json& reminders, std::time_t start, const std::string& kind, std::time_t due) {
    for (const auto& item : reminders.items()) {
        std::time_t at = 0;
        std::time_t dueAt = 0;
        if (selfvoy::parseIsoUtc(item.getString("start"), at) && at == start && item.getString("kind") == kind &&
            selfvoy::parseIsoUtc(item.getString("due_at"), dueAt) && dueAt == due) {
            return true;
        }
    }
    return false;
}

std::size_t countKind(selfvoy::IDatabase& db, long long userId, const std::string& kind) {
    std::size_t count = 0;
    for (const auto& item : db.listNotifications(userId, false, 200)) {
        if (item.kind == kind) ++count;
    }
    return count;
}

// Страница и запись напрямую в БД (без AI); token "" — старая запись без шифра токена.
long long directPage(selfvoy::IDatabase& db, long long ownerId, const std::string& slug, bool reminders = true) {
    selfvoy::BookingPageRecord page;
    page.ownerId = ownerId;
    page.slug = slug;
    page.title = "Разбор";
    page.guestReminders = reminders;
    long long id = 0;
    db.createBookingPage(page, id);
    return id;
}

long long directBooking(selfvoy::IDatabase& db, long long ownerId, long long pageId, std::time_t start,
                        const std::string& email, const std::string& token) {
    Json occurrence = Json::object();
    occurrence.set("start", Json(selfvoy::isoUtc(start)));
    occurrence.set("end", Json(selfvoy::isoUtc(start + 3600)));
    occurrence.set("day", Json(selfvoy::isoUtc(start).substr(0, 10)));
    occurrence.set("local", Json(selfvoy::isoUtc(start + 3 * 3600).substr(0, 16)));  // пояс UTC+3
    occurrence.set("status", Json("active"));
    Json occurrences = Json::array();
    occurrences.push(occurrence);
    selfvoy::BookingRecord booking;
    booking.pageId = pageId;
    booking.ownerId = ownerId;
    booking.manageHash = selfvoy::BookingManager::tokenHash(token.empty() ? growth_test::newToken() : token);
    booking.guestName = "Гость";
    booking.guestEmail = email;
    booking.occurrences = occurrences;
    long long id = 0;
    db.createBooking(booking, id);
    return id;
}

}  // namespace growth_s1b

// F1: расписание напоминаний при записи, переносе и отмене; отправка тиком планировщика
// с будущим now — письмо (dev-почта) и Telegram с кнопками; повторно не уходит.
TEST(booking_reminders_schedule_and_tick) {
    using namespace growth_s1b;
    using namespace growth_test;
    const std::time_t slotA = invite_test::future(30);  // ≥ 25 ч — оба напоминания
    const std::time_t slotD = invite_test::future(54);
    const std::time_t slotB = invite_test::future(5);   // 5–5,5 ч — только «за 2 часа»
    const std::time_t slotC = invite_test::future(1);   // < 2,5 ч — без напоминаний
    const std::time_t slotE = invite_test::future(78);
    invite_test::FakeAi ai([&] {
        Json slots = Json::array();
        for (const std::time_t at : {slotC, slotB, slotA, slotD, slotE}) {
            slots.push(daySlot(at, 60, selfvoy::isoUtc(at).substr(0, 10)));
        }
        Json body = Json::object();
        body.set("slots", slots);
        return body.dump();
    });
    Fixture fixture;
    FakeTelegram* fake = enableBot(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "rem-owner");
    fixture.registerUser(owner, "remowner@example.com", "Ольга");
    Json pageReq = pagePayload("Разбор");
    pageReq.set("max_per_day", Json(8LL));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    CHECK(page.getBool("guest_reminders", false));
    CHECK(!page.getBool("hide_branding", true));
    const std::string slug = page.getString("slug");
    auto guest = sockets.open(fixture, "rem-guest");
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();

    const auto book = [&](std::time_t at, const std::string& name, const std::string& email) {
        Json payload = bookPayload(slug, at, name);
        payload.set("guest_email", Json(email));
        return integration::payloadOf(fixture.call(guest, "booking.book", payload));
    };
    const Json bookedA = book(slotA, "Анна", "rem-a@example.com");
    const Json bookedB = book(slotB, "Борис", "rem-b@example.com");
    const Json bookedC = book(slotC, "Вера", "rem-c@example.com");
    const Json bookedE = book(slotE, "Глеб", "rem-e@example.com");
    const std::string tokenA = bookedA.getString("token");
    CHECK(!tokenA.empty() && !bookedB.getString("token").empty() && !bookedC.getString("token").empty());
    const long long idA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenA))->id;
    const long long idB = db.findBookingByToken(selfvoy::BookingManager::tokenHash(bookedB.getString("token")))->id;
    const long long idC = db.findBookingByToken(selfvoy::BookingManager::tokenHash(bookedC.getString("token")))->id;
    const long long idE = db.findBookingByToken(selfvoy::BookingManager::tokenHash(bookedE.getString("token")))->id;

    // Запись: 24h (если до начала ≥ 25 ч) и 2h (≥ 2 ч 30 мин); вид владельца их показывает.
    Json remindersA = ownerReminders(fixture, owner, idA);
    CHECK_EQ(remindersA.size(), std::size_t(2));
    CHECK(hasReminder(remindersA, slotA, "24h", slotA - 24 * 3600));
    CHECK(hasReminder(remindersA, slotA, "2h", slotA - 2 * 3600));
    for (const auto& item : remindersA.items()) CHECK_EQ(item.getString("sent_at"), std::string());
    const Json remindersB = ownerReminders(fixture, owner, idB);
    CHECK_EQ(remindersB.size(), std::size_t(1));
    CHECK(hasReminder(remindersB, slotB, "2h", slotB - 2 * 3600));
    CHECK_EQ(ownerReminders(fixture, owner, idC).size(), std::size_t(0));
    CHECK_EQ(db.listBookingReminders(idE).size(), std::size_t(2));

    // Отмена — неотправленные напоминания удаляются.
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(bookedE.getString("token"))).getString("type"),
             std::string("ok"));
    CHECK_EQ(db.listBookingReminders(idE).size(), std::size_t(0));

    // Перенос A → D: напоминания прежнего времени удалены, нового — созданы.
    Json move = tokenPayload(tokenA);
    move.set("start", Json(selfvoy::isoUtc(slotD)));
    CHECK_EQ(fixture.call(guest, "booking.reschedule", move).getString("type"), std::string("ok"));
    remindersA = ownerReminders(fixture, owner, idA);
    CHECK_EQ(remindersA.size(), std::size_t(2));
    CHECK(hasReminder(remindersA, slotD, "24h", slotD - 24 * 3600));
    CHECK(hasReminder(remindersA, slotD, "2h", slotD - 2 * 3600));

    // Гость A подключает Telegram — напоминание уйдёт и в чат.
    auto& bot = fixture.server->telegram();
    bot.handleUpdate(textUpdate(9001, "/start " + startPayload(bookedA.getString("telegram_url"))));
    bot.flush();
    CHECK(hasChat(db, idA, 9001));
    bookings.flushMail();
    const std::size_t sentBefore = fake->of("sendMessage").size();

    // Тик «за сутки до D»: захвачены 24h записи A и 2h записи B (B уже началась — пропуск).
    const std::time_t tick1 = slotD - 24 * 3600 + 60;
    CHECK_EQ(bookings.reminderTick(100, tick1), std::size_t(2));
    bookings.flushMail();
    bot.flush();
    const auto mailsA = reminderMails("rem-a@example.com");
    CHECK_EQ(mailsA.size(), std::size_t(1));
    if (!mailsA.empty()) {
        const auto& mail = mailsA.back();
        char clock[8];
        std::tm parts{};
        gmtime_r(&slotD, &parts);
        std::snprintf(clock, sizeof clock, "%02d:%02d", parts.tm_hour, parts.tm_min);
        CHECK_EQ(mail.subject, "Напоминание: завтра в " + std::string(clock) + " — Разбор");
        CHECK(mail.lead.find("Ольга ждёт вас завтра в") == 0);
        CHECK_EQ(mail.linkText, std::string("Приду"));
        CHECK_EQ(mail.linkUrl, "https://selfvoy.example.com/#/m/" + tokenA + "?confirm=" + selfvoy::isoUtc(slotD));
        CHECK(std::find(mail.footer.begin(), mail.footer.end(),
                        "Перенести или отменить: https://selfvoy.example.com/#/m/" + tokenA) != mail.footer.end());
        // Подпись Selfvoy — последняя строка письма.
        CHECK(!mail.footer.empty() &&
              mail.footer.back().find("Запись через Selfvoy — заведите свой бесплатно: "
                                      "https://selfvoy.example.com/#/signup?ref=") == 0);
        CHECK(mail.footer.back().find("&src=booking") != std::string::npos);
        CHECK(mail.lead.find("Анна") == std::string::npos);  // имя гостя в извещения не попадает
    }
    CHECK(reminderMails("rem-b@example.com").empty());
    // Telegram: кнопка «Приду» (callback) и url-кнопка «Перенести или отменить».
    bool telegramOk = false;
    const auto sent = fake->of("sendMessage");
    for (std::size_t i = sentBefore; i < sent.size(); ++i) {
        const auto& item = sent[i];
        if (item.body.getInt("chat_id") != 9001) continue;
        CHECK(item.body.getString("text").find("Напоминание о встрече\n«Разбор» — Ольга") == 0);
        CHECK(item.body.getString("text").find("Запись через Selfvoy — заведите свой бесплатно") != std::string::npos);
        const Json keyboard = item.body.get("reply_markup").get("inline_keyboard");
        telegramOk = keyboard.size() == 2 &&
                     keyboard.at(0).at(0).getString("callback_data") ==
                         "cf:" + std::to_string(idA) + ":" + std::to_string(static_cast<long long>(slotD)) &&
                     keyboard.at(1).at(0).getString("url") == "https://selfvoy.example.com/#/m/" + tokenA;
    }
    CHECK(telegramOk);
    remindersA = ownerReminders(fixture, owner, idA);
    std::size_t sentMarks = 0;
    for (const auto& item : remindersA.items()) {
        if (!item.getString("sent_at").empty()) {
            ++sentMarks;
            CHECK_EQ(item.getString("kind"), std::string("24h"));
        }
    }
    CHECK_EQ(sentMarks, std::size_t(1));

    // Повторный тик с тем же now — ничего не уходит второй раз.
    CHECK_EQ(bookings.reminderTick(100, tick1), std::size_t(0));
    bookings.flushMail();
    CHECK_EQ(reminderMails("rem-a@example.com").size(), std::size_t(1));

    // «За 2 часа»: тема «сегодня/завтра в …», уходит один раз.
    CHECK_EQ(bookings.reminderTick(100, slotD - 2 * 3600 + 60), std::size_t(1));
    bookings.flushMail();
    const auto mailsA2 = reminderMails("rem-a@example.com");
    CHECK_EQ(mailsA2.size(), std::size_t(2));
    if (mailsA2.size() == 2) CHECK(mailsA2.back().subject.find("Напоминание: ") == 0);
    CHECK_EQ(bookings.reminderTick(100, slotD - 60), std::size_t(0));
    // Отменённая запись E не напоминается и после наступления срока.
    CHECK_EQ(bookings.reminderTick(100, slotE - 3600), std::size_t(0));
    CHECK(reminderMails("rem-e@example.com").empty());
}

// F1: пропуски в момент отправки (встреча отменена в обход, guest_reminders=false) и текст
// для старой записи без сохранённого токена.
TEST(booking_reminders_skip_and_legacy_text) {
    using namespace growth_s1b;
    using namespace growth_test;
    Fixture fixture;
    FakeTelegram* fake = enableBot(fixture, "", "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "legacy-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "legacyowner@example.com", "Ирина"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    const std::time_t now = std::time(nullptr);
    const long long page = directPage(db, ownerId, "legacy0001");
    const long long quietPage = directPage(db, ownerId, "legacy0002", false);

    const std::time_t start1 = now + 48 * 3600;
    const long long legacy = directBooking(db, ownerId, page, start1, "legacy@example.com", "");
    const long long dropped = directBooking(db, ownerId, page, start1 + 3600, "dropped@example.com", "");
    const long long quiet = directBooking(db, ownerId, quietPage, start1 + 1800, "quiet@example.com", "");
    for (const long long id : {legacy, dropped, quiet}) bookings.scheduleReminders(*db.findBooking(id), now);
    CHECK_EQ(db.listBookingReminders(legacy).size(), std::size_t(2));
    CHECK(db.linkBookingTelegram(legacy, 9101, 3));
    // Встреча отменена в обход BookingManager (напоминания остались) — отправка её пропускает.
    auto record = db.findBooking(dropped);
    Json occurrences = Json::array();
    Json cancelled = record->occurrences.at(0);
    cancelled.set("status", Json("cancelled"));
    occurrences.push(cancelled);
    record->occurrences = occurrences;
    record->status = "cancelled";
    CHECK(db.updateBooking(*record).ok);
    CHECK_EQ(db.listBookingReminders(dropped).size(), std::size_t(2));

    const std::time_t tick = now + 25 * 3600 + 60;  // наступили все три «за сутки»
    CHECK_EQ(bookings.reminderTick(100, tick), std::size_t(3));
    bookings.flushMail();
    fixture.server->telegram().flush();
    CHECK(reminderMails("dropped@example.com").empty());
    CHECK(reminderMails("quiet@example.com").empty());
    const auto legacyMails = reminderMails("legacy@example.com");
    CHECK_EQ(legacyMails.size(), std::size_t(1));
    if (!legacyMails.empty()) {
        const auto& mail = legacyMails.back();
        // Местное время владельца (UTC+3 по полю local) и пояс владельца.
        char clock[8];
        const std::time_t local = start1 + 3 * 3600;
        std::tm parts{};
        gmtime_r(&local, &parts);
        std::snprintf(clock, sizeof clock, "%02d:%02d", parts.tm_hour, parts.tm_min);
        CHECK(mail.subject.find(std::string(" в ") + clock + " — Разбор") != std::string::npos);
        CHECK(mail.lead.find("(Europe/Moscow)") != std::string::npos);
        CHECK(mail.linkUrl.empty());
        CHECK(std::find(mail.footer.begin(), mail.footer.end(),
                        "Перенести или отменить можно по ссылке из письма о записи") != mail.footer.end());
    }
    const std::string text = fake->last(9101);
    CHECK(text.find("Перенести или отменить можно по ссылке из письма о записи") != std::string::npos);
    bool callbackOnly = false;
    for (const auto& item : fake->of("sendMessage")) {
        if (item.body.getInt("chat_id") != 9101) continue;
        const Json keyboard = item.body.get("reply_markup").get("inline_keyboard");
        callbackOnly = keyboard.size() == 1 &&
                       keyboard.at(0).at(0).getString("callback_data").find("cf:" + std::to_string(legacy) + ":") == 0;
    }
    CHECK(callbackOnly);
    // Захваченные строки отмечены — второй тик их не трогает.
    CHECK_EQ(bookings.reminderTick(100, tick), std::size_t(0));
    std::size_t sentRows = 0;
    for (const long long id : {legacy, dropped, quiet}) {
        for (const auto& reminder : db.listBookingReminders(id)) sentRows += reminder.sentAt.empty() ? 0 : 1;
    }
    CHECK_EQ(sentRows, std::size_t(3));
}

// F1: booking.confirm (идемпотентно, уведомление владельцу один раз) и «Приду» из Telegram.
TEST(booking_confirm_and_telegram_button) {
    using namespace growth_s1b;
    using namespace growth_test;
    Fixture fixture;
    FakeTelegram* fake = enableBot(fixture, "", "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "confirm-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "confirmowner@example.com", "Павел"));
    auto guest = sockets.open(fixture, "confirm-guest");
    auto& db = fixture.server->database().db();
    const std::time_t now = std::time(nullptr);
    const long long page = directPage(db, ownerId, "confirm001");
    const std::string token = newToken();
    const std::time_t start = now + 30 * 3600;
    const long long booking = directBooking(db, ownerId, page, start, "", token);

    const Json first = fixture.call(guest, "booking.confirm", tokenPayload(token));
    CHECK_EQ(first.getString("type"), std::string("ok"));
    const std::string confirmedAt =
        integration::payloadOf(first).get("booking").get("occurrences").at(0).getString("confirmed_at");
    CHECK(!confirmedAt.empty());
    CHECK_EQ(countKind(db, ownerId, "booking.confirmed"), std::size_t(1));
    for (const auto& item : db.listNotifications(ownerId, false, 50)) {
        if (item.kind != "booking.confirmed") continue;
        CHECK_EQ(item.payload.getInt("booking_id"), booking);
        CHECK_EQ(item.payload.getInt("page_id"), page);
        CHECK_EQ(item.payload.getString("start"), selfvoy::isoUtc(start));
        CHECK_EQ(item.payload.getString("guest_name"), std::string("Гость"));
    }
    // Повтор (и с явным occurrence) — без изменений и без второго уведомления.
    Json again = tokenPayload(token);
    again.set("occurrence", Json(selfvoy::isoUtc(start)));
    const Json second = fixture.call(guest, "booking.confirm", again);
    CHECK_EQ(integration::payloadOf(second).get("booking").get("occurrences").at(0).getString("confirmed_at"),
             confirmedAt);
    CHECK_EQ(countKind(db, ownerId, "booking.confirmed"), std::size_t(1));
    // Вид владельца: confirmed_at у встречи и список напоминаний.
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings").at(0);
    CHECK_EQ(listed.get("occurrences").at(0).getString("confirmed_at"), confirmedAt);
    CHECK(listed.get("reminders").isArray());
    // Ошибки: чужой токен, неверное время, нет такой встречи.
    CHECK_EQ(fixture.call(guest, "booking.confirm", tokenPayload(newToken())).getString("code"),
             std::string("not_found"));
    Json badTime = tokenPayload(token);
    badTime.set("occurrence", Json("завтра"));
    CHECK_EQ(fixture.call(guest, "booking.confirm", badTime).getString("code"), std::string("bad_request"));
    Json missing = tokenPayload(token);
    missing.set("occurrence", Json(selfvoy::isoUtc(start + 3600)));
    CHECK_EQ(fixture.call(guest, "booking.confirm", missing).getString("code"), std::string("conflict"));
    // booking.confirm — публичный метод (без входа).
    CHECK(selfvoy::protocol::isPublicType("booking.confirm"));

    // «Приду» из Telegram: чужой чат — отказ; привязанный — подтверждение, клавиатура убрана.
    const std::string token2 = newToken();
    const std::time_t start2 = now + 50 * 3600;
    const long long booking2 = directBooking(db, ownerId, page, start2, "", token2);
    CHECK(db.linkBookingTelegram(booking2, 9201, 3));
    auto& bot = fixture.server->telegram();
    const std::string data = "cf:" + std::to_string(booking2) + ":" + std::to_string(static_cast<long long>(start2));
    bot.handleUpdate(callbackUpdate(9299, data));
    bot.flush();
    CHECK(db.findBooking(booking2)->occurrences.at(0).getString("confirmed_at").empty());
    bot.handleUpdate(callbackUpdate(9201, data));
    bot.flush();
    const auto answers = fake->of("answerCallbackQuery");
    CHECK_EQ(answers.size(), std::size_t(2));
    if (answers.size() == 2) {
        CHECK(answers[0].body.getString("text").find("не для этого чата") != std::string::npos);
        CHECK_EQ(answers[1].body.getString("text"), std::string("Спасибо, ждём вас"));
    }
    CHECK_EQ(fake->of("editMessageReplyMarkup").size(), std::size_t(1));
    CHECK(!db.findBooking(booking2)->occurrences.at(0).getString("confirmed_at").empty());
    CHECK_EQ(countKind(db, ownerId, "booking.confirmed"), std::size_t(2));
    // Повторное нажатие — «спасибо» без второго уведомления; время не той встречи — отказ.
    bot.handleUpdate(callbackUpdate(9201, data));
    bot.handleUpdate(callbackUpdate(9201, "cf:" + std::to_string(booking2) + ":" +
                                              std::to_string(static_cast<long long>(start2 + 60))));
    bot.flush();
    CHECK_EQ(countKind(db, ownerId, "booking.confirmed"), std::size_t(2));
    const auto more = fake->of("answerCallbackQuery");
    CHECK_EQ(more.size(), std::size_t(4));
    if (more.size() == 4) {
        CHECK_EQ(more[2].body.getString("text"), std::string("Спасибо, ждём вас"));
        CHECK(more[3].body.getString("text").find("прошла или отменена") != std::string::npos);
    }
}

// F3: подпись в видах гостя, hide_branding (Pro при enforced=1, действует только с Pro).
TEST(booking_branding_and_pro) {
    using namespace growth_s1b;
    Fixture fixture;
    fixture.config.publicUrl = "https://selfvoy.example.com";
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    CHECK(fixture.server->start(error));
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "brand-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "brandowner@example.com", "Лена"));
    auto guest = sockets.open(fixture, "brand-guest");
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pagePayload("Подпись")))
                          .get("page");
    const std::string slug = page.getString("slug");
    const long long pageId = page.getInt("id");
    const std::string code = integration::payloadOf(fixture.call(owner, "referral.get")).getString("code");
    const auto guestBranding = [&]() {
        return integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page").get("branding");
    };
    Json branding = guestBranding();
    CHECK(branding.getBool("show"));
    CHECK_EQ(branding.getString("text"), std::string("Запись через Selfvoy"));
    CHECK_EQ(branding.getString("url"), "https://selfvoy.example.com/#/signup?ref=" + code + "&src=booking");
    auto& db = fixture.server->database().db();
    const std::string token = growth_test::newToken();
    directBooking(db, ownerId, pageId, std::time(nullptr) + 40 * 3600, "", token);
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token))).get("booking");
    CHECK(managed.get("branding").getBool("show"));
    CHECK_EQ(managed.get("branding").getString("url"), branding.getString("url"));

    // enforced=0 (бета): скрыть можно всем.
    Json hide = Json::object();
    hide.set("page_id", Json(pageId));
    hide.set("hide_branding", Json(true));
    const Json hidden = fixture.call(owner, "booking.pageUpdate", hide);
    CHECK(integration::payloadOf(hidden).get("page").getBool("hide_branding"));
    CHECK(!guestBranding().getBool("show", true));
    Json badType = hide;
    badType.set("hide_branding", Json("yes"));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", badType).getString("code"), std::string("bad_request"));

    // enforced=1, владелец без Pro: скрытие не действует, включить заново/на новой странице — pro_required.
    fixture.config.plansEnforced = true;
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    CHECK(fixture.server->start(error));
    auto owner2 = sockets.open(fixture, "brand-owner2");
    fixture.call(owner2, "auth.login", admin_test::loginPayload("brandowner@example.com"));
    auto guest2 = sockets.open(fixture, "brand-guest2");
    const auto guestBranding2 = [&]() {
        return integration::payloadOf(fixture.call(guest2, "booking.get", slugPayload(slug))).get("page").get("branding");
    };
    CHECK(guestBranding2().getBool("show"));
    Json show = hide;
    show.set("hide_branding", Json(false));
    CHECK_EQ(fixture.call(owner2, "booking.pageUpdate", show).getString("type"), std::string("ok"));  // вернуть — можно
    CHECK_EQ(fixture.call(owner2, "booking.pageUpdate", hide).getString("code"), std::string("pro_required"));
    Json created = pagePayload("Вторая");
    created.set("hide_branding", Json(true));
    CHECK_EQ(fixture.call(owner2, "booking.pageCreate", created).getString("code"), std::string("pro_required"));
    // С Pro — можно, и подпись скрыта.
    auto& db2 = fixture.server->database().db();
    CHECK(db2.setUserPlanDays(ownerId, 30).ok);
    CHECK_EQ(fixture.call(owner2, "booking.pageUpdate", hide).getString("type"), std::string("ok"));
    CHECK(!guestBranding2().getBool("show", true));
    // Pro закончился — подпись снова показывается (флаг на странице остаётся).
    CHECK(db2.setUserPlanDays(ownerId, 0).ok);
    CHECK(guestBranding2().getBool("show"));
    CHECK(db2.findBookingPage(pageId)->hideBranding);
    selfvoy::plan::setEnforced(false);
}

// F3: награда за подтверждённого приглашённого — обоим +30 дней Pro, один раз; не больше
// 12 наград пригласившему.
TEST(referral_reward_both_idempotent_and_cap) {
    Fixture fixture;
    admin_test::Sockets sockets;
    auto inviter = sockets.open(fixture, "ref-inviter");
    const long long inviterId = admin_test::userIdOf(fixture.registerUser(inviter, "inviter@example.com", "Марина"));
    const std::string code = integration::payloadOf(fixture.call(inviter, "referral.get")).getString("code");
    auto newcomer = sockets.open(fixture, "ref-newcomer");
    Json reg = Json::object();
    reg.set("email", Json("newcomer@example.com"));
    reg.set("password", Json("selfvoy1234"));
    reg.set("display_name", Json("Никита"));
    reg.set("ref", Json(code));
    const Json registered = fixture.call(newcomer, "auth.register", reg);
    CHECK_EQ(registered.getString("type"), std::string("ok"));
    Json verify = Json::object();
    verify.set("email", Json("newcomer@example.com"));
    verify.set("code", Json(integration::payloadOf(registered).getString("email_code")));
    CHECK_EQ(fixture.call(newcomer, "auth.verifyEmail", verify).getString("type"), std::string("ok"));
    auto& db = fixture.server->database().db();
    const long long newcomerId = db.findUserByEmail("newcomer@example.com")->id;
    const std::time_t now = std::time(nullptr);
    const auto untilOf = [&](long long id) {
        std::time_t until = 0;
        selfvoy::parseIsoUtc(db.findUserById(id)->planUntil, until);
        return until;
    };
    CHECK(std::llabs(static_cast<long long>(untilOf(inviterId) - (now + 30 * 86400))) < 120);
    CHECK(std::llabs(static_cast<long long>(untilOf(newcomerId) - (now + 30 * 86400))) < 120);
    const Json info = integration::payloadOf(fixture.call(inviter, "referral.get"));
    CHECK_EQ(info.getInt("rewarded"), 1LL);
    CHECK_EQ(info.getInt("reward_days"), 30LL);
    CHECK_EQ(info.getInt("reward_cap"), 12LL);
    CHECK_EQ(info.get("plan").getString("name"), std::string("pro"));
    bool joinedBody = false;
    for (const auto& item : db.listNotifications(inviterId, false, 20)) {
        if (item.kind == "referral.joined") joinedBody = item.body.find("+1 месяц Pro") != std::string::npos;
    }
    CHECK(joinedBody);
    CHECK_EQ(growth_s1b::countKind(db, newcomerId, "referral.reward"), std::size_t(1));
    for (const auto& item : db.listNotifications(newcomerId, false, 20)) {
        if (item.kind == "referral.reward") CHECK_EQ(item.title, std::string("Месяц Pro в подарок"));
    }

    // Повторная верификация не начисляет второй раз.
    const std::time_t inviterUntil = untilOf(inviterId);
    const std::time_t newcomerUntil = untilOf(newcomerId);
    fixture.server->referrals().verified(newcomerId);
    bool referrerRewarded = true;
    CHECK_EQ(db.rewardReferral(newcomerId, 30, 12, referrerRewarded), 0LL);
    CHECK(!referrerRewarded);
    CHECK_EQ(untilOf(inviterId), inviterUntil);
    CHECK_EQ(untilOf(newcomerId), newcomerUntil);
    CHECK_EQ(growth_s1b::countKind(db, newcomerId, "referral.reward"), std::size_t(1));
    CHECK_EQ(growth_s1b::countKind(db, inviterId, "referral.joined"), std::size_t(1));

    // Не подтверждён — не награждается.
    long long pending = 0;
    CHECK(db.createUser("pending-ref@example.com", "argon2id$x", "Ждёт", pending).ok);
    CHECK(db.createReferral(inviterId, pending, "link").ok);
    CHECK_EQ(db.rewardReferral(pending, 30, 12, referrerRewarded), 0LL);

    // Лимит: у пригласившего не больше 12 наград, приглашённые получают всегда; продление
    // складывается: greatest(now, plan_until) + 30 дней.
    long long capper = 0;
    CHECK(db.createUser("capper@example.com", "argon2id$x", "Капитан", capper).ok);
    int rewardedCount = 0;
    for (int i = 0; i < 13; ++i) {
        long long friendId = 0;
        CHECK(db.createUser("cap-friend" + std::to_string(i) + "@example.com", "argon2id$x", "Друг", friendId).ok);
        CHECK(db.createReferral(capper, friendId, "link").ok);
        CHECK_EQ(db.markReferralVerified(friendId), capper);
        bool toReferrer = false;
        CHECK_EQ(db.rewardReferral(friendId, 30, 12, toReferrer), capper);
        if (toReferrer) ++rewardedCount;
        CHECK(std::llabs(static_cast<long long>(untilOf(friendId) - (now + 30 * 86400))) < 120);
    }
    CHECK_EQ(rewardedCount, 12);
    CHECK(std::llabs(static_cast<long long>(untilOf(capper) - (now + 12LL * 30 * 86400))) < 120);
    CHECK(db.extendUserPlanDays(capper, 1).ok);
    CHECK(std::llabs(static_cast<long long>(untilOf(capper) - (now + (12LL * 30 + 1) * 86400))) < 120);
    CHECK(!db.extendUserPlanDays(capper, 0).ok);
}

// ------------------------------------- рост (S2a): цена, предоплата, политика отмены (F4)
namespace payments_test {

using booking_test::bookPayload;
using booking_test::daySlot;
using booking_test::pagePayload;
using booking_test::settledOutbox;
using booking_test::slugPayload;
using booking_test::tokenPayload;

constexpr const char* kShop = "100500";
constexpr const char* kSecret = "test_Fh8hUAVVBGUGbjmlzba6TB0iyUbos_lueTHE-axOwM0";

// Поддельный API ЮKassa: /me (Basic shop:secret), POST /payments (Idempotence-Key →
// тот же платёж), GET /payments/<id>, POST /refunds. Записывает все вызовы.
struct FakeYooKassa : selfvoy::PaymentManager::Transport {
    struct Call {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
        Json body;
    };
    std::mutex mutex;
    std::vector<Call> calls;
    std::map<std::string, std::string> statuses;  // id → pending | succeeded | canceled
    std::map<std::string, std::string> byKey;     // Idempotence-Key → id
    bool failCreate = false;
    bool down = false;  // транспортная ошибка (ЮKassa недоступна)
    int next = 1;

    selfvoy::net::HttpResponse request(const std::string& method, const std::string& url,
                                       const std::map<std::string, std::string>& headers, const std::string& body,
                                       int, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex);
        const std::string base = "http://yookassa.test/v3";
        const std::string path = url.rfind(base, 0) == 0 ? url.substr(base.size()) : url;
        calls.push_back({method, path, headers, body.empty() ? Json() : Json::parse(body)});
        selfvoy::net::HttpResponse response;
        if (down) {
            error = "не удалось подключиться";
            return response;
        }
        const auto auth = headers.find("Authorization");
        const std::string expected =
            "Basic " + selfvoy::crypto::base64Encode(std::string(kShop) + ":" + std::string(kSecret));
        if (auth == headers.end() || auth->second != expected) {
            response.status = 401;
            response.body = R"({"type":"error","id":"e1","code":"invalid_credentials","description":"Authentication by given credentials failed"})";
            return response;
        }
        response.status = 200;
        if (method == "GET" && path == "/me") {
            response.body = R"({"account_id":"100500","test":true,"status":"enabled"})";
        } else if (method == "POST" && path == "/payments") {
            if (failCreate) {
                response.status = 500;
                response.body = R"({"type":"error","code":"internal_server_error","description":"try later"})";
                return response;
            }
            const std::string key = headers.count("Idempotence-Key") ? headers.at("Idempotence-Key") : "";
            std::string id = byKey[key];
            if (id.empty()) {
                id = "pay-" + std::to_string(next++);
                byKey[key] = id;
                statuses[id] = "pending";
            }
            response.body = R"({"id":")" + id + R"(","status":")" + statuses[id] +
                            R"(","paid":false,"confirmation":{"type":"redirect","confirmation_url":"https://yoomoney.example/checkout/)" +
                            id + R"("}})";
        } else if (method == "GET" && path.rfind("/payments/", 0) == 0) {
            const std::string id = path.substr(10);
            if (!statuses.count(id)) {
                response.status = 404;
                response.body = R"({"type":"error","code":"not_found"})";
            } else {
                response.body = R"({"id":")" + id + R"(","status":")" + statuses[id] + R"("})";
            }
        } else if (method == "POST" && path == "/refunds") {
            response.body = R"({"id":"rf-1","status":"succeeded"})";
        } else {
            response.status = 404;
        }
        return response;
    }

    std::vector<Call> of(const std::string& method, const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Call> out;
        for (const auto& call : calls) {
            if (call.method == method && call.path.rfind(path, 0) == 0) out.push_back(call);
        }
        return out;
    }
    void set(const std::string& id, const std::string& status) {
        std::lock_guard<std::mutex> lock(mutex);
        statuses[id] = status;
    }
};

// Перезапуск сервера: поддельный AI, PUBLIC_URL, адрес ЮKassa; транспорт ЮKassa — фейк.
FakeYooKassa* enablePayments(Fixture& fixture, const std::string& aiUrl, bool enforced = false) {
    fixture.config.aiServiceUrl = aiUrl.empty() ? fixture.config.aiServiceUrl : aiUrl;
    fixture.config.publicUrl = "https://selfvoy.example.com";
    fixture.config.yookassaApiUrl = "http://yookassa.test/v3";
    fixture.config.plansEnforced = enforced;
    fixture.server->stop();
    fixture.server = std::make_unique<selfvoy::Server>(fixture.config);
    std::string error;
    if (!fixture.server->start(error)) std::cout << "  не удалось перезапустить сервер: " << error << "\n";
    auto fake = std::make_unique<FakeYooKassa>();
    FakeYooKassa* raw = fake.get();
    fixture.server->payments().setTransport(std::move(fake));
    return raw;
}

// AI отдаёт слоты на указанные моменты (60 минут, местная дата = дата UTC).
std::string slotsBody(const std::vector<std::time_t>& starts) {
    Json slots = Json::array();
    for (const std::time_t at : starts) slots.push(daySlot(at, 60, selfvoy::isoUtc(at).substr(0, 10)));
    Json body = Json::object();
    body.set("slots", slots);
    return body.dump();
}

Json payPage(const std::string& title) {
    Json payload = pagePayload(title);
    payload.set("max_per_day", Json(8LL));
    payload.set("max_series", Json(1LL));
    return payload;
}

std::vector<selfvoy::MailNotice> mails(const std::string& to, const std::string& kind) {
    std::vector<selfvoy::MailNotice> out;
    for (const auto& mail : settledOutbox()) {
        if (mail.to == to && mail.kind == kind) out.push_back(mail);
    }
    return out;
}

bool hasFooter(const selfvoy::MailNotice& mail, const std::string& prefix) {
    return std::any_of(mail.footer.begin(), mail.footer.end(),
                       [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
}

// Уведомления владельца вида kind (по статусу payload.status, если задан).
std::size_t ownerNotices(selfvoy::IDatabase& db, long long ownerId, const std::string& kind,
                         const std::string& status = "") {
    std::size_t count = 0;
    for (const auto& item : db.listNotifications(ownerId, false, 200)) {
        if (item.kind == kind && (status.empty() || item.payload.getString("status") == status)) ++count;
    }
    return count;
}

Json connectPayload(const std::string& shop, const std::string& secret) {
    Json payload = Json::object();
    payload.set("provider", Json("yookassa"));
    payload.set("shop_id", Json(shop));
    payload.set("secret_key", Json(secret));
    return payload;
}

}  // namespace payments_test

// F4: поля страницы — проверка диапазонов и форматов, виды владельца и гостя.
TEST(payments_page_fields_validation) {
    using namespace payments_test;
    Fixture fixture;
    enablePayments(fixture, "");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pay-fields-owner");
    fixture.registerUser(owner, "payfields@example.com", "Ольга");
    const auto code = [&](const std::string& key, const Json& value, const Json& base = Json()) {
        Json payload = base.isObject() ? base : payPage("Проверка");
        payload.set(key, value);
        return fixture.call(owner, "booking.pageCreate", payload).getString("code");
    };
    const std::string bad = "bad_request";
    CHECK_EQ(code("price", Json(-1LL)), bad);
    CHECK_EQ(code("price", Json(100000001LL)), bad);
    CHECK_EQ(code("price", Json(2.5)), bad);
    CHECK_EQ(code("price", Json("2500")), bad);
    CHECK_EQ(code("currency", Json("GBP")), bad);
    CHECK_EQ(code("currency", Json("rub")), bad);
    CHECK_EQ(code("prepay", Json("half")), bad);
    CHECK_EQ(code("pay_within_minutes", Json(14LL)), bad);
    CHECK_EQ(code("pay_within_minutes", Json(1441LL)), bad);
    CHECK_EQ(code("cancel_hours", Json(-1LL)), bad);
    CHECK_EQ(code("cancel_hours", Json(169LL)), bad);
    CHECK_EQ(code("late_cancel", Json("maybe")), bad);
    CHECK_EQ(code("policy_text", Json(std::string(501, 'a'))), bad);
    CHECK_EQ(code("payment_url", Json("http://pay.example/coach")), bad);
    CHECK_EQ(code("payment_url", Json("https://pay.example/a b")), bad);
    CHECK_EQ(code("payment_url", Json("https://pay.example/" + std::string(481, 'a'))), bad);
    CHECK_EQ(code("payment_url", Json("javascript:alert(1)")), bad);
    CHECK_EQ(code("payment_note", Json("строка\nвторая")), bad);
    CHECK_EQ(code("payment_note", Json(std::string(301, 'x'))), bad);
    // Предоплата: нужна цена, способ оплаты; депозит — 1..price.
    Json prepaid = payPage("Предоплата");
    prepaid.set("prepay", Json("full"));
    CHECK_EQ(code("price", Json(0LL), prepaid), bad);
    CHECK_EQ(code("price", Json(250000LL), prepaid), bad);  // нет ни ссылки, ни реквизитов, ни ЮKassa
    prepaid.set("price", Json(250000LL));
    prepaid.set("prepay", Json("deposit"));
    prepaid.set("payment_url", Json("https://pay.example/coach"));
    CHECK_EQ(code("deposit", Json(0LL), prepaid), bad);
    CHECK_EQ(code("deposit", Json(250001LL), prepaid), bad);
    // Цена без предоплаты — бесплатно и без способа оплаты.
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", [] {
                 Json p = payPage("Только цена");
                 p.set("price", Json(150000LL));
                 return p;
             }()).getString("type"),
             std::string("ok"));

    Json valid = payPage("Консультация");
    valid.set("price", Json(250000LL));
    valid.set("currency", Json("RUB"));
    valid.set("prepay", Json("deposit"));
    valid.set("deposit", Json(50000LL));
    valid.set("payment_url", Json("https://pay.example/coach"));
    valid.set("payment_note", Json("  СБП: +7 900 000-00-00 (Ольга)  "));
    valid.set("pay_within_minutes", Json(30LL));
    valid.set("cancel_hours", Json(24LL));
    valid.set("late_cancel", Json("block"));
    valid.set("policy_text", Json("Отмена позже чем за сутки — депозит не возвращается."));
    const Json created = fixture.call(owner, "booking.pageCreate", valid);
    CHECK_EQ(created.getString("type"), std::string("ok"));
    const Json page = integration::payloadOf(created).get("page");
    CHECK_EQ(page.getInt("price"), 250000LL);
    CHECK_EQ(page.getString("currency"), std::string("RUB"));
    CHECK_EQ(page.getString("prepay"), std::string("deposit"));
    CHECK_EQ(page.getInt("deposit"), 50000LL);
    CHECK_EQ(page.getString("payment_url"), std::string("https://pay.example/coach"));
    CHECK_EQ(page.getString("payment_note"), std::string("СБП: +7 900 000-00-00 (Ольга)"));  // обрезаны пробелы
    CHECK_EQ(page.getInt("pay_within_minutes"), 30LL);
    CHECK_EQ(page.getInt("cancel_hours"), 24LL);
    CHECK_EQ(page.getString("late_cancel"), std::string("block"));
    CHECK(page.getString("policy_text").find("депозит") != std::string::npos);
    const long long pageId = page.getInt("id");

    // Вид гостя: цена, предоплата, способ и политика.
    auto guest = sockets.open(fixture, "pay-fields-guest");
    const Json view = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(page.getString("slug"))))
                          .get("page");
    CHECK_EQ(view.getInt("price"), 250000LL);
    CHECK_EQ(view.getString("prepay"), std::string("deposit"));
    CHECK_EQ(view.getInt("deposit"), 50000LL);
    CHECK_EQ(view.getString("payment_url"), std::string("https://pay.example/coach"));
    CHECK_EQ(view.getInt("pay_within_minutes"), 30LL);
    CHECK_EQ(view.get("policy").getInt("cancel_hours"), 24LL);
    CHECK_EQ(view.get("policy").getString("late_cancel"), std::string("block"));
    CHECK(!view.get("policy").getString("text").empty());

    // Правка: цена 0 при предоплате — нельзя; выключить предоплату — можно; название
    // без полей оплаты меняется и без способа оплаты.
    Json update = Json::object();
    update.set("page_id", Json(pageId));
    update.set("price", Json(0LL));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", update).getString("code"), bad);
    update = Json::object();
    update.set("page_id", Json(pageId));
    update.set("deposit", Json(300000LL));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", update).getString("code"), bad);
    update = Json::object();
    update.set("page_id", Json(pageId));
    update.set("prepay", Json("none"));
    update.set("cancel_hours", Json(0LL));
    const Json updated = integration::payloadOf(fixture.call(owner, "booking.pageUpdate", update)).get("page");
    CHECK_EQ(updated.getString("prepay"), std::string("none"));
    CHECK_EQ(updated.getInt("cancel_hours"), 0LL);
    CHECK_EQ(updated.getInt("price"), 250000LL);  // не переданные поля не меняются
    auto stored = fixture.server->database().db().findBookingPage(pageId);
    CHECK(stored && stored->lateCancel == "block" && stored->payWithinMinutes == 30);

    // Сумма для ЮKassa — «2500.00».
    CHECK_EQ(selfvoy::payments::amountText(250000), std::string("2500.00"));
    CHECK_EQ(selfvoy::payments::amountText(5), std::string("0.05"));
    CHECK_EQ(selfvoy::payments::amountText(100000000), std::string("1000000.00"));
    // Поздняя отмена.
    selfvoy::BookingPageRecord policy;
    policy.cancelHours = 24;
    const std::time_t now = std::time(nullptr);
    CHECK(selfvoy::BookingManager::lateFor(policy, now + 23 * 3600, now));
    CHECK(!selfvoy::BookingManager::lateFor(policy, now + 25 * 3600, now));
    policy.cancelHours = 0;
    CHECK(!selfvoy::BookingManager::lateFor(policy, now + 60, now));
}

// F4: при SELFVOY_PLANS_ENFORCED=1 предоплата и подключение ЮKassa — только Pro;
// цена и политика отмены — бесплатно.
TEST(payments_pro_gating_enforced) {
    using namespace payments_test;
    Fixture fixture;
    FakeYooKassa* fake = enablePayments(fixture, "", true);
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pay-pro-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "paypro@example.com", "Пётр"));
    Json prepaid = payPage("Предоплата");
    prepaid.set("price", Json(100000LL));
    prepaid.set("prepay", Json("full"));
    prepaid.set("payment_url", Json("https://pay.example/p"));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", prepaid).getString("code"), std::string("pro_required"));
    Json free = payPage("Бесплатно");
    free.set("price", Json(100000LL));
    free.set("cancel_hours", Json(12LL));
    free.set("late_cancel", Json("block"));
    const Json freePage = fixture.call(owner, "booking.pageCreate", free);
    CHECK_EQ(freePage.getString("type"), std::string("ok"));
    Json enable = Json::object();
    enable.set("page_id", Json(integration::payloadOf(freePage).get("page").getInt("id")));
    enable.set("prepay", Json("full"));
    enable.set("payment_url", Json("https://pay.example/p"));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("code"), std::string("pro_required"));
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, kSecret)).getString("code"),
             std::string("pro_required"));
    CHECK(fake->of("GET", "/me").empty());  // без Pro ключи никуда не уходят

    CHECK(fixture.server->database().db().setUserPlanDays(ownerId, 30).ok);
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", prepaid).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("type"), std::string("ok"));
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, kSecret)).getString("type"),
             std::string("ok"));
    // Pro закончился: уже включённую предоплату можно править (например, сумму).
    CHECK(fixture.server->database().db().setUserPlanDays(ownerId, 0).ok);
    Json change = enable;
    change.erase("prepay");
    change.set("price", Json(120000LL));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", change).getString("type"), std::string("ok"));
    selfvoy::plan::setEnforced(false);
}

// F4: ЮKassa — подключение (GET /me), платёж при записи (POST /payments с Idempotence-Key
// bk-<id>), succeeded → paid в планировщике (не чаще раза в 30 с), canceled → отмена,
// возвраты (rf-<id>) при отмене гостем до срока и владельцем, ошибка создания платежа.
TEST(payments_yookassa_flows) {
    using namespace payments_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    const std::time_t slotD = invite_test::future(102);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB, slotC, slotD}); });
    Fixture fixture;
    FakeYooKassa* fake = enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "yk-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "ykowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();

    CHECK(!integration::payloadOf(fixture.call(owner, "payments.status")).getBool("connected", true));
    // Неверный ключ — bad_request (ЮKassa ответила 401); формат shop_id проверяется до запроса.
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, "test_wrong_secret_value")).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fake->of("GET", "/me").size(), std::size_t(1));
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload("shop-1", kSecret)).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, "short")).getString("code"),
             std::string("bad_request"));
    CHECK_EQ(fake->of("GET", "/me").size(), std::size_t(1));
    // ЮKassa недоступна — upstream_error.
    fake->down = true;
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, kSecret)).getString("code"),
             std::string("upstream_error"));
    fake->down = false;
    const Json connected = fixture.call(owner, "payments.connect", connectPayload(kShop, kSecret));
    CHECK_EQ(connected.getString("type"), std::string("ok"));
    CHECK(integration::payloadOf(connected).getBool("connected"));
    CHECK_EQ(integration::payloadOf(connected).getString("shop_id"), std::string(kShop));
    CHECK_EQ(integration::payloadOf(connected).getString("provider"), std::string("yookassa"));
    const auto account = db.findPaymentAccount(ownerId);
    CHECK(account && account->secretEnc.find(kSecret) == std::string::npos);
    std::string opened;
    CHECK(account && selfvoy::crypto::openField(fixture.config.dataKey(), account->secretEnc,
                                                selfvoy::payments::kSecretAad, opened) &&
          opened == kSecret);
    const Json status = integration::payloadOf(fixture.call(owner, "payments.status"));
    CHECK(status.getBool("connected") && !status.getString("connected_at").empty());
    CHECK(!status.contains("secret_key"));

    // Страница с полной предоплатой: способ — подключённая ЮKassa.
    Json pageReq = payPage("Разбор");
    pageReq.set("price", Json(250000LL));
    pageReq.set("prepay", Json("full"));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    CHECK_EQ(page.getString("prepay"), std::string("full"));
    const std::string slug = page.getString("slug");
    // Цена страницы — в заголовке поста «свободные окна» (F6).
    Json post = Json::object();
    post.set("page_id", Json(page.getInt("id")));
    const std::string postText = integration::payloadOf(fixture.call(owner, "booking.slotsPost", post)).getString("text");
    CHECK(postText.find("Разбор (1 ч, 2 500 ₽)") != std::string::npos);
    auto guest = sockets.open(fixture, "yk-guest");
    const auto book = [&](std::time_t at, const std::string& email) {
        Json payload = bookPayload(slug, at, "Гость");
        payload.set("guest_email", Json(email));
        return fixture.call(guest, "booking.book", payload);
    };
    const std::time_t before = std::time(nullptr);
    const Json bookedA = book(slotA, "yk-a@example.com");
    CHECK_EQ(bookedA.getString("type"), std::string("ok"));
    const std::string tokenA = integration::payloadOf(bookedA).getString("token");
    const Json paymentA = integration::payloadOf(bookedA).get("booking").get("payment");
    CHECK_EQ(paymentA.getString("status"), std::string("pending"));
    CHECK_EQ(paymentA.getString("method"), std::string("yookassa"));
    CHECK_EQ(paymentA.getString("kind"), std::string("full"));
    CHECK_EQ(paymentA.getInt("amount"), 250000LL);
    CHECK_EQ(paymentA.getString("action_url"), std::string("https://yoomoney.example/checkout/pay-1"));
    CHECK(!paymentA.contains("provider_id"));  // гостю — без id платежа
    std::time_t due = 0;
    CHECK(selfvoy::parseIsoUtc(paymentA.getString("due_at"), due) && due >= before + 3600 &&
          due <= std::time(nullptr) + 3600);
    const long long idA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenA))->id;
    const auto created = fake->of("POST", "/payments");
    CHECK_EQ(created.size(), std::size_t(1));
    if (!created.empty()) {
        const auto& call = created.front();
        CHECK_EQ(call.headers.at("Idempotence-Key"), "bk-" + std::to_string(idA));
        CHECK_EQ(call.body.get("amount").getString("value"), std::string("2500.00"));
        CHECK_EQ(call.body.get("amount").getString("currency"), std::string("RUB"));
        CHECK(call.body.getBool("capture"));
        CHECK_EQ(call.body.get("confirmation").getString("type"), std::string("redirect"));
        CHECK_EQ(call.body.get("confirmation").getString("return_url"),
                 "https://selfvoy.example.com/#/m/" + tokenA + "?paid=1");
        CHECK_EQ(call.body.get("metadata").getString("booking_id"), std::to_string(idA));
        CHECK(call.body.getString("description").rfind("Разбор, ", 0) == 0);
        CHECK(!call.body.contains("receipt"));  // чеки 54-ФЗ — «Мой налог» / настройки магазина
    }
    CHECK_EQ(db.findBooking(idA)->payment.getString("provider_id"), std::string("pay-1"));
    // Владелец видит оплату целиком.
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    CHECK(listed.size() == 1 && listed.at(0).get("payment").getString("provider_id") == "pay-1");
    // Письмо о записи — со ссылкой оплаты.
    const auto createdMails = mails("yk-a@example.com", "booking.created");
    CHECK_EQ(createdMails.size(), std::size_t(1));
    if (!createdMails.empty()) {
        CHECK(hasFooter(createdMails.front(), "Оплатить: https://yoomoney.example/checkout/pay-1"));
        CHECK(hasFooter(createdMails.front(), "Предоплата: 2 500 ₽ — оплатите в течение"));
    }

    // Планировщик: pending → ничего; повтор раньше 30 с — без запроса; succeeded → paid.
    const std::time_t now = std::time(nullptr);
    CHECK_EQ(bookings.paymentTick(100, now), std::size_t(1));
    CHECK_EQ(fake->of("GET", "/payments/pay-1").size(), std::size_t(1));
    CHECK_EQ(bookings.paymentTick(100, now + 10), std::size_t(1));
    CHECK_EQ(fake->of("GET", "/payments/pay-1").size(), std::size_t(1));
    fake->set("pay-1", "succeeded");
    CHECK_EQ(bookings.paymentTick(100, now + 31), std::size_t(1));
    CHECK_EQ(fake->of("GET", "/payments/pay-1").size(), std::size_t(2));
    auto stored = db.findBooking(idA);
    CHECK(stored && stored->payment.getString("status") == "paid" && !stored->payment.getString("paid_at").empty());
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "paid"), std::size_t(1));
    CHECK_EQ(mails("yk-a@example.com", "booking.paid").size(), std::size_t(1));
    CHECK_EQ(bookings.paymentTick(100, now + 62), std::size_t(0));  // оплачено — больше не опрашивается
    // «Я оплатил(а)» — только для оплаты по ссылке.
    CHECK_EQ(fixture.call(guest, "booking.paymentClaim", tokenPayload(tokenA)).getString("code"),
             std::string("bad_request"));

    // Гость отменяет до срока (cancel_hours = 0 — всегда) → возврат через ЮKassa.
    const Json cancelledA = fixture.call(guest, "booking.cancel", tokenPayload(tokenA));
    CHECK_EQ(cancelledA.getString("type"), std::string("ok"));
    const auto refunds = fake->of("POST", "/refunds");
    CHECK_EQ(refunds.size(), std::size_t(1));
    if (!refunds.empty()) {
        CHECK_EQ(refunds.front().headers.at("Idempotence-Key"), "rf-" + std::to_string(idA));
        CHECK_EQ(refunds.front().body.getString("payment_id"), std::string("pay-1"));
        CHECK_EQ(refunds.front().body.get("amount").getString("value"), std::string("2500.00"));
    }
    stored = db.findBooking(idA);
    CHECK(stored && stored->payment.getString("status") == "refunded" &&
          !stored->payment.getString("refunded_at").empty());
    CHECK_EQ(integration::payloadOf(cancelledA).get("booking").get("payment").getString("status"),
             std::string("refunded"));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "refunded"), std::size_t(1));
    const auto cancelMails = mails("yk-a@example.com", "booking.cancelled");
    CHECK(!cancelMails.empty() && hasFooter(cancelMails.back(), "Оплата 2 500 ₽ будет возвращена"));

    // ЮKassa отменила платёж (canceled) → booking.manage видит это сразу: запись отменена системой.
    const Json bookedB = book(slotB, "yk-b@example.com");
    const std::string tokenB = integration::payloadOf(bookedB).getString("token");
    fake->set("pay-2", "canceled");
    const Json managedB = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(tokenB))).get("booking");
    CHECK_EQ(managedB.getString("status"), std::string("cancelled"));
    CHECK_EQ(managedB.getString("cancelled_by"), std::string("system"));
    CHECK_EQ(managedB.get("payment").getString("status"), std::string("expired"));
    CHECK_EQ(mails("yk-b@example.com", "booking.payment_expired").size(), std::size_t(1));

    // Оплачено (manage после succeeded) → владелец отменяет → возврат rf-<id>.
    const Json bookedC = book(slotC, "yk-c@example.com");
    const std::string tokenC = integration::payloadOf(bookedC).getString("token");
    const long long idC = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenC))->id;
    fake->set("pay-3", "succeeded");
    const Json managedC = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(tokenC))).get("booking");
    CHECK_EQ(managedC.get("payment").getString("status"), std::string("paid"));
    Json ownerCancel = Json::object();
    ownerCancel.set("booking_id", Json(idC));
    const Json cancelledC = integration::payloadOf(fixture.call(owner, "booking.ownerCancel", ownerCancel));
    CHECK_EQ(cancelledC.get("booking").get("payment").getString("status"), std::string("refunded"));
    const auto refundsC = fake->of("POST", "/refunds");
    CHECK(refundsC.size() == 2 && refundsC.back().headers.at("Idempotence-Key") == "rf-" + std::to_string(idC));

    // ЮKassa не создала платёж — запись не держит время: отмена системой, гостю upstream_error.
    fake->failCreate = true;
    const Json failed = book(slotD, "yk-d@example.com");
    CHECK_EQ(failed.getString("code"), std::string("upstream_error"));
    bool systemCancelled = false;
    for (const auto& item : db.listBookings(ownerId, 0, false, 50)) {
        if (item.guestEmail == "yk-d@example.com") {
            systemCancelled = item.status == "cancelled" && item.cancelledBy == "system" &&
                              item.payment.getString("status") == "expired";
        }
    }
    CHECK(systemCancelled);
    CHECK(mails("yk-d@example.com", "booking.created").empty());
    fake->failCreate = false;

    CHECK(!integration::payloadOf(fixture.call(owner, "payments.disconnect")).getBool("connected", true));
    CHECK(!db.findPaymentAccount(ownerId));
}

// F4: оплата по ссылке — «Я оплатил(а)» (claimed не истекает), отметка владельцем,
// истечение срока оплаты → отмена системой (тик планировщика с подставленным now).
TEST(payments_link_claim_set_and_expiry) {
    using namespace payments_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB, slotC}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "link-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "linkowner@example.com", "Ирина"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    Json pageReq = payPage("Сессия");
    pageReq.set("price", Json(200000LL));
    pageReq.set("prepay", Json("deposit"));
    pageReq.set("deposit", Json(50000LL));
    pageReq.set("payment_url", Json("https://pay.example/irina"));
    pageReq.set("payment_note", Json("СБП по номеру +7 900 000-00-00"));
    pageReq.set("pay_within_minutes", Json(15LL));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const std::string slug = page.getString("slug");
    auto guest = sockets.open(fixture, "link-guest");
    const auto book = [&](std::time_t at, const std::string& email) {
        Json payload = bookPayload(slug, at, "Гость");
        payload.set("guest_email", Json(email));
        return integration::payloadOf(fixture.call(guest, "booking.book", payload));
    };
    const Json bookedA = book(slotA, "link-a@example.com");
    const std::string tokenA = bookedA.getString("token");
    const Json paymentA = bookedA.get("booking").get("payment");
    CHECK_EQ(paymentA.getString("method"), std::string("link"));
    CHECK_EQ(paymentA.getString("kind"), std::string("deposit"));
    CHECK_EQ(paymentA.getInt("amount"), 50000LL);  // депозит — один раз на запись
    CHECK_EQ(paymentA.getString("action_url"), std::string("https://pay.example/irina"));
    CHECK_EQ(bookedA.get("booking").getString("payment_note"), std::string("СБП по номеру +7 900 000-00-00"));
    const long long idA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenA))->id;
    const auto createdMails = mails("link-a@example.com", "booking.created");
    CHECK(!createdMails.empty() && hasFooter(createdMails.front(), "Депозит: 500 ₽ — оплатите в течение 15 мин") &&
          hasFooter(createdMails.front(), "Оплатить: https://pay.example/irina") &&
          hasFooter(createdMails.front(), "Как оплатить: СБП по номеру") &&
          hasFooter(createdMails.front(), "После оплаты нажмите «Я оплатил(а)»"));

    // «Я оплатил(а)»: pending → claimed, владельцу — booking.payment (claimed) один раз.
    const Json claimed = integration::payloadOf(fixture.call(guest, "booking.paymentClaim", tokenPayload(tokenA)));
    CHECK_EQ(claimed.get("booking").get("payment").getString("status"), std::string("claimed"));
    CHECK_EQ(fixture.call(guest, "booking.paymentClaim", tokenPayload(tokenA)).getString("type"), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "claimed"), std::size_t(1));
    CHECK_EQ(fixture.call(guest, "booking.paymentClaim", tokenPayload(growth_test::newToken())).getString("code"),
             std::string("not_found"));

    // Истечение: B не оплачена к сроку → отмена системой; claimed A не истекает.
    const Json bookedB = book(slotB, "link-b@example.com");
    const std::string tokenB = bookedB.getString("token");
    const long long idB = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenB))->id;
    const std::time_t now = std::time(nullptr);
    CHECK_EQ(bookings.paymentTick(100, now + 5 * 60), std::size_t(1));  // ещё в сроке
    CHECK_EQ(db.findBooking(idB)->status, std::string("active"));
    CHECK_EQ(bookings.paymentTick(100, now + 16 * 60), std::size_t(1));
    auto expired = db.findBooking(idB);
    CHECK(expired && expired->status == "cancelled" && expired->cancelledBy == "system" &&
          expired->payment.getString("status") == "expired");
    CHECK(db.listBookingReminders(idB).empty());
    CHECK_EQ(db.findBooking(idA)->status, std::string("active"));
    CHECK_EQ(db.findBooking(idA)->payment.getString("status"), std::string("claimed"));
    const auto expiredMails = mails("link-b@example.com", "booking.payment_expired");
    CHECK_EQ(expiredMails.size(), std::size_t(1));
    if (!expiredMails.empty()) {
        CHECK_EQ(expiredMails.front().subject, std::string("Selfvoy: оплата не поступила — запись отменена"));
        CHECK_EQ(expiredMails.front().linkUrl, "https://selfvoy.example.com/#/b/" + slug);
    }
    std::size_t systemCancels = 0;
    for (const auto& item : db.listNotifications(ownerId, false, 200)) {
        if (item.kind == "booking.cancelled" && item.payload.getString("cancelled_by") == "system") ++systemCancels;
    }
    CHECK_EQ(systemCancels, std::size_t(1));
    CHECK_EQ(bookings.paymentTick(100, now + 3600), std::size_t(0));  // повторно не отменяется
    CHECK_EQ(fixture.call(guest, "booking.paymentClaim", tokenPayload(tokenB)).getString("code"),
             std::string("conflict"));
    const Json managedB = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(tokenB))).get("booking");
    CHECK_EQ(managedB.getString("cancelled_by"), std::string("system"));

    // Владелец отмечает оплату: paid (гостю — «оплата получена»), неверный статус, чужая запись.
    Json set = Json::object();
    set.set("booking_id", Json(idA));
    set.set("status", Json("paid"));
    const Json paid = integration::payloadOf(fixture.call(owner, "booking.paymentSet", set)).get("booking");
    CHECK_EQ(paid.get("payment").getString("status"), std::string("paid"));
    CHECK(!paid.get("payment").getString("paid_at").empty());
    CHECK_EQ(mails("link-a@example.com", "booking.paid").size(), std::size_t(1));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "paid"), std::size_t(0));  // сам отметил
    Json badSet = set;
    badSet.set("status", Json("pending"));
    CHECK_EQ(fixture.call(owner, "booking.paymentSet", badSet).getString("code"), std::string("bad_request"));
    auto stranger = sockets.open(fixture, "link-stranger");
    fixture.registerUser(stranger, "linkstranger@example.com", "Чужой");
    CHECK_EQ(fixture.call(stranger, "booking.paymentSet", set).getString("code"), std::string("not_found"));

    // Отмена гостем до срока (cancel_hours = 0) оплаченной по ссылке — владельцу «верните вручную».
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(tokenA)).getString("type"), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "refund_due"), std::size_t(1));
    CHECK_EQ(db.findBooking(idA)->payment.getString("status"), std::string("paid"));  // вернёт — отметит refunded
    set.set("status", Json("refunded"));
    const Json refunded = integration::payloadOf(fixture.call(owner, "booking.paymentSet", set)).get("booking");
    CHECK(refunded.get("payment").getString("status") == "refunded" &&
          !refunded.get("payment").getString("refunded_at").empty());

    // waived: запись без оплаты — больше не истекает.
    const Json bookedC = book(slotC, "link-c@example.com");
    const long long idC = db.findBookingByToken(selfvoy::BookingManager::tokenHash(bookedC.getString("token")))->id;
    set.set("booking_id", Json(idC));
    set.set("status", Json("waived"));
    CHECK_EQ(fixture.call(owner, "booking.paymentSet", set).getString("type"), std::string("ok"));
    CHECK_EQ(bookings.paymentTick(100, now + 3600), std::size_t(0));
    CHECK_EQ(db.findBooking(idC)->status, std::string("active"));
}

// F4: политика отмены — late_cancel=block запрещает отмену и перенос позже срока;
// решение о возврате: до срока — возврат, позже — оплата остаётся специалисту.
TEST(payments_late_cancel_block_and_refund_decision) {
    using namespace payments_test;
    const std::time_t soon = invite_test::future(30);   // меньше 48 ч — поздно
    const std::time_t later = invite_test::future(60);  // больше 48 ч
    const std::time_t soon2 = invite_test::future(31);
    const std::time_t later2 = invite_test::future(61);
    invite_test::FakeAi ai([&] { return slotsBody({soon, soon2, later, later2}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "late-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "lateowner@example.com", "Анна"));
    auto& db = fixture.server->database().db();
    Json blockReq = payPage("Строго");
    blockReq.set("cancel_hours", Json(48LL));
    blockReq.set("late_cancel", Json("block"));
    const std::string blockSlug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", blockReq)).get("page").getString("slug");
    auto guest = sockets.open(fixture, "late-guest");
    const auto book = [&](const std::string& slug, std::time_t at, const std::string& email) {
        Json payload = bookPayload(slug, at, "Гость");
        payload.set("guest_email", Json(email));
        return integration::payloadOf(fixture.call(guest, "booking.book", payload)).getString("token");
    };
    const std::string lateToken = book(blockSlug, soon, "late-1@example.com");
    const Json lateView = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(lateToken))).get("booking");
    CHECK(lateView.getBool("late"));
    CHECK(!lateView.getBool("can_cancel", true));
    CHECK(!lateView.getBool("can_reschedule", true));
    CHECK_EQ(lateView.get("policy").getInt("cancel_hours"), 48LL);
    CHECK(lateView.get("payment").isNull());  // без предоплаты оплаты нет
    const Json refused = fixture.call(guest, "booking.cancel", tokenPayload(lateToken));
    CHECK_EQ(refused.getString("code"), std::string("conflict"));
    CHECK_EQ(refused.getString("message"), std::string("до встречи меньше 48 ч — свяжитесь со специалистом"));
    Json move = tokenPayload(lateToken);
    move.set("start", Json(selfvoy::isoUtc(later2)));
    CHECK_EQ(fixture.call(guest, "booking.reschedule", move).getString("code"), std::string("conflict"));
    CHECK_EQ(db.findBookingByToken(selfvoy::BookingManager::tokenHash(lateToken))->status, std::string("active"));
    // Владелец отменяет всегда.
    Json ownerCancel = Json::object();
    ownerCancel.set("booking_id", Json(db.findBookingByToken(selfvoy::BookingManager::tokenHash(lateToken))->id));
    CHECK_EQ(fixture.call(owner, "booking.ownerCancel", ownerCancel).getString("type"), std::string("ok"));
    const std::string earlyToken = book(blockSlug, later, "late-2@example.com");
    const Json earlyView = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(earlyToken))).get("booking");
    CHECK(!earlyView.getBool("late", true));
    CHECK(earlyView.getBool("can_cancel"));
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(earlyToken)).getString("type"), std::string("ok"));

    // late_cancel=allow + оплата по ссылке: поздняя отмена разрешена, но без возврата.
    Json allowReq = payPage("Мягко");
    allowReq.set("cancel_hours", Json(48LL));
    allowReq.set("price", Json(300000LL));
    allowReq.set("prepay", Json("full"));
    allowReq.set("payment_note", Json("Перевод на карту 2200 0000 0000 0000"));
    const std::string allowSlug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", allowReq)).get("page").getString("slug");
    const auto paidBooking = [&](std::time_t at, const std::string& email) {
        const std::string token = book(allowSlug, at, email);
        const long long id = db.findBookingByToken(selfvoy::BookingManager::tokenHash(token))->id;
        Json set = Json::object();
        set.set("booking_id", Json(id));
        set.set("status", Json("paid"));
        fixture.call(owner, "booking.paymentSet", set);
        return token;
    };
    const std::string keptToken = paidBooking(soon2, "late-3@example.com");
    const Json keptView = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(keptToken))).get("booking");
    CHECK(keptView.getBool("late") && keptView.getBool("can_cancel"));
    CHECK_EQ(keptView.get("payment").getString("action_url"), std::string());  // реквизиты — в payment_note
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(keptToken)).getString("type"), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "refund_due"), std::size_t(0));
    const auto keptMails = mails("late-3@example.com", "booking.cancelled");
    CHECK(!keptMails.empty() && hasFooter(keptMails.back(), "Отмена позже чем за 48 ч до встречи — оплата не возвращается."));
    CHECK_EQ(db.findBookingByToken(selfvoy::BookingManager::tokenHash(keptToken))->payment.getString("status"),
             std::string("paid"));
    const std::string refundToken = paidBooking(later2, "late-4@example.com");
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(refundToken)).getString("type"), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "booking.payment", "refund_due"), std::size_t(1));
    const auto refundMails = mails("late-4@example.com", "booking.cancelled");
    CHECK(!refundMails.empty() && hasFooter(refundMails.back(), "Организатор вернёт оплату 3 000 ₽."));
}

// PostgreSQL (S2a): поля страницы F4, оплата записи, ожидающие оплаты, магазин ЮKassa.
TEST(pg_payments_methods) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto db = selfvoy::makePostgresDatabase(url);
    std::string error;
    CHECK(db && db->connect(error));
    if (!db) return;
    long long owner = 0;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    CHECK(db->createUser("pg-pay-" + suffix + "@example.com", "argon2id$x", "Оплата", owner).ok);
    selfvoy::BookingPageRecord page;
    page.ownerId = owner;
    page.slug = "pay" + suffix + "00";
    page.slug.resize(10, '0');
    page.title = "Оплата";
    page.price = 250000;
    page.currency = "EUR";
    page.prepay = "deposit";
    page.deposit = 1000;
    page.paymentUrl = "https://pay.example/x";
    page.paymentNote = "СБП";
    page.payWithinMinutes = 45;
    page.cancelHours = 24;
    page.lateCancel = "block";
    page.policyText = "Политика";
    long long pageId = 0;
    CHECK(db->createBookingPage(page, pageId).ok);
    auto stored = db->findBookingPage(pageId);
    CHECK(stored && stored->price == 250000 && stored->currency == "EUR" && stored->prepay == "deposit" &&
          stored->deposit == 1000 && stored->paymentUrl == "https://pay.example/x" && stored->paymentNote == "СБП" &&
          stored->payWithinMinutes == 45 && stored->cancelHours == 24 && stored->lateCancel == "block" &&
          stored->policyText == "Политика");
    if (stored) {
        stored->prepay = "none";
        stored->cancelHours = 0;
        CHECK(db->updateBookingPage(*stored).ok);
        CHECK_EQ(db->findBookingPage(pageId)->prepay, std::string("none"));
    }
    stored->currency = "GBP";  // CHECK в схеме
    CHECK(!db->updateBookingPage(*stored).ok);

    const auto makeBooking = [&](const std::string& dueAt) {
        selfvoy::BookingRecord booking;
        booking.pageId = pageId;
        booking.ownerId = owner;
        booking.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
        booking.guestName = "Гость";
        Json payment = Json::object();
        payment.set("status", Json("pending"));
        payment.set("amount", Json(1000LL));
        payment.set("due_at", Json(dueAt));
        booking.payment = payment;
        long long id = 0;
        CHECK(db->createBooking(booking, id).ok);
        return id;
    };
    const long long later = makeBooking("2099-01-02T00:00:00Z");
    const long long sooner = makeBooking("2000-01-01T00:00:00Z");
    auto awaiting = db->listAwaitingPayments(1000);
    std::vector<long long> order;
    for (const auto& item : awaiting) {
        if (item.id == later || item.id == sooner) order.push_back(item.id);
    }
    CHECK(order.size() == 2 && order.front() == sooner);
    auto record = db->findBooking(sooner);
    CHECK(record && record->payment.getInt("amount") == 1000);
    if (record) {
        Json payment = record->payment;
        payment.set("status", Json("paid"));
        record->payment = payment;
        CHECK(db->updateBooking(*record).ok);
        CHECK_EQ(db->findBooking(sooner)->payment.getString("status"), std::string("paid"));
        record->payment = Json();
        CHECK(db->updateBooking(*record).ok);
        CHECK(db->findBooking(sooner)->payment.isNull());
    }
    for (const auto& item : db->listAwaitingPayments(1000)) CHECK(item.id != sooner);

    CHECK(!db->findPaymentAccount(owner));
    selfvoy::PaymentAccountRecord account;
    account.userId = owner;
    account.shopId = "100500";
    account.secretEnc = "sealed-1";
    CHECK(db->setPaymentAccount(account).ok);
    account.secretEnc = "sealed-2";
    CHECK(db->setPaymentAccount(account).ok);
    const auto found = db->findPaymentAccount(owner);
    CHECK(found && found->shopId == "100500" && found->secretEnc == "sealed-2" && found->provider == "yookassa" &&
          !found->connectedAt.empty());
    CHECK(db->deletePaymentAccount(owner).ok);
    CHECK(!db->findPaymentAccount(owner));
}

// ------------------------------------------- F5: пакеты (абонементы) и второй получатель

namespace packages_test {

using namespace payments_test;

// Сессия с собственным адресом гостя: лимиты гостевых методов считаются по IP.
std::shared_ptr<selfvoy::Session> openFrom(Fixture& fixture, admin_test::Sockets& sockets, const std::string& id,
                                           const std::string& address) {
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return nullptr;
    sockets.fds.push_back(pair[0]);
    sockets.fds.push_back(pair[1]);
    return std::make_shared<selfvoy::Session>(pair[1], id, address, fixture.config);
}

Json offer(const std::string& title, long long sessions, long long price, long long validDays = 0) {
    Json json = Json::object();
    json.set("title", Json(title));
    json.set("sessions", Json(sessions));
    json.set("price", Json(price));
    if (validDays > 0) json.set("valid_days", Json(validDays));
    return json;
}

Json offersPage(const std::string& title, const Json& offers) {
    Json payload = payPage(title);
    payload.set("packages", offers);
    return payload;
}

Json packageIdPayload(long long id) {
    Json payload = Json::object();
    payload.set("package_id", Json(id));
    return payload;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Писем вида kind на адрес (без учёта регистра).
std::size_t mailCount(const std::string& to, const std::string& kind) {
    std::size_t count = 0;
    const std::string wanted = lower(to);
    for (const auto& mail : settledOutbox()) {
        if (lower(mail.to) == wanted && mail.kind == kind) ++count;
    }
    return count;
}

std::optional<selfvoy::MailNotice> lastMail(const std::string& to, const std::string& kind) {
    const auto found = mails(to, kind);
    if (found.empty()) return std::nullopt;
    return found.back();
}

bool footerHas(const selfvoy::MailNotice& mail, const std::string& part) {
    return std::any_of(mail.footer.begin(), mail.footer.end(),
                       [&](const std::string& line) { return line.find(part) != std::string::npos; });
}

// Последнее уведомление владельца вида kind (payload) или null.
Json lastNotice(selfvoy::IDatabase& db, long long ownerId, const std::string& kind) {
    for (const auto& item : db.listNotifications(ownerId, false, 200)) {
        if (item.kind == kind) return item.payload;  // новые сверху
    }
    return Json();
}

}  // namespace packages_test

// F5: предложения пакетов страницы — проверка полей, сохранение id при pageUpdate, вид гостя.
TEST(packages_offers_validation_and_ids) {
    using namespace packages_test;
    Fixture fixture;
    enablePayments(fixture, "");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pk-offers-owner");
    fixture.registerUser(owner, "pkoffers@example.com", "Ольга");
    const auto code = [&](const Json& packages) {
        return fixture.call(owner, "booking.pageCreate", offersPage("Пакеты", packages)).getString("code");
    };
    const auto one = [](const Json& item) {
        Json list = Json::array();
        list.push(item);
        return list;
    };
    const std::string bad = "bad_request";
    CHECK_EQ(code(Json("пять занятий")), bad);
    Json six = Json::array();
    for (int i = 0; i < 6; ++i) six.push(offer("Пакет " + std::to_string(i), 5, 100000));
    CHECK_EQ(code(six), bad);
    CHECK_EQ(code(one(offer("", 5, 100000))), bad);
    CHECK_EQ(code(one(offer(std::string(81, 'a'), 5, 100000))), bad);
    CHECK_EQ(code(one(offer("Две\nстроки", 5, 100000))), bad);
    CHECK_EQ(code(one(offer("Пакет", 1, 100000))), bad);
    CHECK_EQ(code(one(offer("Пакет", 101, 100000))), bad);
    CHECK_EQ(code(one(offer("Пакет", 5, 0))), bad);
    CHECK_EQ(code(one(offer("Пакет", 5, 100000001))), bad);
    CHECK_EQ(code(one(offer("Пакет", 5, 100000, 6))), bad);
    CHECK_EQ(code(one(offer("Пакет", 5, 100000, 366))), bad);
    CHECK_EQ(code(one(Json("строка"))), bad);
    Json withId = offer("Пакет", 5, 100000);
    withId.set("id", Json(7LL));
    CHECK_EQ(code(one(withId)), bad);  // у новой страницы нет прежних предложений
    Json noSessions = offer("Пакет", 5, 100000);
    noSessions.erase("sessions");
    CHECK_EQ(code(one(noSessions)), bad);

    Json offers = Json::array();
    offers.push(offer("  5 занятий  ", 5, 1000000));
    offers.push(offer("10 занятий", 10, 1800000, 180));
    const Json created =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", offersPage("Пакеты", offers))).get("page");
    const Json stored = created.get("packages");
    CHECK_EQ(stored.size(), std::size_t(2));
    CHECK(stored.at(0).getInt("id") == 1 && stored.at(1).getInt("id") == 2);
    CHECK_EQ(stored.at(0).getString("title"), std::string("5 занятий"));
    CHECK(stored.at(0).getInt("valid_days") == 90 && stored.at(1).getInt("valid_days") == 180);
    CHECK(stored.at(0).getInt("sessions") == 5 && stored.at(0).getInt("price") == 1000000);
    const long long pageId = created.getInt("id");

    // Правка целым массивом: элемент с id сохраняет id, без id — новый (больше прежних).
    Json update = Json::object();
    update.set("page_id", Json(pageId));
    Json kept = offer("10 занятий+", 10, 1900000);
    kept.set("id", Json(2LL));
    Json next = Json::array();
    next.push(kept);
    next.push(offer("3 занятия", 3, 500000));
    update.set("packages", next);
    const Json updated = integration::payloadOf(fixture.call(owner, "booking.pageUpdate", update)).get("page");
    const Json after = updated.get("packages");
    CHECK_EQ(after.size(), std::size_t(2));
    CHECK(after.at(0).getInt("id") == 2 && after.at(0).getString("title") == "10 занятий+" &&
          after.at(0).getInt("price") == 1900000);
    CHECK_EQ(after.at(1).getInt("id"), 3LL);
    // Удалённое предложение (id 1) больше не известно; повтор id — ошибка.
    Json gone = offer("5 занятий", 5, 1000000);
    gone.set("id", Json(1LL));
    update.set("packages", one(gone));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", update).getString("code"), bad);
    Json duplicates = Json::array();
    duplicates.push(kept);
    duplicates.push(kept);
    update.set("packages", duplicates);
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", update).getString("code"), bad);
    // Правка без packages их не трогает.
    Json rename = Json::object();
    rename.set("page_id", Json(pageId));
    rename.set("title", Json("Абонементы"));
    const Json renamed = integration::payloadOf(fixture.call(owner, "booking.pageUpdate", rename)).get("page");
    CHECK_EQ(renamed.get("packages").size(), std::size_t(2));

    // Гость видит предложения в booking.get.
    auto guest = sockets.open(fixture, "pk-offers-guest");
    const Json view =
        integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(created.getString("slug")))).get("page");
    CHECK_EQ(view.get("packages").size(), std::size_t(2));
    CHECK_EQ(view.get("packages").at(1).getString("title"), std::string("3 занятия"));

    // Пустой массив — предложений нет.
    update.set("packages", Json::array());
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "booking.pageUpdate", update)).get("page").get("packages").size(),
             std::size_t(0));
}

// F5: при SELFVOY_PLANS_ENFORCED=1 непустой packages — только Pro; уже включённые можно править.
TEST(packages_pro_gating_enforced) {
    using namespace packages_test;
    Fixture fixture;
    enablePayments(fixture, "", true);
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pk-pro-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "pkpro@example.com", "Пётр"));
    Json offers = Json::array();
    offers.push(offer("4 занятия", 4, 400000));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", offersPage("Пакеты", offers)).getString("code"),
             std::string("pro_required"));
    Json plain = payPage("Без пакетов");
    plain.set("packages", Json::array());  // пустой массив — бесплатно
    const Json page = fixture.call(owner, "booking.pageCreate", plain);
    CHECK_EQ(page.getString("type"), std::string("ok"));
    Json enable = Json::object();
    enable.set("page_id", Json(integration::payloadOf(page).get("page").getInt("id")));
    enable.set("packages", offers);
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("code"), std::string("pro_required"));

    CHECK(fixture.server->database().db().setUserPlanDays(ownerId, 30).ok);
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", offersPage("Пакеты", offers)).getString("type"), std::string("ok"));
    const Json enabled = fixture.call(owner, "booking.pageUpdate", enable);
    CHECK_EQ(enabled.getString("type"), std::string("ok"));
    // Pro закончился: уже включённые пакеты можно править.
    CHECK(fixture.server->database().db().setUserPlanDays(ownerId, 0).ok);
    Json edited = integration::payloadOf(enabled).get("page").get("packages");
    Json first = edited.at(0);
    first.set("price", Json(450000LL));
    Json list = Json::array();
    list.push(first);
    enable.set("packages", list);
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("type"), std::string("ok"));
    selfvoy::plan::setEnforced(false);
}

// F5: покупка пакета при записи без способа оплаты (сразу active), запись по package_token,
// остаток и «осталось N», чужая страница, второй получатель, возврат занятия до срока и
// сгорание при поздней отмене, отмена владельцем, истечение valid_until (тик с подставленным now).
TEST(packages_purchase_token_booking_return_and_expiry) {
    using namespace packages_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(5);   // позже cancel_hours (24 ч)
    const std::time_t slotD = invite_test::future(78);
    const std::time_t slotE = invite_test::future(102);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB, slotC, slotD, slotE}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pk-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "pkowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    Json offers = Json::array();
    offers.push(offer("3 занятия", 3, 900000));
    Json pageReq = offersPage("Тренировки", offers);
    pageReq.set("cancel_hours", Json(24LL));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const std::string slug = page.getString("slug");
    const std::string other =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", payPage("Другая"))).get("page").getString("slug");
    auto guest = sockets.open(fixture, "pk-guest");

    // Ошибки запроса: и package_id, и package_token; неизвестный пакет; встреч больше, чем в пакете.
    Json both = bookPayload(slug, slotA, "Мария");
    both.set("package_id", Json(1LL));
    both.set("package_token", Json(growth_test::newToken()));
    CHECK_EQ(fixture.call(guest, "booking.book", both).getString("code"), std::string("bad_request"));
    Json unknown = bookPayload(slug, slotA, "Мария");
    unknown.set("package_id", Json(99LL));
    CHECK_EQ(fixture.call(guest, "booking.book", unknown).getString("code"), std::string("bad_request"));
    Json strangerToken = bookPayload(slug, slotA, "Мария");
    strangerToken.set("package_token", Json(growth_test::newToken()));
    CHECK_EQ(fixture.call(guest, "booking.book", strangerToken).getString("code"), std::string("not_found"));

    // Покупка: страница без способа оплаты — пакет сразу active, запись списывает занятие.
    const std::time_t before = std::time(nullptr);
    Json buy = bookPayload(slug, slotA, "Мария");
    buy.set("guest_email", Json("pk-maria@example.com"));
    buy.set("cc_email", Json("pk-mama@example.com"));
    buy.set("package_id", Json(1LL));
    const Json bought = fixture.call(guest, "booking.book", buy);
    CHECK_EQ(bought.getString("type"), std::string("ok"));
    const Json boughtPayload = integration::payloadOf(bought);
    const std::string packageToken = boughtPayload.getString("package_token");
    CHECK_EQ(packageToken.size(), std::size_t(43));
    CHECK_EQ(boughtPayload.getString("package_url"), "https://selfvoy.example.com/#/p/" + packageToken);
    const Json pack = boughtPayload.get("package");
    CHECK_EQ(pack.getString("status"), std::string("active"));
    CHECK_EQ(pack.getInt("sessions_total"), 3LL);
    CHECK_EQ(pack.getInt("sessions_left"), 2LL);
    CHECK_EQ(pack.get("payment").getString("status"), std::string("pending"));  // отметит владелец
    CHECK_EQ(pack.get("payment").getString("kind"), std::string("package"));
    CHECK_EQ(pack.get("payment").getString("due_at"), std::string());
    std::time_t until = 0;
    CHECK(selfvoy::parseIsoUtc(pack.getString("valid_until"), until) && until >= before + 90 * 86400 &&
          until <= std::time(nullptr) + 90 * 86400);
    CHECK_EQ(boughtPayload.get("booking").getString("cc_email"), std::string("p***@example.com"));
    CHECK(boughtPayload.get("booking").getBool("by_package"));
    CHECK(boughtPayload.get("booking").get("payment").isNull());  // запись оплачена пакетом
    auto purchase = db.findPackagePurchaseByToken(selfvoy::BookingManager::tokenHash(packageToken));
    CHECK(purchase && purchase->sessionsUsed == 1 && purchase->ccEmail == "pk-mama@example.com" &&
          purchase->offerId == 1 && purchase->price == 900000);
    const long long packageId = purchase ? purchase->id : 0;
    // Токен ссылки пакета — только sha256 и шифр (для писем).
    CHECK(purchase && purchase->tokenEnc.find(packageToken) == std::string::npos &&
          bookings.packageTokenOf(*purchase) == packageToken);
    const long long bookingA =
        db.findBookingByToken(selfvoy::BookingManager::tokenHash(boughtPayload.getString("token")))->id;
    CHECK_EQ(db.findBooking(bookingA)->packageId, packageId);
    const Json owned = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    CHECK(owned.size() == 1 && owned.at(0).getString("cc_email") == "pk-mama@example.com" &&
          owned.at(0).getInt("package_id") == packageId);
    CHECK_EQ(ownerNotices(db, ownerId, "package.purchased"), std::size_t(1));
    // Письмо о записи — со ссылкой пакета; копия — второму получателю.
    const auto createdMail = lastMail("pk-maria@example.com", "booking.created");
    CHECK(createdMail && footerHas(*createdMail, "Абонемент «3 занятия»: 3 занятия, 9 000 ₽.") &&
          footerHas(*createdMail, "https://selfvoy.example.com/#/p/" + packageToken) &&
          footerHas(*createdMail, "Оплату пакета (9 000 ₽) примет организатор."));
    CHECK_EQ(mailCount("pk-mama@example.com", "booking.created"), std::size_t(1));

    // Гость открывает ссылку пакета.
    const Json viewed = integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(packageToken))).get("package");
    CHECK_EQ(viewed.getString("title"), std::string("3 занятия"));
    CHECK_EQ(viewed.getInt("sessions_left"), 2LL);
    CHECK_EQ(viewed.getString("page_slug"), slug);
    CHECK_EQ(viewed.getString("owner_name"), std::string("Ольга"));
    CHECK(viewed.get("bookings").size() == 1 && viewed.get("bookings").at(0).getString("status") == "active");
    CHECK(viewed.get("branding").getBool("show"));
    CHECK_EQ(viewed.getString("cc_email"), std::string("p***@example.com"));
    CHECK_EQ(fixture.call(guest, "package.get", tokenPayload(growth_test::newToken())).getString("code"),
             std::string("not_found"));

    // Запись по пакету: имя и адреса — из пакета; остаток 1 → «продлите» гостю и копии.
    const auto byToken = [&](std::time_t at) {
        Json payload = slugPayload(slug);
        payload.set("start", Json(selfvoy::isoUtc(at)));
        payload.set("package_token", Json(packageToken));
        return fixture.call(guest, "booking.book", payload);
    };
    const Json bookedB = byToken(slotB);
    CHECK_EQ(bookedB.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(bookedB).get("booking").getString("guest_name"), std::string("Мария"));
    CHECK_EQ(integration::payloadOf(bookedB).get("package").getInt("sessions_left"), 1LL);
    const std::string tokenB = integration::payloadOf(bookedB).getString("token");
    const auto storedB = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenB));
    CHECK(storedB && storedB->guestEmail == "pk-maria@example.com" && storedB->ccEmail == "pk-mama@example.com" &&
          storedB->packageId == packageId);
    const auto lowMail = lastMail("pk-maria@example.com", "package.low");
    CHECK(lowMail && lowMail->subject == "Selfvoy: осталось 1 занятие по абонементу" &&
          lowMail->lead == "Осталось 1 занятие — продлите абонемент: https://selfvoy.example.com/#/b/" + slug);
    CHECK_EQ(mailCount("pk-mama@example.com", "package.low"), std::size_t(1));
    CHECK_EQ(ownerNotices(db, ownerId, "package.low"), std::size_t(1));
    CHECK_EQ(lastNotice(db, ownerId, "package.low").getInt("left"), 1LL);
    CHECK_EQ(lastNotice(db, ownerId, "package.low").getInt("package_id"), packageId);
    const auto createdB = lastMail("pk-maria@example.com", "booking.created");
    CHECK(createdB && footerHas(*createdB, "Списано с абонемента «3 занятия»: осталось 1 из 3."));

    // Чужая страница — нельзя.
    Json elsewhere = slugPayload(other);
    elsewhere.set("start", Json(selfvoy::isoUtc(slotD)));
    elsewhere.set("package_token", Json(packageToken));
    CHECK_EQ(fixture.call(guest, "booking.book", elsewhere).getString("code"), std::string("bad_request"));

    // Последнее занятие → used (и ещё одно «осталось 0»); дальше — conflict.
    const Json bookedC = byToken(slotC);
    CHECK_EQ(bookedC.getString("type"), std::string("ok"));
    CHECK_EQ(db.findPackagePurchase(packageId)->status, std::string("used"));
    CHECK_EQ(ownerNotices(db, ownerId, "package.low"), std::size_t(2));
    const Json refused = byToken(slotD);
    CHECK_EQ(refused.getString("code"), std::string("conflict"));
    CHECK_EQ(refused.getString("message"), std::string("занятия по абонементу закончились"));

    // Поздняя отмена (до встречи меньше 24 ч) — занятие сгорает.
    const std::string tokenC = integration::payloadOf(bookedC).getString("token");
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(tokenC)).getString("type"), std::string("ok"));
    auto stored = db.findPackagePurchase(packageId);
    CHECK(stored && stored->sessionsUsed == 3 && stored->status == "used");
    const auto lateMail = lastMail("pk-maria@example.com", "booking.cancelled");
    CHECK(lateMail && footerHas(*lateMail, "занятие списано с абонемента"));
    CHECK_EQ(mailCount("pk-mama@example.com", "booking.cancelled"), std::size_t(1));  // копия отмены
    // Отмена до срока — занятие возвращается, пакет снова active.
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(tokenB)).getString("type"), std::string("ok"));
    stored = db.findPackagePurchase(packageId);
    CHECK(stored && stored->sessionsUsed == 2 && stored->status == "active");
    const auto returnMail = lastMail("pk-maria@example.com", "booking.cancelled");
    CHECK(returnMail && footerHas(*returnMail, "Занятия возвращены на абонемент: 1."));
    // Владелец отменяет запись A — занятие возвращается всегда.
    Json ownerCancel = Json::object();
    ownerCancel.set("booking_id", Json(bookingA));
    CHECK_EQ(fixture.call(owner, "booking.ownerCancel", ownerCancel).getString("type"), std::string("ok"));
    CHECK_EQ(db.findPackagePurchase(packageId)->sessionsUsed, 1);
    const Json listedView = integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(packageToken))).get("package");
    CHECK_EQ(listedView.getInt("sessions_left"), 2LL);
    CHECK_EQ(listedView.get("bookings").size(), std::size_t(3));

    // Истечение: планировщик переводит active → expired по valid_until (подставленное now).
    const std::time_t now = std::time(nullptr);
    CHECK_EQ(bookings.packageTick(100, now + 89 * 86400), std::size_t(0));
    CHECK_EQ(db.findPackagePurchase(packageId)->status, std::string("active"));
    CHECK_EQ(bookings.packageTick(100, now + 91 * 86400), std::size_t(1));
    CHECK_EQ(db.findPackagePurchase(packageId)->status, std::string("expired"));
    CHECK_EQ(bookings.packageTick(100, now + 92 * 86400), std::size_t(0));
    const Json expired = byToken(slotE);
    CHECK_EQ(expired.getString("code"), std::string("conflict"));
    CHECK_EQ(expired.getString("message"), std::string("срок действия абонемента истёк"));
}

// F5: покупка через ЮKassa — pending (POST /payments pk-<id>, возврат на #/p/<токен>?paid=1),
// запись по неоплаченному пакету нельзя, succeeded → active (тик), неоплаченный к сроку →
// покупка cancelled и запись отменена системой, оплата при package.get, возврат pr-<id>.
TEST(packages_yookassa_purchase_paid_expired_refund) {
    using namespace packages_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    const std::time_t slotD = invite_test::future(102);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB, slotC, slotD}); });
    Fixture fixture;
    FakeYooKassa* fake = enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pk-yk-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "pkykowner@example.com", "Ирина"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    CHECK_EQ(fixture.call(owner, "payments.connect", connectPayload(kShop, kSecret)).getString("type"), std::string("ok"));
    Json offers = Json::array();
    offers.push(offer("4 занятия", 4, 1200000));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", offersPage("Йога", offers))).get("page");
    const std::string slug = page.getString("slug");
    auto guest = sockets.open(fixture, "pk-yk-guest");
    const auto buy = [&](std::time_t at, const std::string& email) {
        Json payload = bookPayload(slug, at, "Гость");
        payload.set("guest_email", Json(email));
        payload.set("package_id", Json(1LL));
        return integration::payloadOf(fixture.call(guest, "booking.book", payload));
    };
    const Json boughtA = buy(slotA, "pk-yk-a@example.com");
    const std::string tokenA = boughtA.getString("package_token");
    const Json packA = boughtA.get("package");
    CHECK_EQ(packA.getString("status"), std::string("pending"));
    CHECK_EQ(packA.getString("valid_until"), std::string());
    CHECK_EQ(packA.get("payment").getString("method"), std::string("yookassa"));
    CHECK_EQ(packA.get("payment").getInt("amount"), 1200000LL);
    CHECK_EQ(packA.get("payment").getString("action_url"), std::string("https://yoomoney.example/checkout/pay-1"));
    CHECK(!packA.get("payment").contains("provider_id"));
    const long long idA = db.findPackagePurchaseByToken(selfvoy::BookingManager::tokenHash(tokenA))->id;
    const auto created = fake->of("POST", "/payments");
    CHECK_EQ(created.size(), std::size_t(1));
    if (!created.empty()) {
        CHECK_EQ(created.front().headers.at("Idempotence-Key"), "pk-" + std::to_string(idA));
        CHECK_EQ(created.front().body.get("amount").getString("value"), std::string("12000.00"));
        CHECK_EQ(created.front().body.get("confirmation").getString("return_url"),
                 "https://selfvoy.example.com/#/p/" + tokenA + "?paid=1");
        CHECK_EQ(created.front().body.get("metadata").getString("package_id"), std::to_string(idA));
        CHECK_EQ(created.front().body.getString("description"), std::string("Йога: 4 занятия"));
    }
    CHECK_EQ(db.findPackagePurchase(idA)->payment.getString("provider_id"), std::string("pay-1"));
    const auto mailA = lastMail("pk-yk-a@example.com", "booking.created");
    CHECK(mailA && footerHas(*mailA, "Оплатить: https://yoomoney.example/checkout/pay-1") &&
          footerHas(*mailA, "Оплата пакета: 12 000 ₽ — оплатите в течение") &&
          footerHas(*mailA, "Срок действия — 90 дней с момента оплаты."));
    // Пока не оплачен — по пакету не записаться.
    Json early = slugPayload(slug);
    early.set("start", Json(selfvoy::isoUtc(slotB)));
    early.set("package_token", Json(tokenA));
    const Json notPaid = fixture.call(guest, "booking.book", early);
    CHECK_EQ(notPaid.getString("code"), std::string("conflict"));
    CHECK_EQ(notPaid.getString("message"), std::string("абонемент ещё не оплачен"));

    // Тик: pending — опрос; succeeded → active, срок действия — с момента оплаты.
    const std::time_t now = std::time(nullptr);
    CHECK_EQ(bookings.packageTick(100, now), std::size_t(1));
    CHECK_EQ(fake->of("GET", "/payments/pay-1").size(), std::size_t(1));
    CHECK_EQ(bookings.packageTick(100, now + 10), std::size_t(1));
    CHECK_EQ(fake->of("GET", "/payments/pay-1").size(), std::size_t(1));  // не чаще раза в 30 с
    fake->set("pay-1", "succeeded");
    CHECK_EQ(bookings.packageTick(100, now + 31), std::size_t(1));
    auto storedA = db.findPackagePurchase(idA);
    CHECK(storedA && storedA->status == "active" && storedA->payment.getString("status") == "paid" &&
          storedA->validUntil == selfvoy::isoUtc(now + 31 + 90 * 86400));
    CHECK_EQ(ownerNotices(db, ownerId, "package.payment", "paid"), std::size_t(1));
    const auto paidMail = lastMail("pk-yk-a@example.com", "package.paid");
    CHECK(paidMail && paidMail->linkUrl == "https://selfvoy.example.com/#/p/" + tokenA);
    CHECK_EQ(fixture.call(guest, "booking.book", early).getString("type"), std::string("ok"));
    CHECK_EQ(db.findPackagePurchase(idA)->sessionsUsed, 2);
    // «Я оплатил(а)» — только для оплаты по ссылке.
    CHECK_EQ(fixture.call(guest, "package.paymentClaim", tokenPayload(tokenA)).getString("code"),
             std::string("bad_request"));

    // Не оплачен к сроку → покупка cancelled, её запись отменена системой.
    const Json boughtC = buy(slotC, "pk-yk-c@example.com");
    const std::string tokenC = boughtC.getString("package_token");
    const long long idC = db.findPackagePurchaseByToken(selfvoy::BookingManager::tokenHash(tokenC))->id;
    const long long bookingC = db.findBookingByToken(selfvoy::BookingManager::tokenHash(boughtC.getString("token")))->id;
    CHECK_EQ(bookings.packageTick(100, now + 2 * 3600), std::size_t(1));
    auto storedC = db.findPackagePurchase(idC);
    CHECK(storedC && storedC->status == "cancelled" && storedC->payment.getString("status") == "expired");
    auto cancelledC = db.findBooking(bookingC);
    CHECK(cancelledC && cancelledC->status == "cancelled" && cancelledC->cancelledBy == "system");
    CHECK(db.listBookingReminders(bookingC).empty());
    CHECK_EQ(ownerNotices(db, ownerId, "package.payment", "expired"), std::size_t(1));
    CHECK_EQ(lastNotice(db, ownerId, "booking.cancelled").getString("cancelled_by"), std::string("system"));
    const auto expiredMail = lastMail("pk-yk-c@example.com", "package.payment_expired");
    CHECK(expiredMail && expiredMail->linkUrl == "https://selfvoy.example.com/#/b/" + slug);
    CHECK_EQ(bookings.packageTick(100, now + 3 * 3600), std::size_t(0));  // повторно не отменяется

    // Оплата видна при открытии ссылки пакета (гость вернулся с ?paid=1); владелец отменил
    // единственную запись (занятие вернулось) и сам пакет — возврат всей суммы pr-<id>.
    const Json boughtD = buy(slotD, "pk-yk-d@example.com");
    const std::string tokenD = boughtD.getString("package_token");
    const long long idD = db.findPackagePurchaseByToken(selfvoy::BookingManager::tokenHash(tokenD))->id;
    fake->set("pay-3", "succeeded");
    const Json openedD = integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(tokenD))).get("package");
    CHECK_EQ(openedD.getString("status"), std::string("active"));
    CHECK_EQ(openedD.get("payment").getString("status"), std::string("paid"));
    Json ownerCancel = Json::object();
    ownerCancel.set("booking_id",
                    Json(db.findBookingByToken(selfvoy::BookingManager::tokenHash(boughtD.getString("token")))->id));
    CHECK_EQ(fixture.call(owner, "booking.ownerCancel", ownerCancel).getString("type"), std::string("ok"));
    CHECK_EQ(db.findPackagePurchase(idD)->sessionsUsed, 0);
    const Json cancelledD = integration::payloadOf(fixture.call(owner, "package.cancel", packageIdPayload(idD))).get("package");
    CHECK_EQ(cancelledD.getString("status"), std::string("cancelled"));
    CHECK_EQ(cancelledD.get("payment").getString("status"), std::string("refunded"));
    const auto refunds = fake->of("POST", "/refunds");
    CHECK(refunds.size() == 1 && refunds.back().headers.at("Idempotence-Key") == "pr-" + std::to_string(idD) &&
          refunds.back().body.getString("payment_id") == "pay-3");
    CHECK_EQ(ownerNotices(db, ownerId, "package.payment", "refunded"), std::size_t(1));
    // Пакет с использованными занятиями — без автоматического возврата.
    CHECK_EQ(fixture.call(owner, "package.cancel", packageIdPayload(idA)).getString("type"), std::string("ok"));
    CHECK_EQ(fake->of("POST", "/refunds").size(), std::size_t(1));
}

// F5: оплата пакета по ссылке («Я оплатил(а)» → active), продажа вне сайта (package.issue),
// корректировка остатка, отметка оплаты владельцем, список и отмена пакета.
TEST(packages_claim_issue_adjust_cancel_payment_set) {
    using namespace packages_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "pk-iss-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "pkissowner@example.com", "Анна"));
    auto& db = fixture.server->database().db();
    Json offers = Json::array();
    offers.push(offer("2 занятия", 2, 500000, 30));
    Json pageReq = offersPage("Массаж", offers);
    pageReq.set("payment_url", Json("https://pay.example/anna"));
    pageReq.set("payment_note", Json("СБП +7 900 000-00-00"));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const long long pageId = page.getInt("id");
    auto guest = sockets.open(fixture, "pk-iss-guest");

    // Покупка с оплатой по ссылке: pending до «Я оплатил(а)».
    Json buy = bookPayload(page.getString("slug"), slotA, "Вера");
    buy.set("guest_email", Json("pk-vera@example.com"));
    buy.set("package_id", Json(1LL));
    const Json bought = integration::payloadOf(fixture.call(guest, "booking.book", buy));
    const std::string token = bought.getString("package_token");
    CHECK_EQ(bought.get("package").getString("status"), std::string("pending"));
    CHECK_EQ(bought.get("package").get("payment").getString("method"), std::string("link"));
    CHECK_EQ(bought.get("package").get("payment").getString("action_url"), std::string("https://pay.example/anna"));
    const auto mail = lastMail("pk-vera@example.com", "booking.created");
    CHECK(mail && footerHas(*mail, "Как оплатить: СБП +7 900 000-00-00") &&
          footerHas(*mail, "После оплаты нажмите «Я оплатил(а)» на странице абонемента."));
    const Json claimed = integration::payloadOf(fixture.call(guest, "package.paymentClaim", tokenPayload(token))).get("package");
    CHECK_EQ(claimed.getString("status"), std::string("active"));
    CHECK_EQ(claimed.get("payment").getString("status"), std::string("claimed"));
    CHECK(!claimed.getString("valid_until").empty());
    CHECK_EQ(fixture.call(guest, "package.paymentClaim", tokenPayload(token)).getString("type"), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "package.payment", "claimed"), std::size_t(1));
    CHECK_EQ(ownerNotices(db, ownerId, "package.low"), std::size_t(1));  // активирован с остатком 1
    CHECK_EQ(fixture.call(guest, "package.paymentClaim", tokenPayload(growth_test::newToken())).getString("code"),
             std::string("not_found"));
    // Неоплаченный по ссылке пакет claimed не истекает.
    CHECK_EQ(fixture.server->bookings().packageTick(100, std::time(nullptr) + 3 * 3600), std::size_t(0));

    // package.issue — проверка полей.
    const auto issue = [&](const Json& payload) { return fixture.call(owner, "package.issue", payload); };
    Json base = Json::object();
    base.set("page_id", Json(pageId));
    base.set("guest_name", Json("Олег"));
    base.set("guest_email", Json("pk-oleg@example.com"));
    base.set("cc_email", Json("pk-oleg-cc@example.com"));
    Json wrong = base;
    wrong.set("page_id", Json(999999LL));
    wrong.set("offer_id", Json(1LL));
    CHECK_EQ(issue(wrong).getString("code"), std::string("not_found"));
    wrong = base;
    wrong.set("offer_id", Json(9LL));
    CHECK_EQ(issue(wrong).getString("code"), std::string("bad_request"));
    wrong = base;
    wrong.set("title", Json("Своё"));
    wrong.set("sessions", Json(0LL));
    CHECK_EQ(issue(wrong).getString("code"), std::string("bad_request"));
    wrong = base;
    wrong.set("offer_id", Json(1LL));
    wrong.set("guest_name", Json(""));
    CHECK_EQ(issue(wrong).getString("code"), std::string("bad_request"));
    wrong = base;
    wrong.set("offer_id", Json(1LL));
    wrong.set("cc_email", Json("PK-OLEG@example.com"));
    CHECK_EQ(issue(wrong).getString("code"), std::string("bad_request"));
    wrong = base;
    wrong.set("offer_id", Json(1LL));
    wrong.set("paid", Json("да"));
    CHECK_EQ(issue(wrong).getString("code"), std::string("bad_request"));
    auto stranger = sockets.open(fixture, "pk-iss-stranger");
    fixture.registerUser(stranger, "pkissstranger@example.com", "Чужой");
    Json fromOffer = base;
    fromOffer.set("offer_id", Json(1LL));
    fromOffer.set("paid", Json(true));
    CHECK_EQ(fixture.call(stranger, "package.issue", fromOffer).getString("code"), std::string("not_found"));

    // Выдача по предложению, оплачено: пакет сразу active, письмо гостю и копия — со ссылкой.
    const Json issued = integration::payloadOf(issue(fromOffer));
    const Json issuedPack = issued.get("package");
    const std::string issuedToken = issued.getString("token");
    CHECK_EQ(issuedPack.getString("status"), std::string("active"));
    CHECK_EQ(issuedPack.getInt("offer_id"), 1LL);
    CHECK_EQ(issuedPack.getInt("sessions_left"), 2LL);
    CHECK_EQ(issuedPack.getInt("valid_days"), 30LL);
    CHECK_EQ(issuedPack.get("payment").getString("status"), std::string("paid"));
    CHECK_EQ(issuedPack.getString("cc_email"), std::string("pk-oleg-cc@example.com"));
    CHECK_EQ(issued.getString("url"), "https://selfvoy.example.com/#/p/" + issuedToken);
    const auto issuedMail = lastMail("pk-oleg@example.com", "package.issued");
    CHECK(issuedMail && issuedMail->linkUrl == issued.getString("url") &&
          issuedMail->subject == "Selfvoy: ваш абонемент «2 занятия»");
    CHECK_EQ(mailCount("pk-oleg-cc@example.com", "package.issued"), std::size_t(1));
    // Своё название и число занятий: без цены — без оплаты; с ценой и без paid — ждёт оплату.
    Json custom = base;
    custom.erase("cc_email");
    custom.set("title", Json("Индивидуально"));
    custom.set("sessions", Json(4LL));
    const Json free = integration::payloadOf(issue(custom)).get("package");
    CHECK(free.get("payment").isNull() && free.getInt("sessions_total") == 4 && free.getInt("offer_id") == 0 &&
          free.getInt("valid_days") == 90);
    custom.set("price", Json(300000LL));
    const Json unpaid = integration::payloadOf(issue(custom)).get("package");
    CHECK_EQ(unpaid.get("payment").getString("status"), std::string("pending"));
    CHECK_EQ(unpaid.getString("status"), std::string("active"));

    // package.adjust — корректировка остатка.
    const long long freeId = free.getInt("id");
    const auto adjust = [&](long long id, long long delta) {
        Json payload = packageIdPayload(id);
        payload.set("delta", Json(delta));
        return fixture.call(owner, "package.adjust", payload);
    };
    CHECK_EQ(adjust(freeId, 0).getString("code"), std::string("bad_request"));
    CHECK_EQ(adjust(freeId, 101).getString("code"), std::string("bad_request"));
    CHECK_EQ(adjust(freeId, -4).getString("code"), std::string("bad_request"));  // меньше 1 занятия
    CHECK_EQ(integration::payloadOf(adjust(freeId, 3)).get("package").getInt("sessions_left"), 7LL);
    CHECK_EQ(adjust(freeId, -7).getString("code"), std::string("bad_request"));
    CHECK_EQ(integration::payloadOf(adjust(freeId, -6)).get("package").getInt("sessions_left"), 1LL);
    CHECK_EQ(db.findPackagePurchase(freeId)->sessionsTotal, 1);
    Json strangerAdjust = packageIdPayload(freeId);
    strangerAdjust.set("delta", Json(1LL));
    CHECK_EQ(fixture.call(stranger, "package.adjust", strangerAdjust).getString("code"), std::string("not_found"));
    // Купленный пакет (1 занятие использовано): убрать больше остатка нельзя; −1 → used.
    const long long boughtId = db.findPackagePurchaseByToken(selfvoy::BookingManager::tokenHash(token))->id;
    CHECK_EQ(adjust(boughtId, -2).getString("code"), std::string("bad_request"));
    CHECK_EQ(integration::payloadOf(adjust(boughtId, -1)).get("package").getString("status"), std::string("used"));
    CHECK_EQ(integration::payloadOf(adjust(boughtId, 2)).get("package").getString("status"), std::string("active"));

    // package.paymentSet: paid — гостю «оплата получена»; неверный статус; пакет без оплаты.
    const long long unpaidId = unpaid.getInt("id");
    Json set = packageIdPayload(unpaidId);
    set.set("status", Json("paid"));
    const Json paid = integration::payloadOf(fixture.call(owner, "package.paymentSet", set)).get("package");
    CHECK(paid.get("payment").getString("status") == "paid" && !paid.get("payment").getString("paid_at").empty());
    CHECK_EQ(mailCount("pk-oleg@example.com", "package.paid"), std::size_t(1));
    CHECK_EQ(ownerNotices(db, ownerId, "package.payment", "paid"), std::size_t(0));  // сам отметил
    set.set("status", Json("pending"));
    CHECK_EQ(fixture.call(owner, "package.paymentSet", set).getString("code"), std::string("bad_request"));
    Json freeSet = packageIdPayload(freeId);
    freeSet.set("status", Json("paid"));
    CHECK_EQ(fixture.call(owner, "package.paymentSet", freeSet).getString("code"), std::string("bad_request"));
    set.set("status", Json("refunded"));
    CHECK(!integration::payloadOf(fixture.call(owner, "package.paymentSet", set)).get("package").get("payment")
               .getString("refunded_at").empty());

    // package.list: все, по статусу, по странице; неверный статус.
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "package.list")).get("packages").size(), std::size_t(4));
    Json filter = Json::object();
    filter.set("status", Json("active"));
    filter.set("page_id", Json(pageId));
    const Json active = integration::payloadOf(fixture.call(owner, "package.list", filter)).get("packages");
    CHECK_EQ(active.size(), std::size_t(4));
    CHECK_EQ(active.at(0).getString("page_title"), std::string("Массаж"));
    filter.set("status", Json("lost"));
    CHECK_EQ(fixture.call(owner, "package.list", filter).getString("code"), std::string("bad_request"));
    CHECK_EQ(integration::payloadOf(fixture.call(stranger, "package.list")).get("packages").size(), std::size_t(0));

    // package.cancel: по пакету больше не записаться, корректировать нельзя, повтор — conflict.
    const long long issuedId = issuedPack.getInt("id");
    CHECK_EQ(fixture.call(stranger, "package.cancel", packageIdPayload(issuedId)).getString("code"),
             std::string("not_found"));
    const Json cancelled = integration::payloadOf(fixture.call(owner, "package.cancel", packageIdPayload(issuedId)));
    CHECK_EQ(cancelled.get("package").getString("status"), std::string("cancelled"));
    CHECK_EQ(mailCount("pk-oleg@example.com", "package.cancelled"), std::size_t(1));
    CHECK_EQ(fixture.call(owner, "package.cancel", packageIdPayload(issuedId)).getString("code"), std::string("conflict"));
    CHECK_EQ(adjust(issuedId, 1).getString("code"), std::string("conflict"));
    Json byCancelled = slugPayload(page.getString("slug"));
    byCancelled.set("start", Json(selfvoy::isoUtc(slotB)));
    byCancelled.set("package_token", Json(issuedToken));
    CHECK_EQ(fixture.call(guest, "booking.book", byCancelled).getString("message"), std::string("абонемент отменён"));
    const Json guestView = integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(issuedToken))).get("package");
    CHECK_EQ(guestView.getString("status"), std::string("cancelled"));
}

// F5: второй получатель (cc_email) — проверка, письма о записи, напоминания и отмене,
// свой лимит писем на адрес, маска в виде гостя.
TEST(cc_email_notices_limit_and_mask) {
    using namespace packages_test;
    std::vector<std::time_t> slots;
    for (int k = 0; k < 10; ++k) slots.push_back(invite_test::future(30 + 12 * k));
    invite_test::FakeAi ai([&] { return slotsBody(slots); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "cc-owner");
    fixture.registerUser(owner, "ccowner@example.com", "Ольга");
    auto& db = fixture.server->database().db();
    const std::string slug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", payPage("Репетитор"))).get("page").getString("slug");
    const auto book = [&](int index, const std::string& guestEmail, const std::string& cc) {
        auto guest = openFrom(fixture, sockets, "cc-guest-" + std::to_string(index),
                              "127.0.3." + std::to_string(index + 1) + ":5000");
        Json payload = bookPayload(slug, slots[static_cast<std::size_t>(index)], "Ученик");
        payload.set("guest_email", Json(guestEmail));
        if (!cc.empty()) payload.set("cc_email", Json(cc));
        return fixture.call(guest, "booking.book", payload);
    };
    CHECK_EQ(book(0, "cc-g0@example.com", "не адрес").getString("code"), std::string("bad_request"));
    CHECK_EQ(book(0, "cc-g0@example.com", "CC-G0@example.com").getString("code"), std::string("bad_request"));
    CHECK_EQ(book(0, "cc-g0@example.com", "x@" + std::string(260, 'a') + ".ru").getString("code"),
             std::string("bad_request"));

    const std::string cc = "Parent.CC@Example.com";
    const Json first = book(0, "cc-g0@example.com", cc);
    CHECK_EQ(first.getString("type"), std::string("ok"));
    const std::string token = integration::payloadOf(first).getString("token");
    CHECK_EQ(integration::payloadOf(first).get("booking").getString("cc_email"), std::string("P***@Example.com"));
    auto guest = sockets.open(fixture, "cc-manage");
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token))).get("booking");
    CHECK_EQ(managed.getString("cc_email"), std::string("P***@Example.com"));
    CHECK(!managed.getBool("by_package", true));
    const auto copy = lastMail(cc, "booking.created");
    CHECK(copy && !copy->linkUrl.empty() && copy->linkUrl.find("/#/m/" + token) != std::string::npos);
    CHECK_EQ(db.findBookingByToken(selfvoy::BookingManager::tokenHash(token))->ccEmail, cc);

    // Напоминание за сутки — и второму получателю.
    CHECK_EQ(fixture.server->bookings().reminderTick(100, std::time(nullptr) + 7 * 3600), std::size_t(1));
    CHECK_EQ(mailCount(cc, "booking.reminder"), std::size_t(1));
    CHECK_EQ(mailCount("cc-g0@example.com", "booking.reminder"), std::size_t(1));
    // Отмена — копия тоже.
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(token)).getString("type"), std::string("ok"));
    CHECK_EQ(mailCount(cc, "booking.cancelled"), std::size_t(1));

    // Свой лимит: 10 писем в час на адрес (ключ — в нижнем регистре). У копии уже 3 письма;
    // ещё 7 записей — 10, дальше копия не уходит, а гость своё письмо получает.
    for (int i = 1; i <= 7; ++i) {
        CHECK_EQ(book(i, "cc-g" + std::to_string(i) + "@example.com", "parent.cc@example.com").getString("type"),
                 std::string("ok"));
    }
    CHECK_EQ(mailCount(cc, "booking.created"), std::size_t(8));
    CHECK_EQ(book(8, "cc-g8@example.com", "parent.cc@example.com").getString("type"), std::string("ok"));
    CHECK_EQ(mailCount(cc, "booking.created"), std::size_t(8));
    CHECK_EQ(mailCount("cc-g8@example.com", "booking.created"), std::size_t(1));

    // Маска: первый символ локальной части (UTF-8 — целиком).
    CHECK_EQ(selfvoy::BookingManager::maskEmail("mama@example.com"), std::string("m***@example.com"));
    CHECK_EQ(selfvoy::BookingManager::maskEmail("ю@почта.рф"), std::string("ю***@почта.рф"));
    CHECK_EQ(selfvoy::BookingManager::maskEmail(""), std::string(""));
}

// PostgreSQL (S2b): предложения страницы, покупки пакетов, cc_email и package_id записи.
// Снимок цены записи: price/currency — цена страницы на момент booking.book; смена цены
// страницы, перенос и отмена его не меняют; payment.amount (депозит) — отдельно.
TEST(booking_price_snapshot_survives_page_price_change) {
    using namespace packages_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB, slotC}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "price-owner");
    fixture.registerUser(owner, "priceowner@example.com", "Вера");
    auto& db = fixture.server->database().db();
    Json pageReq = payPage("Консультация");
    pageReq.set("price", Json(300000LL));
    pageReq.set("currency", Json("EUR"));
    pageReq.set("prepay", Json("deposit"));
    pageReq.set("deposit", Json(100000LL));
    pageReq.set("payment_url", Json("https://pay.example/vera"));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const std::string slug = page.getString("slug");
    auto guest = sockets.open(fixture, "price-guest");

    const Json bookedA = integration::payloadOf(fixture.call(guest, "booking.book", bookPayload(slug, slotA, "Анна")));
    const std::string tokenA = bookedA.getString("token");
    CHECK_EQ(bookedA.get("booking").getInt("price"), 300000LL);
    CHECK_EQ(bookedA.get("booking").getString("currency"), std::string("EUR"));
    CHECK_EQ(bookedA.get("booking").get("payment").getInt("amount"), 100000LL);  // депозит — не цена
    const auto storedA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenA));
    CHECK(storedA && storedA->price == 300000 && storedA->currency == "EUR");

    // Владелец поднял цену: старая запись хранит свою, новая — по новой цене.
    Json raise = Json::object();
    raise.set("page_id", Json(page.getInt("id")));
    raise.set("price", Json(500000LL));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", raise).getString("type"), std::string("ok"));
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(tokenA))).get("booking");
    CHECK_EQ(managed.getInt("price"), 300000LL);
    CHECK_EQ(managed.getString("currency"), std::string("EUR"));
    const Json bookedB = integration::payloadOf(fixture.call(guest, "booking.book", bookPayload(slug, slotB, "Борис")));
    CHECK_EQ(bookedB.get("booking").getInt("price"), 500000LL);
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    CHECK_EQ(listed.size(), std::size_t(2));
    for (const auto& item : listed.items()) {
        CHECK_EQ(item.getInt("price"), item.getString("guest_name") == "Анна" ? 300000LL : 500000LL);
        CHECK_EQ(item.getString("currency"), std::string("EUR"));
    }

    // Перенос и отмена цену не меняют.
    Json move = tokenPayload(tokenA);
    move.set("start", Json(selfvoy::isoUtc(slotC)));
    const Json moved = fixture.call(guest, "booking.reschedule", move);
    CHECK_EQ(moved.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(moved).get("booking").getInt("price"), 300000LL);
    CHECK_EQ(fixture.call(guest, "booking.cancel", tokenPayload(tokenA)).getString("type"), std::string("ok"));
    const auto cancelledA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(tokenA));
    CHECK(cancelledA && cancelledA->status == "cancelled" && cancelledA->price == 300000 &&
          cancelledA->currency == "EUR");
}

// Запись по пакету (покупка при записи и по package_token) — price 0; package.get отдаёт
// ref — реферальный код владельца (как booking.get / invite.get).
TEST(booking_price_zero_by_package_and_package_ref) {
    using namespace packages_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    invite_test::FakeAi ai([&] { return slotsBody({slotA, slotB}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "price-pk-owner");
    fixture.registerUser(owner, "pricepk@example.com", "Ольга");
    auto& db = fixture.server->database().db();
    Json offers = Json::array();
    offers.push(offer("3 занятия", 3, 900000));
    Json pageReq = offersPage("Тренировки", offers);
    pageReq.set("price", Json(350000LL));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const std::string slug = page.getString("slug");
    auto guest = sockets.open(fixture, "price-pk-guest");

    Json buy = bookPayload(slug, slotA, "Мария");
    buy.set("package_id", Json(1LL));
    const Json bought = integration::payloadOf(fixture.call(guest, "booking.book", buy));
    CHECK_EQ(bought.get("booking").getInt("price"), 0LL);
    CHECK(bought.get("booking").getBool("by_package"));
    const std::string packageToken = bought.getString("package_token");
    const auto storedA = db.findBookingByToken(selfvoy::BookingManager::tokenHash(bought.getString("token")));
    CHECK(storedA && storedA->price == 0 && storedA->packageId > 0);

    Json byToken = slugPayload(slug);
    byToken.set("start", Json(selfvoy::isoUtc(slotB)));
    byToken.set("package_token", Json(packageToken));
    const Json bookedB = integration::payloadOf(fixture.call(guest, "booking.book", byToken));
    CHECK_EQ(bookedB.get("booking").getInt("price"), 0LL);
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
    CHECK_EQ(listed.size(), std::size_t(2));
    for (const auto& item : listed.items()) {
        CHECK_EQ(item.getInt("price"), 0LL);
        CHECK(item.getInt("package_id") > 0);
    }

    const Json pageView = integration::payloadOf(fixture.call(guest, "booking.get", slugPayload(slug))).get("page");
    const Json pack = integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(packageToken))).get("package");
    CHECK_EQ(pack.getString("ref").size(), std::size_t(8));
    CHECK_EQ(pack.getString("ref"), pageView.getString("ref"));
}

TEST(pg_packages_methods) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto db = selfvoy::makePostgresDatabase(url);
    std::string error;
    CHECK(db && db->connect(error));
    if (!db) return;
    long long owner = 0;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    CHECK(db->createUser("pg-pkg-" + suffix + "@example.com", "argon2id$x", "Пакеты", owner).ok);
    selfvoy::BookingPageRecord page;
    page.ownerId = owner;
    page.slug = "pkg" + suffix + "00";
    page.slug.resize(10, '0');
    page.title = "Пакеты";
    Json offers = Json::array();
    Json offer = Json::object();
    offer.set("id", Json(1LL));
    offer.set("title", Json("5 занятий"));
    offer.set("sessions", Json(5LL));
    offer.set("price", Json(1000000LL));
    offer.set("valid_days", Json(90LL));
    offers.push(offer);
    page.packages = offers;
    long long pageId = 0;
    CHECK(db->createBookingPage(page, pageId).ok);
    auto stored = db->findBookingPage(pageId);
    CHECK(stored && stored->packages.size() == 1 && stored->packages.at(0).getString("title") == "5 занятий");
    if (stored) {
        stored->packages = Json::array();
        CHECK(db->updateBookingPage(*stored).ok);
        CHECK_EQ(db->findBookingPage(pageId)->packages.size(), std::size_t(0));
    }

    selfvoy::PackagePurchaseRecord purchase;
    purchase.ownerId = owner;
    purchase.pageId = pageId;
    purchase.offerId = 1;
    purchase.title = "5 занятий";
    purchase.sessionsTotal = 5;
    purchase.sessionsUsed = 1;
    purchase.validDays = 30;
    purchase.price = 1000000;
    purchase.currency = "EUR";
    purchase.status = "pending";
    Json payment = Json::object();
    payment.set("status", Json("pending"));
    payment.set("due_at", Json("2000-01-01T00:00:00Z"));
    purchase.payment = payment;
    purchase.guestName = "Гость";
    purchase.guestEmail = "g@example.com";
    purchase.ccEmail = "cc@example.com";
    purchase.tokenHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    purchase.tokenEnc = "sealed";
    long long packageId = 0;
    CHECK(db->createPackagePurchase(purchase, packageId).ok);
    CHECK(!db->createPackagePurchase(purchase, packageId).ok || packageId == 0);  // token_hash UNIQUE
    packageId = 0;
    for (const auto& item : db->listPackagePurchases(owner, 0, "", 10)) packageId = item.id;
    auto found = db->findPackagePurchase(packageId);
    CHECK(found && found->title == "5 занятий" && found->sessionsTotal == 5 && found->sessionsUsed == 1 &&
          found->validDays == 30 && found->currency == "EUR" && found->status == "pending" &&
          found->payment.getString("status") == "pending" && found->ccEmail == "cc@example.com" &&
          found->tokenEnc == "sealed" && found->validUntil.empty() && !found->createdAt.empty());
    CHECK(db->findPackagePurchaseByToken(purchase.tokenHash).has_value());
    bool pending = false;
    for (const auto& item : db->listPendingPackagePurchases(1000)) pending = pending || item.id == packageId;
    CHECK(pending);
    if (found) {
        found->status = "active";
        found->validUntil = "2001-01-01T00:00:00Z";
        found->sessionsTotal = 6;
        found->sessionsUsed = 2;
        found->payment = Json();
        CHECK(db->updatePackagePurchase(*found).ok);
        const auto updated = db->findPackagePurchase(packageId);
        CHECK(updated && updated->status == "active" && updated->validUntil == "2001-01-01T00:00:00Z" &&
              updated->sessionsTotal == 6 && updated->sessionsUsed == 2 && updated->payment.isNull());
        bool due = false;
        for (const auto& item : db->listExpiredPackagePurchases("2002-01-01T00:00:00Z", 1000)) due = due || item.id == packageId;
        CHECK(due);
        for (const auto& item : db->listExpiredPackagePurchases("2000-06-01T00:00:00Z", 1000)) CHECK(item.id != packageId);
        found->status = "lost";  // CHECK в схеме
        CHECK(!db->updatePackagePurchase(*found).ok);
    }
    CHECK_EQ(db->listPackagePurchases(owner, pageId, "active", 10).size(), std::size_t(1));
    CHECK_EQ(db->listPackagePurchases(owner, pageId, "used", 10).size(), std::size_t(0));

    selfvoy::BookingRecord booking;
    booking.pageId = pageId;
    booking.ownerId = owner;
    booking.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    booking.guestName = "Гость";
    booking.ccEmail = "copy@example.com";
    booking.packageId = packageId;
    long long bookingId = 0;
    CHECK(db->createBooking(booking, bookingId).ok);
    const auto withPackage = db->findBooking(bookingId);
    CHECK(withPackage && withPackage->ccEmail == "copy@example.com" && withPackage->packageId == packageId);
    CHECK_EQ(db->listBookingsByPackage(packageId, 10).size(), std::size_t(1));
    selfvoy::BookingRecord plainBooking = booking;
    plainBooking.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    plainBooking.ccEmail.clear();
    plainBooking.packageId = 0;
    long long plainId = 0;
    CHECK(db->createBooking(plainBooking, plainId).ok);
    CHECK(db->findBooking(plainId) && db->findBooking(plainId)->packageId == 0);
    // Снимок цены записи: price/currency сохраняются и не меняются updateBooking.
    selfvoy::BookingRecord priced = plainBooking;
    priced.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    priced.price = 250000;
    priced.currency = "EUR";
    long long pricedId = 0;
    CHECK(db->createBooking(priced, pricedId).ok);
    auto pricedStored = db->findBooking(pricedId);
    CHECK(pricedStored && pricedStored->price == 250000 && pricedStored->currency == "EUR");
    if (pricedStored) {
        pricedStored->status = "cancelled";
        CHECK(db->updateBooking(*pricedStored).ok);
        pricedStored = db->findBooking(pricedId);
        CHECK(pricedStored && pricedStored->price == 250000 && pricedStored->currency == "EUR" &&
              pricedStored->status == "cancelled");
    }
    CHECK(withPackage && withPackage->price == 0 && withPackage->currency == "RUB");
}

// ------------------------------- F8: лист ожидания и «пора на повторный приём»; F10: «Мой налог»
namespace waitlist_test {

using namespace packages_test;
using booking_test::isBusy;

// Слоты AI с местным временем владельца (пояс UTC: local = время UTC).
std::string localSlots(const std::vector<std::time_t>& starts) {
    Json slots = Json::array();
    for (const std::time_t at : starts) {
        Json slot = daySlot(at, 60, selfvoy::isoUtc(at).substr(0, 10));
        slot.set("local", Json(selfvoy::isoUtc(at).substr(0, 16)));
        slots.push(slot);
    }
    Json body = Json::object();
    body.set("slots", slots);
    return body.dump();
}

Json waitPage(const std::string& title, long long hold = 60) {
    Json payload = payPage(title);
    payload.set("waitlist", Json(true));
    payload.set("waitlist_hold_minutes", Json(hold));
    return payload;
}

Json joinPayload(const std::string& slug, const std::string& name, const std::string& email) {
    Json payload = slugPayload(slug);
    payload.set("guest_name", Json(name));
    payload.set("guest_email", Json(email));
    return payload;
}

// Токен из ссылки последнего письма вида kind на адрес (…/#/w/<токен>, …/#/m/<токен>).
std::string tokenFromMail(const std::string& to, const std::string& kind) {
    const auto mail = lastMail(to, kind);
    if (!mail) return "";
    const std::size_t at = mail->linkUrl.rfind('/');
    return at == std::string::npos ? std::string() : mail->linkUrl.substr(at + 1);
}

std::string codeOf(const Json& response) {
    return response.getString("type") == "ok" ? std::string("ok") : response.getString("code");
}

std::optional<selfvoy::WaitlistEntryRecord> entryOf(selfvoy::IDatabase& db, long long ownerId,
                                                    const std::string& email) {
    for (const auto& entry : db.listWaitlistEntries(ownerId, 0, "", 200)) {
        if (entry.guestEmail == email) return entry;  // новые сверху
    }
    return std::nullopt;
}

long long bookingIdOf(selfvoy::IDatabase& db, const Json& booked) {
    const auto stored = db.findBookingByToken(selfvoy::BookingManager::tokenHash(integration::payloadOf(booked).getString("token")));
    return stored ? stored->id : 0;
}

Json pageId(long long id) {
    Json payload = Json::object();
    payload.set("page_id", Json(id));
    return payload;
}

std::size_t taskCount(selfvoy::IDatabase& db, long long ownerId, const std::string& title) {
    std::size_t count = 0;
    for (const auto& task : db.listTasks(ownerId, "", 500)) {
        if (task.title == title) ++count;
    }
    return count;
}

}  // namespace waitlist_test

// F8: поля страницы (Pro, диапазоны), вход в лист, повтор обновляет, лимит по IP, выход.
TEST(waitlist_join_validation_limits_and_pro) {
    using namespace waitlist_test;
    const std::time_t slotA = invite_test::future(30);
    const std::string dayA = selfvoy::isoUtc(slotA).substr(0, 10);
    invite_test::FakeAi ai([&] { return localSlots({slotA}); });
    Fixture fixture;
    enablePayments(fixture, ai.url(), true);
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "wl-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "wlowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();

    // Без Pro (SELFVOY_PLANS_ENFORCED=1) лист и повторный приём не включить; диапазоны.
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageCreate", waitPage("Лист"))), std::string("pro_required"));
    Json rebook = payPage("Повтор");
    rebook.set("rebook_after_days", Json(30LL));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageCreate", rebook)), std::string("pro_required"));
    Json badHold = waitPage("Лист", 10);
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageCreate", badHold)), std::string("bad_request"));
    for (const long long days : {3LL, 181LL, -1LL}) {
        Json badRebook = payPage("Повтор");
        badRebook.set("rebook_after_days", Json(days));
        CHECK_EQ(codeOf(fixture.call(owner, "booking.pageCreate", badRebook)), std::string("bad_request"));
    }
    Json notBool = payPage("Лист");
    notBool.set("waitlist", Json("да"));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageCreate", notBool)), std::string("bad_request"));
    const std::string plainSlug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", payPage("Без листа"))).get("page").getString("slug");

    CHECK(db.setUserPlanDays(ownerId, 30).ok);
    Json withAll = waitPage("Разбор", 45);
    withAll.set("rebook_after_days", Json(14LL));
    const Json created = integration::payloadOf(fixture.call(owner, "booking.pageCreate", withAll)).get("page");
    CHECK(created.getBool("waitlist") && created.getInt("waitlist_hold_minutes") == 45 &&
          created.getInt("rebook_after_days") == 14);
    const std::string slug = created.getString("slug");
    const long long page = created.getInt("id");
    const Json view = integration::payloadOf(fixture.call(owner, "booking.get", slugPayload(slug))).get("page");
    CHECK(view.getBool("waitlist") && view.getInt("waitlist_hold_minutes") == 45 && view.getInt("rebook_after_days") == 14);
    // Pro закончился: править включённое можно, включить заново — нет.
    CHECK(db.setUserPlanDays(ownerId, 0).ok);
    Json hold = pageId(page);
    hold.set("waitlist_hold_minutes", Json(60LL));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", hold)), std::string("ok"));
    Json off = pageId(page);
    off.set("waitlist", Json(false));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", off)), std::string("ok"));
    Json on = pageId(page);
    on.set("waitlist", Json(true));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", on)), std::string("pro_required"));
    CHECK(db.setUserPlanDays(ownerId, 30).ok);
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", on)), std::string("ok"));
    selfvoy::plan::setEnforced(false);

    auto guest = openFrom(fixture, sockets, "wl-guest", "203.0.113.10:5000");
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", joinPayload(plainSlug, "Анна", "wl-anna@example.com"))),
             std::string("forbidden"));
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", joinPayload("zzzzzzzzzz", "Анна", "wl-anna@example.com"))),
             std::string("not_found"));
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", joinPayload(slug, "Анна", ""))), std::string("bad_request"));
    Json badPart = joinPayload(slug, "Анна", "wl-anna@example.com");
    badPart.set("part_of_day", Json("night"));
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", badPart)), std::string("bad_request"));
    Json tooMany = joinPayload(slug, "Анна", "wl-anna@example.com");
    Json fifteen = Json::array();
    for (int i = 0; i < 15; ++i) fifteen.push(Json(selfvoy::isoUtc(slotA + i * 86400).substr(0, 10)));
    tooMany.set("days", fifteen);
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", tooMany)), std::string("bad_request"));
    Json farDay = joinPayload(slug, "Анна", "wl-anna@example.com");
    Json far = Json::array();
    far.push(Json("2099-01-01"));
    farDay.set("days", far);
    CHECK_EQ(codeOf(fixture.call(guest, "booking.waitlistJoin", farDay)), std::string("bad_request"));

    // Вход: дни без повторов, срок 30 дней, уведомление владельцу, письмо со ссылкой #/w/<токен>.
    auto other = openFrom(fixture, sockets, "wl-guest2", "203.0.113.11:5000");
    Json join = joinPayload(slug, "Анна", "wl-anna@example.com");
    Json days = Json::array();
    days.push(Json(dayA));
    days.push(Json(dayA));
    join.set("days", days);
    join.set("comment", Json("Лучше после обеда"));
    const std::time_t before = std::time(nullptr);
    const Json joined = fixture.call(other, "booking.waitlistJoin", join);
    CHECK_EQ(codeOf(joined), std::string("ok"));
    const Json entry = integration::payloadOf(joined).get("entry");
    CHECK_EQ(entry.getString("status"), std::string("waiting"));
    CHECK(entry.get("days").size() == 1 && entry.get("days").at(0).asString() == dayA);
    CHECK_EQ(entry.getString("part_of_day"), std::string("any"));
    CHECK_EQ(integration::payloadOf(joined).getString("telegram_url"), std::string());  // бот не настроен
    std::time_t expires = 0;
    CHECK(selfvoy::parseIsoUtc(entry.getString("expires_at"), expires) && expires >= before + 30 * 86400 &&
          expires <= std::time(nullptr) + 30 * 86400);
    CHECK_EQ(ownerNotices(db, ownerId, "waitlist.joined"), std::size_t(1));
    CHECK_EQ(lastNotice(db, ownerId, "waitlist.joined").getInt("entry_id"), entry.getInt("id"));
    const auto joinMail = lastMail("wl-anna@example.com", "booking.waitlist_joined");
    CHECK(joinMail && joinMail->linkUrl.rfind("https://selfvoy.example.com/#/w/", 0) == 0 &&
          joinMail->subject == "Selfvoy: вы в листе ожидания — Разбор");
    const std::string token = tokenFromMail("wl-anna@example.com", "booking.waitlist_joined");
    const auto stored = db.findWaitlistEntryByToken(selfvoy::BookingManager::tokenHash(token));
    CHECK(stored && stored->id == entry.getInt("id") && stored->tokenEnc.find(token) == std::string::npos &&
          bookings.waitlistTokenOf(*stored) == token && stored->comment == "Лучше после обеда");
    const Json got = integration::payloadOf(fixture.call(other, "booking.waitlistGet", tokenPayload(token))).get("offer");
    CHECK(got.getString("status") == "waiting" && got.getString("start").empty() && got.getString("page_title") == "Разбор" &&
          got.getString("owner_name") == "Ольга" && got.getString("guest_name") == "Анна");
    CHECK_EQ(codeOf(fixture.call(other, "booking.waitlistGet", tokenPayload(growth_test::newToken()))), std::string("not_found"));

    // Повтор с тем же email (регистр не важен) — та же запись, пожелания обновлены, без второго письма.
    Json again = joinPayload(slug, "Анна К.", "WL-Anna@Example.com");
    again.set("part_of_day", Json("morning"));
    const Json rejoined = integration::payloadOf(fixture.call(other, "booking.waitlistJoin", again)).get("entry");
    CHECK_EQ(rejoined.getInt("id"), entry.getInt("id"));
    CHECK(rejoined.getString("part_of_day") == "morning" && rejoined.get("days").size() == 0);
    CHECK_EQ(ownerNotices(db, ownerId, "waitlist.joined"), std::size_t(1));
    CHECK_EQ(mailCount("wl-anna@example.com", "booking.waitlist_joined"), std::size_t(1));

    // Вид владельца: email замаскирован; фильтры.
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.waitlist", pageId(page))).get("entries");
    CHECK(listed.size() == 1 && listed.at(0).getString("guest_email") == "w***@example.com" &&
          listed.at(0).getString("guest_name") == "Анна К." && listed.at(0).getString("status") == "waiting" &&
          !listed.at(0).contains("offer") && listed.at(0).getString("page_title") == "Разбор");
    Json offeredOnly = Json::object();
    offeredOnly.set("status", Json("offered"));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "booking.waitlist", offeredOnly)).get("entries").size(),
             std::size_t(0));
    Json bogus = Json::object();
    bogus.set("status", Json("bogus"));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.waitlist", bogus)), std::string("bad_request"));

    // Выход по ссылке; повтор — без изменений; гость видит cancelled.
    const Json left = fixture.call(other, "booking.waitlistLeave", tokenPayload(token));
    CHECK_EQ(integration::payloadOf(left).get("entry").getString("status"), std::string("left"));
    CHECK_EQ(codeOf(fixture.call(other, "booking.waitlistLeave", tokenPayload(token))), std::string("ok"));
    CHECK_EQ(integration::payloadOf(fixture.call(other, "booking.waitlistGet", tokenPayload(token))).get("offer").getString("status"),
             std::string("cancelled"));
    // Снова в лист — новая запись; владелец убирает её; чужой владелец — not_found.
    const long long second =
        integration::payloadOf(fixture.call(other, "booking.waitlistJoin", join)).get("entry").getInt("id");
    CHECK(second > 0 && second != entry.getInt("id"));
    auto stranger = sockets.open(fixture, "wl-stranger");
    fixture.registerUser(stranger, "wlstranger@example.com", "Чужой");
    Json remove = Json::object();
    remove.set("entry_id", Json(second));
    CHECK_EQ(codeOf(fixture.call(stranger, "booking.waitlistRemove", remove)), std::string("not_found"));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "booking.waitlistRemove", remove)).get("entry").getString("status"),
             std::string("left"));

    // Лимит — как у booking.book: 10 в минуту с IP.
    auto flood = openFrom(fixture, sockets, "wl-flood", "203.0.113.12:5000");
    int refused = 0;
    for (int i = 0; i < 11; ++i) {
        const Json answer = fixture.call(flood, "booking.waitlistJoin",
                                         joinPayload(slug, "Гость", "wl-flood" + std::to_string(i) + "@example.com"));
        if (answer.getString("code") == "forbidden") ++refused;
    }
    CHECK_EQ(refused, 1);
}

// F8: отмена → время держится за старейшим подходящим; держание — занятость для других
// гостей; не забрал → следующему; забрал → обычная запись; затем — освобождение.
TEST(waitlist_offer_hold_claim_and_expiry) {
    using namespace waitlist_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::string dayA = selfvoy::isoUtc(slotA).substr(0, 10);
    const std::string dayB = selfvoy::isoUtc(slotB).substr(0, 10);
    invite_test::FakeAi ai([&] { return localSlots({slotA, slotB}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "wo-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "woowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", waitPage("Консультация"))).get("page");
    const std::string slug = page.getString("slug");

    auto first = openFrom(fixture, sockets, "wo-g1", "198.51.100.1:1");
    Json bookA = bookPayload(slug, slotA, "Гость");
    bookA.set("guest_email", Json("wo-g1@example.com"));
    const Json bookedA = fixture.call(first, "booking.book", bookA);
    CHECK_EQ(codeOf(bookedA), std::string("ok"));
    auto waiter = openFrom(fixture, sockets, "wo-w", "198.51.100.2:1");
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistJoin", joinPayload(slug, "Первая", "wo-w1@example.com"))),
             std::string("ok"));
    Json second = joinPayload(slug, "Вторая", "wo-w2@example.com");
    Json onlyA = Json::array();
    onlyA.push(Json(dayA));
    second.set("days", onlyA);
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistJoin", second)), std::string("ok"));
    Json third = joinPayload(slug, "Третья", "wo-w3@example.com");
    Json onlyB = Json::array();
    onlyB.push(Json(dayB));
    third.set("days", onlyB);
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistJoin", third)), std::string("ok"));
    CHECK(!isBusy(fixture, ownerId, slotB));

    // Гость отменил — время A держится за первой (самой старой подходящей).
    CHECK_EQ(codeOf(fixture.call(first, "booking.cancel", tokenPayload(integration::payloadOf(bookedA).getString("token")))),
             std::string("ok"));
    auto w1 = entryOf(db, ownerId, "wo-w1@example.com");
    auto w2 = entryOf(db, ownerId, "wo-w2@example.com");
    CHECK(w1 && w1->status == "offered" && w1->offerStart == selfvoy::isoUtc(slotA) && w1->offerStatus == "offered");
    CHECK(w2 && w2->status == "waiting");
    std::time_t holdUntil = 0;
    CHECK(w1 && selfvoy::parseIsoUtc(w1->offerExpiresAt, holdUntil) && holdUntil > std::time(nullptr) + 3500 &&
          holdUntil <= std::time(nullptr) + 3600);
    // Держание — занятость: время не видно и не записывается другим гостем.
    CHECK(isBusy(fixture, ownerId, slotA));
    const Json view = integration::payloadOf(fixture.call(first, "booking.get", slugPayload(slug))).get("page");
    CHECK(!booking_test::hasSlot(view, slotA) && booking_test::hasSlot(view, slotB));
    auto rival = openFrom(fixture, sockets, "wo-rival", "198.51.100.3:1");
    CHECK_EQ(codeOf(fixture.call(rival, "booking.book", bookPayload(slug, slotA, "Соперник"))), std::string("conflict"));
    const Json listed = integration::payloadOf(fixture.call(owner, "booking.waitlist")).get("entries");
    bool shown = false;
    for (const auto& item : listed.items()) {
        if (item.getString("status") == "offered") shown = item.get("offer").getString("start") == selfvoy::isoUtc(slotA);
    }
    CHECK(shown);
    const auto offerMail = lastMail("wo-w1@example.com", "booking.waitlist_offer");
    CHECK(offerMail && offerMail->subject == "Selfvoy: освободилось время — Консультация" &&
          offerMail->lead.find("Время держится за вами до ") != std::string::npos &&
          offerMail->linkUrl.rfind("https://selfvoy.example.com/#/w/", 0) == 0);
    CHECK_EQ(mailCount("wo-w2@example.com", "booking.waitlist_offer"), std::size_t(0));
    const std::string token1 = tokenFromMail("wo-w1@example.com", "booking.waitlist_offer");
    const Json offered = integration::payloadOf(fixture.call(waiter, "booking.waitlistGet", tokenPayload(token1))).get("offer");
    CHECK(offered.getString("status") == "offered" && offered.getString("start") == selfvoy::isoUtc(slotA) &&
          offered.getString("expires_at") == w1->offerExpiresAt);

    // Не забрала за час — время уходит второй (её день), первая — снова в листе.
    const std::time_t now = std::time(nullptr);
    CHECK_EQ(bookings.waitlistTick(100, now + 30 * 60), std::size_t(0));
    CHECK_EQ(bookings.waitlistTick(100, now + 61 * 60), std::size_t(1));
    w1 = entryOf(db, ownerId, "wo-w1@example.com");
    w2 = entryOf(db, ownerId, "wo-w2@example.com");
    CHECK(w1 && w1->status == "waiting" && w1->offerStatus == "expired");
    CHECK(w2 && w2->status == "offered" && w2->offerStart == selfvoy::isoUtc(slotA));
    CHECK_EQ(integration::payloadOf(fixture.call(waiter, "booking.waitlistGet", tokenPayload(token1))).get("offer").getString("status"),
             std::string("expired"));
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistClaim", tokenPayload(token1))), std::string("conflict"));
    CHECK(isBusy(fixture, ownerId, slotA));

    // Вторая забирает — обычная запись (ссылка управления), запись листа — booked.
    const std::string token2 = tokenFromMail("wo-w2@example.com", "booking.waitlist_offer");
    const std::size_t createdBefore = ownerNotices(db, ownerId, "booking.created");
    const Json claimed = fixture.call(waiter, "booking.waitlistClaim", tokenPayload(token2));
    CHECK_EQ(codeOf(claimed), std::string("ok"));
    const Json claimedPayload = integration::payloadOf(claimed);
    CHECK_EQ(claimedPayload.get("booking").getString("guest_name"), std::string("Вторая"));
    CHECK(claimedPayload.getString("manage_url").rfind("https://selfvoy.example.com/#/m/", 0) == 0 &&
          claimedPayload.getString("manage_url") == claimedPayload.getString("url") &&
          claimedPayload.getString("token").size() == 43 && claimedPayload.get("payment").isNull());
    const auto claimedBooking =
        db.findBookingByToken(selfvoy::BookingManager::tokenHash(claimedPayload.getString("token")));
    CHECK(claimedBooking && claimedBooking->guestEmail == "wo-w2@example.com" &&
          claimedBooking->occurrences.at(0).getString("start") == selfvoy::isoUtc(slotA));
    w2 = entryOf(db, ownerId, "wo-w2@example.com");
    CHECK(w2 && w2->status == "booked" && w2->offerStatus == "claimed" && claimedBooking &&
          w2->bookingId == claimedBooking->id);
    CHECK_EQ(ownerNotices(db, ownerId, "waitlist.booked"), std::size_t(1));
    CHECK_EQ(lastNotice(db, ownerId, "waitlist.booked").getInt("entry_id"), w2 ? w2->id : -1);
    CHECK_EQ(ownerNotices(db, ownerId, "booking.created"), createdBefore);  // одно уведомление, не два
    CHECK_EQ(mailCount("wo-w2@example.com", "booking.created"), std::size_t(1));
    CHECK_EQ(integration::payloadOf(fixture.call(waiter, "booking.waitlistGet", tokenPayload(token2))).get("offer").getString("status"),
             std::string("claimed"));
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistClaim", tokenPayload(token2))), std::string("conflict"));

    // Вторая отменила — время снова первой; не забрала — подходящих больше нет, время свободно.
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.cancel", tokenPayload(claimedPayload.getString("token")))),
             std::string("ok"));
    w1 = entryOf(db, ownerId, "wo-w1@example.com");
    CHECK(w1 && w1->status == "offered" && w1->offerStart == selfvoy::isoUtc(slotA));
    CHECK_EQ(bookings.waitlistTick(100, std::time(nullptr) + 61 * 60), std::size_t(1));
    w1 = entryOf(db, ownerId, "wo-w1@example.com");
    const auto w3 = entryOf(db, ownerId, "wo-w3@example.com");
    CHECK(w1 && w1->status == "waiting" && w3 && w3->status == "waiting");
    CHECK(!isBusy(fixture, ownerId, slotA));
    const Json freed = integration::payloadOf(fixture.call(first, "booking.get", slugPayload(slug))).get("page");
    CHECK(booking_test::hasSlot(freed, slotA));

    // Выход из листа с предложением — держание снимается сразу.
    auto again = openFrom(fixture, sockets, "wo-g5", "198.51.100.5:1");
    const Json rebooked = fixture.call(again, "booking.book", bookPayload(slug, slotA, "Пятый"));
    CHECK_EQ(codeOf(rebooked), std::string("ok"));
    CHECK_EQ(codeOf(fixture.call(again, "booking.cancel", tokenPayload(integration::payloadOf(rebooked).getString("token")))),
             std::string("ok"));
    w1 = entryOf(db, ownerId, "wo-w1@example.com");
    CHECK(w1 && w1->status == "offered");
    const std::string token1b = tokenFromMail("wo-w1@example.com", "booking.waitlist_offer");
    CHECK_EQ(token1b, token1);  // одна ссылка на запись листа
    CHECK_EQ(integration::payloadOf(fixture.call(waiter, "booking.waitlistLeave", tokenPayload(token1))).get("entry").getString("status"),
             std::string("left"));
    CHECK(!isBusy(fixture, ownerId, slotA));
    // Запись листа старше 30 дней — expired.
    CHECK_EQ(bookings.waitlistTick(100, std::time(nullptr) + 31 * 86400), std::size_t(1));
    CHECK_EQ(entryOf(db, ownerId, "wo-w3@example.com")->status, std::string("expired"));
}

// F8: перенос освобождает прежнее время; часть дня; забрать — тот же путь оплаты (предоплата).
TEST(waitlist_reschedule_frees_old_slot_and_payment_path) {
    using namespace waitlist_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    invite_test::FakeAi ai([&] { return localSlots({slotA, slotB}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "wr-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "wrowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    Json pageReq = waitPage("Массаж");
    pageReq.set("price", Json(250000LL));
    pageReq.set("prepay", Json("full"));
    pageReq.set("payment_url", Json("https://pay.example/p"));
    const std::string slug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page").getString("slug");
    auto guest = openFrom(fixture, sockets, "wr-g", "192.0.2.1:1");
    const Json booked = fixture.call(guest, "booking.book", bookPayload(slug, slotA, "Гость"));
    CHECK_EQ(codeOf(booked), std::string("ok"));

    const int hour = std::atoi(selfvoy::isoUtc(slotA).substr(11, 2).c_str());
    const std::string right = hour < 12 ? "morning" : hour < 17 ? "afternoon" : "evening";
    const std::string wrong = right == "morning" ? "evening" : "morning";
    auto waiter = openFrom(fixture, sockets, "wr-w", "192.0.2.2:1");
    Json early = joinPayload(slug, "Не то время", "wr-wrong@example.com");
    early.set("part_of_day", Json(wrong));
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistJoin", early)), std::string("ok"));
    Json fits = joinPayload(slug, "Мария", "wr-right@example.com");
    fits.set("part_of_day", Json(right));
    fits.set("comment", Json("Из листа ожидания"));
    CHECK_EQ(codeOf(fixture.call(waiter, "booking.waitlistJoin", fits)), std::string("ok"));

    // Перенос A → B: прежнее время предлагается листу (часть дня — по местному времени).
    Json move = tokenPayload(integration::payloadOf(booked).getString("token"));
    move.set("start", Json(selfvoy::isoUtc(slotB)));
    CHECK_EQ(codeOf(fixture.call(guest, "booking.reschedule", move)), std::string("ok"));
    CHECK_EQ(entryOf(db, ownerId, "wr-wrong@example.com")->status, std::string("waiting"));
    const auto offered = entryOf(db, ownerId, "wr-right@example.com");
    CHECK(offered && offered->status == "offered" && offered->offerStart == selfvoy::isoUtc(slotA));

    // Забрать — как booking.book: снимок цены, предоплата по ссылке, письмо с суммой.
    const Json claimed = fixture.call(waiter, "booking.waitlistClaim",
                                      tokenPayload(tokenFromMail("wr-right@example.com", "booking.waitlist_offer")));
    CHECK_EQ(codeOf(claimed), std::string("ok"));
    const Json payment = integration::payloadOf(claimed).get("payment");
    CHECK(payment.getString("status") == "pending" && payment.getString("kind") == "full" &&
          payment.getInt("amount") == 250000 && payment.getString("method") == "link" &&
          payment.getString("action_url") == "https://pay.example/p" && !payment.contains("provider_id"));
    const Json bookingView = integration::payloadOf(claimed).get("booking");
    CHECK(bookingView.getInt("price") == 250000 && bookingView.getString("guest_name") == "Мария");
    const auto stored = db.findBookingByToken(selfvoy::BookingManager::tokenHash(integration::payloadOf(claimed).getString("token")));
    CHECK(stored && stored->guestComment == "Из листа ожидания" && stored->payment.getString("status") == "pending");
    const auto created = lastMail("wr-right@example.com", "booking.created");
    CHECK(created && footerHas(*created, "Предоплата: 2 500 ₽ — оплатите в течение"));
}

// F8: «пора записаться снова» — ровно один раз, только без более поздних записей гостя,
// не позже 3 дней после срока; выключение поля отменяет неотправленные.
TEST(rebook_reminder_once_later_booking_stale_and_disable) {
    using namespace waitlist_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    const std::time_t slotD = invite_test::future(102);
    invite_test::FakeAi ai([&] { return localSlots({slotA, slotB, slotC, slotD}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "rb-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "rbowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bookings = fixture.server->bookings();
    Json pageReq = payPage("Стрижка");
    pageReq.set("rebook_after_days", Json(7LL));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageReq)).get("page");
    const std::string slug = page.getString("slug");
    auto guest = openFrom(fixture, sockets, "rb-g", "192.0.2.10:1");
    const auto book = [&](std::time_t at, const std::string& email) {
        Json payload = bookPayload(slug, at, "Гость");
        payload.set("guest_email", Json(email));
        return bookingIdOf(db, fixture.call(guest, "booking.book", payload));
    };
    const long long bookingA = book(slotA, "rb-one@example.com");
    const long long bookingB = book(slotB, "rb-two@example.com");
    const long long bookingC = book(slotC, "RB-Two@example.com");
    const long long bookingD = book(slotD, "rb-late@example.com");
    CHECK(bookingA > 0 && bookingB > 0 && bookingC > 0 && bookingD > 0);
    const std::time_t endA = slotA + 3600;
    const auto ownerView = [&](long long id) {
        const Json list = integration::payloadOf(fixture.call(owner, "booking.list")).get("bookings");
        for (const auto& item : list.items()) {
            if (item.getInt("id") == id) return item;
        }
        return Json();
    };
    CHECK_EQ(ownerView(bookingA).getString("rebook_due_at"), selfvoy::isoUtc(endA + 7 * 86400));
    CHECK_EQ(ownerView(bookingA).getString("rebook_sent_at"), std::string());

    CHECK_EQ(bookings.rebookTick(100, std::time(nullptr)), std::size_t(0));
    CHECK_EQ(bookings.rebookTick(100, endA + 7 * 86400 - 60), std::size_t(0));
    CHECK_EQ(bookings.rebookTick(100, endA + 7 * 86400 + 60), std::size_t(1));
    const auto mail = lastMail("rb-one@example.com", "booking.rebook");
    CHECK(mail && mail->subject == "Selfvoy: пора записаться снова — Стрижка" &&
          mail->linkUrl == "https://selfvoy.example.com/#/b/" + slug && mail->linkText == "Записаться снова" &&
          mail->lead.find("С последней встречи прошло 7 дней.") == 0);
    CHECK(!ownerView(bookingA).getString("rebook_sent_at").empty());
    CHECK_EQ(ownerView(bookingA).getString("rebook_due_at"), std::string());
    // Ровно один раз.
    CHECK_EQ(bookings.rebookTick(100, endA + 7 * 86400 + 3600), std::size_t(0));
    CHECK_EQ(mailCount("rb-one@example.com", "booking.rebook"), std::size_t(1));
    // У гостя есть более поздняя запись (C) — за B не пишем, напишет C.
    CHECK_EQ(bookings.rebookTick(100, slotB + 3600 + 7 * 86400 + 60), std::size_t(1));
    CHECK_EQ(db.findBooking(bookingB)->rebookStatus, std::string("skipped"));
    CHECK_EQ(mailCount("rb-two@example.com", "booking.rebook"), std::size_t(0));
    CHECK_EQ(bookings.rebookTick(100, slotC + 3600 + 7 * 86400 + 60), std::size_t(1));
    CHECK_EQ(mailCount("RB-Two@example.com", "booking.rebook"), std::size_t(1));
    CHECK_EQ(db.findBooking(bookingC)->rebookStatus, std::string("sent"));
    // Сервер стоял больше 3 дней после срока — старое не шлём.
    CHECK_EQ(bookings.rebookTick(100, slotD + 3600 + 11 * 86400), std::size_t(1));
    CHECK_EQ(db.findBooking(bookingD)->rebookStatus, std::string("skipped"));
    CHECK_EQ(mailCount("rb-late@example.com", "booking.rebook"), std::size_t(0));

    // Прошедшая запись ждёт напоминания; выключение поля его отменяет, включение не возвращает.
    selfvoy::BookingRecord past;
    past.pageId = page.getInt("id");
    past.ownerId = ownerId;
    past.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    past.guestName = "Прошлый";
    past.guestEmail = "rb-past@example.com";
    const std::time_t pastEnd = std::time(nullptr) - 86400;
    Json occurrence = Json::object();
    occurrence.set("start", Json(selfvoy::isoUtc(pastEnd - 3600)));
    occurrence.set("end", Json(selfvoy::isoUtc(pastEnd)));
    occurrence.set("day", Json(selfvoy::isoUtc(pastEnd).substr(0, 10)));
    occurrence.set("status", Json("active"));
    occurrence.set("task_id", Json(0LL));
    past.occurrences = Json::array();
    past.occurrences.push(occurrence);
    long long pastId = 0;
    CHECK(db.createBooking(past, pastId).ok);
    CHECK_EQ(ownerView(pastId).getString("rebook_due_at"), selfvoy::isoUtc(pastEnd + 7 * 86400));
    Json off = pageId(page.getInt("id"));
    off.set("rebook_after_days", Json(0LL));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", off)), std::string("ok"));
    CHECK_EQ(db.findBooking(pastId)->rebookStatus, std::string("cancelled"));
    CHECK_EQ(ownerView(pastId).getString("rebook_due_at"), std::string());
    Json on = pageId(page.getInt("id"));
    on.set("rebook_after_days", Json(7LL));
    CHECK_EQ(codeOf(fixture.call(owner, "booking.pageUpdate", on)), std::string("ok"));
    CHECK_EQ(bookings.rebookTick(100, pastEnd + 7 * 86400 + 60), std::size_t(0));
    CHECK_EQ(mailCount("rb-past@example.com", "booking.rebook"), std::size_t(0));
}

// F8: подписка на предложения в боте (w_<entry>_<sig>) и сообщение о свободном времени.
TEST(waitlist_telegram_subscription_and_offer) {
    using namespace waitlist_test;
    using growth_test::startPayload;
    using growth_test::textUpdate;
    const std::time_t slotA = invite_test::future(30);
    invite_test::FakeAi ai([&] { return localSlots({slotA}); });
    Fixture fixture;
    growth_test::FakeTelegram* fake = growth_test::enableBot(fixture, ai.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "wt-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "wtowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    const std::string slug =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", waitPage("Разбор"))).get("page").getString("slug");
    auto guest = sockets.open(fixture, "wt-guest");
    const Json booked = fixture.call(guest, "booking.book", bookPayload(slug, slotA, "Гость"));
    const Json joined = integration::payloadOf(fixture.call(guest, "booking.waitlistJoin",
                                                            joinPayload(slug, "Вера", "wt-vera@example.com")));
    const std::string url = joined.getString("telegram_url");
    CHECK(url.rfind("https://t.me/selfvoy_test_bot?start=w_", 0) == 0);
    auto& bot = fixture.server->telegram();
    std::string forged = startPayload(url);
    forged.back() = forged.back() == 'A' ? 'B' : 'A';
    bot.handleUpdate(textUpdate(9101, "/start " + forged));
    bot.handleUpdate(textUpdate(9102, "/start " + startPayload(url)));
    bot.flush();
    CHECK(fake->last(9101).find("Ссылка недействительна") == 0);
    CHECK(fake->last(9102).find("Готово: как только на «Разбор» освободится подходящее время, напишу сюда.") == 0);
    CHECK_EQ(entryOf(db, ownerId, "wt-vera@example.com")->telegramChatId, 9102LL);
    CHECK(integration::payloadOf(fixture.call(owner, "booking.waitlist")).get("entries").at(0).getBool("telegram_linked"));

    CHECK_EQ(codeOf(fixture.call(guest, "booking.cancel", tokenPayload(integration::payloadOf(booked).getString("token")))),
             std::string("ok"));
    fixture.server->bookings().flushMail();
    bot.flush();
    bool button = false;
    for (const auto& call : fake->of("sendMessage")) {
        if (call.body.getInt("chat_id") != 9102 || call.body.getString("text").find("Освободилось время") != 0) continue;
        const Json keyboard = call.body.get("reply_markup").get("inline_keyboard");
        button = keyboard.at(0).at(0).getString("url").rfind("https://selfvoy.example.com/#/w/", 0) == 0;
    }
    CHECK(button);
    // /stop — подписка листа снимается.
    bot.handleUpdate(textUpdate(9102, "/stop"));
    bot.flush();
    CHECK_EQ(entryOf(db, ownerId, "wt-vera@example.com")->telegramChatId, 0LL);
}

// F10: оплата получена → задача и tax.receipt (RUB, настройка включена); tax.pending,
// tax.receiptDone (гость видит чек), возврат → tax.refund; пакеты; депозит.
TEST(npd_receipt_task_pending_done_and_refund) {
    using namespace waitlist_test;
    const std::time_t slotA = invite_test::future(30);
    const std::time_t slotB = invite_test::future(54);
    const std::time_t slotC = invite_test::future(78);
    const std::time_t slotD = invite_test::future(102);
    invite_test::FakeAi ai([&] { return localSlots({slotA, slotB, slotC, slotD}); });
    Fixture fixture;
    enablePayments(fixture, ai.url());
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "tx-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "txowner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    const auto prepaid = [&](const std::string& title, const std::string& currency, const std::string& prepay) {
        Json payload = payPage(title);
        payload.set("price", Json(250000LL));
        payload.set("currency", Json(currency));
        payload.set("prepay", Json(prepay));
        if (prepay == "deposit") payload.set("deposit", Json(50000LL));
        payload.set("payment_url", Json("https://pay.example/p"));
        return integration::payloadOf(fixture.call(owner, "booking.pageCreate", payload)).get("page").getString("slug");
    };
    const std::string rub = prepaid("Массаж", "RUB", "full");
    const std::string usd = prepaid("Coaching", "USD", "full");
    const std::string deposit = prepaid("Депозит", "RUB", "deposit");
    auto guest = openFrom(fixture, sockets, "tx-g", "192.0.2.20:1");
    const auto book = [&](const std::string& slug, std::time_t at, const std::string& name, std::string& token) {
        const Json booked = fixture.call(guest, "booking.book", bookPayload(slug, at, name));
        token = integration::payloadOf(booked).getString("token");
        return bookingIdOf(db, booked);
    };
    const auto setPayment = [&](long long id, const std::string& status) {
        Json payload = Json::object();
        payload.set("booking_id", Json(id));
        payload.set("status", Json(status));
        return fixture.call(owner, "booking.paymentSet", payload);
    };
    const std::string taskTitle = "Выдать чек: Массаж, 2 500 ₽";

    // Настройка выключена — ничего.
    std::string token1;
    const long long booking1 = book(rub, slotA, "Тимур", token1);
    CHECK_EQ(codeOf(setPayment(booking1, "paid")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(0));
    CHECK_EQ(taskCount(db, ownerId, taskTitle), std::size_t(0));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "tax.pending")).get("items").size(), std::size_t(0));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "prefs.get")).getBool("npd_receipts", true), false);
    Json wrong = Json::object();
    wrong.set("npd_receipts", Json("да"));
    CHECK_EQ(codeOf(fixture.call(owner, "prefs.set", wrong)), std::string("bad_request"));
    Json enable = Json::object();
    enable.set("npd_receipts", Json(true));
    CHECK(integration::payloadOf(fixture.call(owner, "prefs.set", enable)).getBool("npd_receipts"));

    // Оплата получена (отметка владельца) → tax.receipt и задача со сроком «сегодня 23:59».
    std::string token2;
    const long long booking2 = book(rub, slotB, "Тимур", token2);
    CHECK_EQ(codeOf(setPayment(booking2, "paid")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(1));
    const Json notice = lastNotice(db, ownerId, "tax.receipt");
    const std::string text = "Наименование: Массаж; Сумма: 2 500 ₽; Покупатель: физическое лицо";
    CHECK(notice.getInt("booking_id") == booking2 && notice.getString("service") == "Массаж" &&
          notice.getInt("amount") == 250000 && notice.getString("currency") == "RUB" &&
          notice.getString("client") == "Тимур" && !notice.getString("paid_at").empty() &&
          notice.getString("text") == text && notice.getString("url") == "https://lknpd.nalog.ru/");
    CHECK_EQ(taskCount(db, ownerId, taskTitle), std::size_t(1));
    const long long taskId = db.findBooking(booking2)->payment.getInt("receipt_task_id");
    const auto task = db.findTask(taskId);
    std::time_t due = 0;
    CHECK(task && task->status == "pending" && selfvoy::parseIsoUtc(task->dueAt, due) && due > std::time(nullptr) &&
          due <= std::time(nullptr) + 86400 && (due + 60) % 3600 == 0);  // 23:59 (пояс UTC)
    CHECK(db.findBooking(booking2)->payment.getBool("npd"));
    // Повторная отметка paid — второй задачи нет.
    CHECK_EQ(codeOf(setPayment(booking2, "waived")), std::string("ok"));
    CHECK_EQ(codeOf(setPayment(booking2, "paid")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(1));
    CHECK_EQ(taskCount(db, ownerId, taskTitle), std::size_t(1));
    const Json pending = integration::payloadOf(fixture.call(owner, "tax.pending")).get("items");
    CHECK(pending.size() == 1 && pending.at(0).getString("kind") == "booking" && pending.at(0).getInt("id") == booking2 &&
          pending.at(0).getString("text") == text && pending.at(0).getString("client") == "Тимур");
    // Гость не видит служебных полей.
    const Json managed = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token2))).get("booking");
    CHECK(!managed.get("payment").contains("npd") && !managed.get("payment").contains("receipt_task_id") &&
          managed.getString("receipt_url").empty());

    // Чек отмечен: ссылка у гостя, задача закрыта, в tax.pending пусто.
    CHECK_EQ(codeOf(fixture.call(owner, "tax.receiptDone", Json::object())), std::string("bad_request"));
    Json badUrl = Json::object();
    badUrl.set("booking_id", Json(booking2));
    badUrl.set("receipt_url", Json("http://lknpd.nalog.ru/x"));
    CHECK_EQ(codeOf(fixture.call(owner, "tax.receiptDone", badUrl)), std::string("bad_request"));
    const std::string receipt = "https://lknpd.nalog.ru/api/v1/receipt/770000000000/200abc/print";
    Json done = Json::object();
    done.set("booking_id", Json(booking2));
    done.set("receipt_url", Json(receipt));
    auto stranger = sockets.open(fixture, "tx-stranger");
    fixture.registerUser(stranger, "txstranger@example.com", "Чужой");
    CHECK_EQ(codeOf(fixture.call(stranger, "tax.receiptDone", done)), std::string("not_found"));
    const Json marked = integration::payloadOf(fixture.call(owner, "tax.receiptDone", done)).get("booking");
    CHECK(marked.get("payment").getString("receipt_url") == receipt && !marked.get("payment").getString("receipt_at").empty());
    CHECK_EQ(db.findTask(taskId)->status, std::string("done"));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "tax.pending")).get("items").size(), std::size_t(0));
    const Json withReceipt = integration::payloadOf(fixture.call(guest, "booking.manage", tokenPayload(token2))).get("booking");
    CHECK(withReceipt.getString("receipt_url") == receipt && withReceipt.get("payment").getString("receipt_url") == receipt);

    // Возврат по оплате с чеком → tax.refund и задача «Аннулировать чек».
    CHECK_EQ(codeOf(setPayment(booking2, "refunded")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.refund"), std::size_t(1));
    CHECK(lastNotice(db, ownerId, "tax.refund").getInt("booking_id") == booking2 &&
          lastNotice(db, ownerId, "tax.refund").getString("receipt_url") == receipt);
    CHECK_EQ(taskCount(db, ownerId, "Аннулировать чек: Массаж, 2 500 ₽"), std::size_t(1));
    // Без чека возврат — без tax.refund (booking1 оплачен до включения настройки).
    CHECK_EQ(codeOf(setPayment(booking1, "refunded")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.refund"), std::size_t(1));

    // Не рубли — ничего; депозит — сумма депозита.
    std::string token3;
    CHECK_EQ(codeOf(setPayment(book(usd, slotC, "Джон", token3), "paid")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(1));
    std::string token4;
    CHECK_EQ(codeOf(setPayment(book(deposit, slotD, "Дина", token4), "paid")), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(2));
    CHECK(lastNotice(db, ownerId, "tax.receipt").getInt("amount") == 50000 &&
          lastNotice(db, ownerId, "tax.receipt").getString("text") ==
              "Наименование: Депозит; Сумма: 500 ₽; Покупатель: физическое лицо");

    // Пакет: выдан с отметкой «оплачено» → чек; отметка чека — гость видит ссылку; возврат.
    Json offers = Json::array();
    offers.push(offer("3 занятия", 3, 900000));
    const long long packagesPage =
        integration::payloadOf(fixture.call(owner, "booking.pageCreate", offersPage("Тренировки", offers))).get("page").getInt("id");
    Json issue = Json::object();
    issue.set("page_id", Json(packagesPage));
    issue.set("offer_id", Json(1LL));
    issue.set("guest_name", Json("Пётр"));
    issue.set("paid", Json(true));
    const Json issued = integration::payloadOf(fixture.call(owner, "package.issue", issue));
    const long long packageId = issued.get("package").getInt("id");
    CHECK_EQ(ownerNotices(db, ownerId, "tax.receipt"), std::size_t(3));
    CHECK(lastNotice(db, ownerId, "tax.receipt").getInt("package_id") == packageId &&
          lastNotice(db, ownerId, "tax.receipt").getString("service") == "Тренировки: абонемент «3 занятия»");
    CHECK(issued.get("package").get("payment").getBool("npd"));
    bool listedPackage = false;
    for (const auto& item : integration::payloadOf(fixture.call(owner, "tax.pending")).get("items").items()) {
        listedPackage = listedPackage || (item.getString("kind") == "package" && item.getInt("id") == packageId);
    }
    CHECK(listedPackage);
    Json packageDone = Json::object();
    packageDone.set("package_id", Json(packageId));
    packageDone.set("receipt_url", Json(receipt));
    CHECK_EQ(integration::payloadOf(fixture.call(owner, "tax.receiptDone", packageDone)).get("package").get("payment").getString("receipt_url"),
             receipt);
    const Json packageView =
        integration::payloadOf(fixture.call(guest, "package.get", tokenPayload(issued.getString("token")))).get("package");
    CHECK(packageView.getString("receipt_url") == receipt && !packageView.get("payment").contains("npd"));
    Json refund = packageIdPayload(packageId);
    refund.set("status", Json("refunded"));
    CHECK_EQ(codeOf(fixture.call(owner, "package.paymentSet", refund)), std::string("ok"));
    CHECK_EQ(ownerNotices(db, ownerId, "tax.refund"), std::size_t(2));
    CHECK_EQ(lastNotice(db, ownerId, "tax.refund").getInt("package_id"), packageId);

    // Включение настройки — Pro (при SELFVOY_PLANS_ENFORCED=1).
    selfvoy::plan::setEnforced(true);
    auto free = sockets.open(fixture, "tx-free");
    fixture.registerUser(free, "txfree@example.com", "Без Pro");
    CHECK_EQ(codeOf(fixture.call(free, "prefs.set", enable)), std::string("pro_required"));
    Json disable = Json::object();
    disable.set("npd_receipts", Json(false));
    CHECK_EQ(codeOf(fixture.call(free, "prefs.set", disable)), std::string("ok"));
    selfvoy::plan::setEnforced(false);
}

// PostgreSQL: лист ожидания (уникальность активной записи, держания, планировщик), захват
// «пора записаться снова» (один раз, SKIP LOCKED), отмена, поздние записи, оплаты к чеку.
TEST(pg_waitlist_rebook_tax_methods) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto db = selfvoy::makePostgresDatabase(url);
    auto other = selfvoy::makePostgresDatabase(url);  // «второй экземпляр сервера»
    std::string error;
    CHECK(db && db->connect(error));
    CHECK(other && other->connect(error));
    if (!db || !other) return;
    long long owner = 0;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    CHECK(db->createUser("pg-wl-" + suffix + "@example.com", "argon2id$x", "Лист", owner).ok);
    selfvoy::BookingPageRecord page;
    page.ownerId = owner;
    page.slug = "wl" + suffix;
    page.slug.resize(10, '0');
    page.title = "Лист";
    page.waitlist = true;
    page.waitlistHoldMinutes = 45;
    page.rebookAfterDays = 7;
    long long pageId = 0;
    CHECK(db->createBookingPage(page, pageId).ok);
    auto storedPage = db->findBookingPage(pageId);
    CHECK(storedPage && storedPage->waitlist && storedPage->waitlistHoldMinutes == 45 && storedPage->rebookAfterDays == 7);
    if (storedPage) {
        storedPage->rebookAfterDays = 200;  // CHECK в схеме
        CHECK(!db->updateBookingPage(*storedPage).ok);
        storedPage->rebookAfterDays = 7;
        storedPage->waitlistHoldMinutes = 90;
        CHECK(db->updateBookingPage(*storedPage).ok);
        CHECK_EQ(db->findBookingPage(pageId)->waitlistHoldMinutes, 90);
    }

    // F10: настройка npd_receipts хранится колонкой user_preferences.
    Json npd = Json::object();
    npd.set("npd_receipts", Json(true));
    CHECK(db->setPreferences(owner, npd).ok);
    CHECK(db->getPreferences(owner).getBool("npd_receipts"));
    npd.set("npd_receipts", Json(false));
    CHECK(db->setPreferences(owner, npd).ok);
    CHECK(!db->getPreferences(owner).getBool("npd_receipts", true));

    // Лист ожидания.
    selfvoy::WaitlistEntryRecord entry;
    entry.ownerId = owner;
    entry.pageId = pageId;
    entry.guestName = "Гость";
    entry.guestEmail = "Wait@Example.com";
    Json days = Json::array();
    days.push(Json("2030-01-02"));
    entry.days = days;
    entry.partOfDay = "evening";
    entry.comment = "после 18";
    entry.tokenHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    entry.tokenEnc = "sealed";
    entry.expiresAt = "2030-02-01T00:00:00Z";
    long long entryId = 0;
    CHECK(db->createWaitlistEntry(entry, entryId).ok && entryId > 0);
    selfvoy::WaitlistEntryRecord duplicate = entry;
    duplicate.tokenHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    duplicate.guestEmail = "wait@example.COM";
    long long duplicateId = 0;
    CHECK(!db->createWaitlistEntry(duplicate, duplicateId).ok);  // одна активная запись на email
    auto found = db->findWaitlistEntry(entryId);
    CHECK(found && found->guestEmail == "Wait@Example.com" && found->days.size() == 1 && found->partOfDay == "evening" &&
          found->comment == "после 18" && found->status == "waiting" && found->tokenEnc == "sealed" &&
          found->expiresAt == "2030-02-01T00:00:00Z" && !found->queuedAt.empty() && found->offerStart.empty());
    CHECK(db->findWaitlistEntryByToken(entry.tokenHash).has_value());
    CHECK(db->findActiveWaitlistEntry(pageId, "wait@example.com").has_value());
    CHECK_EQ(db->countActiveWaitlistEntries(pageId), 1);
    CHECK_EQ(db->listWaitingEntries(pageId, "2030-01-01T00:00:00Z", 10).size(), std::size_t(1));
    CHECK_EQ(db->listWaitingEntries(pageId, "2030-03-01T00:00:00Z", 10).size(), std::size_t(0));
    if (found) {
        found->status = "offered";
        found->offerStart = "2030-01-02T18:00:00Z";
        found->offerEnd = "2030-01-02T19:00:00Z";
        found->offerDay = "2030-01-02";
        found->offerLocal = "2030-01-02T21:00";
        found->offerExpiresAt = "2030-01-01T12:00:00Z";
        found->offerStatus = "offered";
        CHECK(db->updateWaitlistEntry(*found).ok);
        const auto holds = db->listWaitlistHolds(owner, "2030-01-01T11:00:00Z");
        CHECK(holds.size() == 1 && holds.front().offerStart == "2030-01-02T18:00:00Z" &&
              holds.front().offerLocal == "2030-01-02T21:00");
        CHECK_EQ(db->listWaitlistHolds(owner, "2030-01-01T12:30:00Z").size(), std::size_t(0));
        bool due = false;
        for (const auto& item : db->listDueWaitlist("2030-01-01T12:30:00Z", 1000)) due = due || item.id == entryId;
        CHECK(due);
        for (const auto& item : db->listDueWaitlist("2030-01-01T11:30:00Z", 1000)) CHECK(item.id != entryId);
        CHECK(db->setWaitlistTelegram(entryId, 777001).ok);
        CHECK_EQ(db->findWaitlistEntry(entryId)->telegramChatId, 777001LL);
        CHECK(db->unlinkTelegramChat(777001).ok);
        CHECK_EQ(db->findWaitlistEntry(entryId)->telegramChatId, 0LL);
        found->status = "left";
        found->offerStatus = "cancelled";
        CHECK(db->updateWaitlistEntry(*found).ok);
        CHECK(db->createWaitlistEntry(duplicate, duplicateId).ok);  // прежняя закрыта — можно снова
        found->status = "lost";  // CHECK в схеме
        CHECK(!db->updateWaitlistEntry(*found).ok);
    }
    CHECK_EQ(db->listWaitlistEntries(owner, pageId, "", 10).size(), std::size_t(2));
    CHECK_EQ(db->listWaitlistEntries(owner, pageId, "left", 10).size(), std::size_t(1));

    // «Пора записаться снова»: два экземпляра захватывают каждую запись один раз.
    const auto makeBooking = [&](const std::string& email, const std::string& start, const std::string& end) {
        selfvoy::BookingRecord booking;
        booking.pageId = pageId;
        booking.ownerId = owner;
        booking.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
        booking.guestName = "Гость";
        booking.guestEmail = email;
        Json occurrence = Json::object();
        occurrence.set("start", Json(start));
        occurrence.set("end", Json(end));
        occurrence.set("status", Json("active"));
        booking.occurrences = Json::array();
        booking.occurrences.push(occurrence);
        long long id = 0;
        CHECK(db->createBooking(booking, id).ok);
        return id;
    };
    const long long early = makeBooking("one@example.com", "2001-01-01T10:00:00Z", "2001-01-01T11:00:00Z");
    const long long later = makeBooking("ONE@example.com", "2001-01-05T10:00:00Z", "2001-01-05T11:00:00Z");
    const long long future = makeBooking("two@example.com", "2001-03-01T10:00:00Z", "2001-03-01T11:00:00Z");
    CHECK(db->hasLaterBooking(pageId, "one@example.com", early, "2001-01-01T11:00:00Z"));
    CHECK(!db->hasLaterBooking(pageId, "one@example.com", later, "2001-01-05T11:00:00Z"));
    CHECK(!db->hasLaterBooking(pageId, "", later, "2001-01-05T11:00:00Z"));
    std::vector<long long> claimed;
    for (const auto& booking : db->claimDueRebooks("2001-01-20T00:00:00Z", 100)) claimed.push_back(booking.id);
    for (const auto& booking : other->claimDueRebooks("2001-01-20T00:00:00Z", 100)) claimed.push_back(booking.id);
    CHECK(std::count(claimed.begin(), claimed.end(), early) == 1 && std::count(claimed.begin(), claimed.end(), later) == 1);
    CHECK(std::count(claimed.begin(), claimed.end(), future) == 0);
    auto claimedBooking = db->findBooking(early);
    CHECK(claimedBooking && claimedBooking->rebookStatus == "claimed" &&
          claimedBooking->rebookSentAt == "2001-01-20T00:00:00Z");
    CHECK(db->setRebookStatus(early, "sent").ok);
    CHECK_EQ(db->findBooking(early)->rebookStatus, std::string("sent"));
    CHECK(!db->setRebookStatus(later, "bogus").ok);  // CHECK в схеме
    CHECK(db->cancelPendingRebooks(pageId, "2001-03-02T00:00:00Z").ok);
    CHECK_EQ(db->findBooking(future)->rebookStatus, std::string("cancelled"));
    for (const auto& booking : db->claimDueRebooks("2001-12-01T00:00:00Z", 100)) CHECK(booking.pageId != pageId);

    // Оплаты, ждущие чек «Мой налог».
    selfvoy::BookingRecord paid;
    paid.pageId = pageId;
    paid.ownerId = owner;
    paid.manageHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    paid.guestName = "Плательщик";
    Json payment = Json::object();
    payment.set("status", Json("paid"));
    payment.set("amount", Json(100000LL));
    payment.set("currency", Json("RUB"));
    payment.set("npd", Json(true));
    payment.set("receipt_at", Json(""));
    paid.payment = payment;
    long long paidId = 0;
    CHECK(db->createBooking(paid, paidId).ok);
    const auto pendingReceipts = db->listReceiptPendingBookings(owner, 10);
    CHECK(pendingReceipts.size() == 1 && pendingReceipts.front().id == paidId);
    auto receipted = db->findBooking(paidId);
    if (receipted) {
        Json withReceipt = receipted->payment;
        withReceipt.set("receipt_at", Json("2001-01-01T00:00:00Z"));
        receipted->payment = withReceipt;
        CHECK(db->updateBooking(*receipted).ok);
    }
    CHECK_EQ(db->listReceiptPendingBookings(owner, 10).size(), std::size_t(0));
    selfvoy::PackagePurchaseRecord purchase;
    purchase.ownerId = owner;
    purchase.pageId = pageId;
    purchase.title = "Абонемент";
    purchase.sessionsTotal = 3;
    purchase.status = "active";
    purchase.payment = payment;
    purchase.guestName = "Плательщик";
    purchase.tokenHash = selfvoy::BookingManager::tokenHash(growth_test::newToken());
    long long purchaseId = 0;
    CHECK(db->createPackagePurchase(purchase, purchaseId).ok);
    const auto pendingPackages = db->listReceiptPendingPackages(owner, 10);
    CHECK(pendingPackages.size() == 1 && pendingPackages.front().id == purchaseId);
}

// ------------------------------------------- F7: ИИ-администратор в Telegram
namespace f7_test {

using invite_test::FakeAi;
using invite_test::future;

// Поддельный AI для F7: /v1/agent/admin → admin (тело как есть), /v1/agent/series →
// повторы, иначе (/v1/agent/slots) — три свободных окна в один день.
struct Ai {
    std::time_t first = future(30);
    std::string admin = R"({"reply":"Здравствуйте!","action":"none","slots":[]})";
    std::vector<std::string> adminRequests;
    FakeAi* self = nullptr;
    FakeAi fake;

    Ai()
        : fake([this] {
              const std::string& request = self->requests.back();
              if (request.find("POST /v1/agent/admin") != std::string::npos) {
                  adminRequests.push_back(request.substr(request.find("\r\n\r\n") + 4));
                  return admin;
              }
              if (request.find("POST /v1/agent/series") != std::string::npos) return booking_test::seriesBody(request);
              const std::string day = selfvoy::isoUtc(first).substr(0, 10);
              Json slots = Json::array();
              for (int i = 0; i < 3; ++i) slots.push(booking_test::daySlot(first + i * 7200, 60, day));
              Json body = Json::object();
              body.set("slots", slots);
              return body.dump();
          }) {
        self = &fake;
    }
    // Последний запрос к /v1/agent/admin (разобранный JSON).
    Json lastAdmin() {
        std::lock_guard<std::mutex> lock(fake.mutex);
        return adminRequests.empty() ? Json() : Json::parse(adminRequests.back());
    }
    std::size_t adminCalls() {
        std::lock_guard<std::mutex> lock(fake.mutex);
        return adminRequests.size();
    }
    void answer(const std::string& body) {
        std::lock_guard<std::mutex> lock(fake.mutex);
        admin = body;
    }
};

std::size_t utf8Count(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char ch : text) {
        if ((ch & 0xC0) != 0x80) ++count;
    }
    return count;
}

Json guestText(long long chatId, const std::string& text, const std::string& first = "Анна",
               const std::string& last = "Смирнова", const std::string& username = "anna_tg") {
    Json update = growth_test::textUpdate(chatId, text, "private", username);
    Json message = update.get("message");
    Json from = message.get("from");
    if (!first.empty()) from.set("first_name", Json(first));
    if (!last.empty()) from.set("last_name", Json(last));
    message.set("from", from);
    update.set("message", message);
    return update;
}

// Вызовы sendMessage в чат начиная с mark-го вызова sendMessage.
std::vector<growth_test::FakeTelegram::Call> sentTo(growth_test::FakeTelegram& fake, long long chatId,
                                                    std::size_t mark = 0) {
    std::vector<growth_test::FakeTelegram::Call> out;
    const auto all = fake.of("sendMessage");
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i >= mark && all[i].body.getInt("chat_id") == chatId) out.push_back(all[i]);
    }
    return out;
}

std::size_t sendCount(growth_test::FakeTelegram& fake) { return fake.of("sendMessage").size(); }

// callback_data кнопки row сообщения.
std::string callbackOf(const growth_test::FakeTelegram::Call& call, std::size_t row = 0) {
    const Json keyboard = call.body.get("reply_markup").get("inline_keyboard");
    return keyboard.at(row).at(0).getString("callback_data");
}

std::string lastAnswer(growth_test::FakeTelegram& fake) {
    const auto answers = fake.of("answerCallbackQuery");
    return answers.empty() ? std::string() : answers.back().body.getString("text");
}

Json pageWithAdmin(const std::string& title, bool aiAdmin) {
    Json payload = booking_test::pagePayload(title);
    payload.set("ai_admin", Json(aiAdmin));
    payload.set("price", Json(250000LL));
    return payload;
}

// Сервер с ботом и поддельным AI, владелец «Ольга», страница «Массаж» с администратором,
// гость 7201 привязан командой /start p_<slug>.
struct Stage {
    Ai ai;
    Fixture fixture;
    growth_test::FakeTelegram* fake = nullptr;
    admin_test::Sockets sockets;
    std::shared_ptr<selfvoy::Session> owner;
    long long ownerId = 0;
    long long pageId = 0;
    std::string slug;
    static constexpr long long kGuest = 7201;

    explicit Stage(const std::string& email) {
        fake = growth_test::enableBot(fixture, ai.fake.url(), "https://selfvoy.example.com");
        owner = sockets.open(fixture, "f7-" + email);
        ownerId = admin_test::userIdOf(fixture.registerUser(owner, email, "Ольга"));
        const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageWithAdmin("Массаж", true)))
                              .get("page");
        pageId = page.getInt("id");
        slug = page.getString("slug");
        bot().handleUpdate(guestText(kGuest, "/start p_" + slug));
        bot().flush();
    }
    selfvoy::TelegramBot& bot() { return fixture.server->telegram(); }
    selfvoy::IDatabase& db() { return fixture.server->database().db(); }
    void say(const std::string& text, long long chat = kGuest) {
        bot().handleUpdate(guestText(chat, text));
        bot().flush();
    }
    void press(const std::string& data, long long chat = kGuest) {
        bot().handleUpdate(growth_test::callbackUpdate(chat, data));
        fixture.server->bookings().flushMail();
        bot().flush();
    }
};

}  // namespace f7_test

// Поле ai_admin (нужен бот, Pro), telegram_ask_url, привязка /start p_<slug>: неизвестная
// страница, выключенный администратор, нет Pro, пауза.
TEST(telegram_admin_start_binding_and_page_fields) {
    using namespace f7_test;
    Ai ai;
    {
        // Бот не настроен — включить нельзя (bad_request с понятным текстом).
        Fixture plain;
        invite_test::useAi(plain, ai.fake.url(), "https://selfvoy.example.com");
        admin_test::Sockets sockets;
        auto owner = sockets.open(plain, "f7-nobot");
        plain.registerUser(owner, "f7nobot@example.com", "Ольга");
        const Json refused = plain.call(owner, "booking.pageCreate", pageWithAdmin("Массаж", true));
        CHECK_EQ(refused.getString("code"), std::string("bad_request"));
        CHECK(refused.getString("message").find("Telegram-бот") != std::string::npos);
        const Json page = integration::payloadOf(plain.call(owner, "booking.pageCreate", pageWithAdmin("Массаж", false)))
                              .get("page");
        CHECK_EQ(page.getBool("ai_admin", true), false);
        CHECK_EQ(page.getString("telegram_ask_url", "x"), std::string());
    }
    Fixture fixture;
    growth_test::FakeTelegram* fake = growth_test::enableBot(fixture, ai.fake.url(), "https://selfvoy.example.com");
    admin_test::Sockets sockets;
    auto owner = sockets.open(fixture, "f7-owner");
    const long long ownerId = admin_test::userIdOf(fixture.registerUser(owner, "f7owner@example.com", "Ольга"));
    auto& db = fixture.server->database().db();
    auto& bot = fixture.server->telegram();

    Json wrongType = pageWithAdmin("Массаж", true);
    wrongType.set("ai_admin", Json("yes"));
    CHECK_EQ(fixture.call(owner, "booking.pageCreate", wrongType).getString("code"), std::string("bad_request"));
    const Json page = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageWithAdmin("Массаж", true)))
                          .get("page");
    const std::string slug = page.getString("slug");
    const long long pageId = page.getInt("id");
    CHECK(page.getBool("ai_admin"));
    CHECK_EQ(page.getString("telegram_ask_url"), "https://t.me/selfvoy_test_bot?start=p_" + slug);
    CHECK_EQ(db.findBookingPage(pageId)->aiAdmin, true);
    auto guest = sockets.open(fixture, "f7-guest");
    const Json view =
        integration::payloadOf(fixture.call(guest, "booking.get", booking_test::slugPayload(slug))).get("page");
    CHECK_EQ(view.getString("telegram_ask_url"), "https://t.me/selfvoy_test_bot?start=p_" + slug);

    // Неизвестная страница и мусор — «не найдена», чат не привязан.
    bot.handleUpdate(guestText(7101, "/start p_zzzzzzzzzz"));
    bot.handleUpdate(guestText(7101, "/start p_bad"));
    bot.flush();
    const auto unknown = sentTo(*fake, 7101);
    CHECK_EQ(unknown.size(), std::size_t(2));
    for (const auto& item : unknown) CHECK(item.body.getString("text").find("Страница записи не найдена") == 0);
    CHECK(!db.findTelegramGuestChat(7101).has_value());

    // Администратор выключен на странице — отказ со ссылкой; текст в чате — справка.
    const Json off = integration::payloadOf(fixture.call(owner, "booking.pageCreate", pageWithAdmin("Без бота", false)))
                         .get("page");
    CHECK_EQ(off.getString("telegram_ask_url", "x"), std::string());
    bot.handleUpdate(guestText(7102, "/start p_" + off.getString("slug")));
    bot.handleUpdate(guestText(7102, "Сколько стоит?"));
    bot.flush();
    const auto disabledAnswers = sentTo(*fake, 7102);
    CHECK_EQ(disabledAnswers.size(), std::size_t(2));
    if (disabledAnswers.size() == 2) {
        CHECK(disabledAnswers[0].body.getString("text").find("нет ИИ-администратора") != std::string::npos);
        CHECK(disabledAnswers[0].body.getString("text").find("https://selfvoy.example.com/#/b/" + off.getString("slug")) !=
              std::string::npos);
        CHECK(disabledAnswers[1].body.getString("text").find("Это бот Selfvoy") == 0);
    }
    CHECK(!db.findTelegramGuestChat(7102).has_value());
    CHECK_EQ(ai.adminCalls(), std::size_t(0));

    // Тарифы включены, Pro нет: ссылка пустая, привязка не работает, включить — pro_required.
    selfvoy::plan::setEnforced(true);
    CHECK_EQ(integration::payloadOf(fixture.call(guest, "booking.get", booking_test::slugPayload(slug)))
                 .get("page")
                 .getString("telegram_ask_url", "x"),
             std::string());
    bot.handleUpdate(guestText(7103, "/start p_" + slug));
    bot.flush();
    CHECK(fake->last(7103).find("нет ИИ-администратора") != std::string::npos);
    CHECK(!db.findTelegramGuestChat(7103).has_value());
    Json enable = Json::object();
    enable.set("page_id", Json(off.getInt("id")));
    enable.set("ai_admin", Json(true));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("code"), std::string("pro_required"));
    // С Pro — привязка: приветствие, имя и ник из Telegram, пустая история.
    CHECK(db.setUserPlanDays(ownerId, 30).ok);
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", enable).getString("type"), std::string("ok"));
    bot.handleUpdate(guestText(7103, "/start p_" + slug));
    bot.flush();
    CHECK(fake->last(7103).find("Здравствуйте! Я ИИ-администратор «Массаж» (Ольга)") == 0);
    const auto bound = db.findTelegramGuestChat(7103);
    CHECK(bound.has_value());
    if (bound) {
        CHECK_EQ(bound->pageId, pageId);
        CHECK_EQ(bound->ownerId, ownerId);
        CHECK_EQ(bound->guestName, std::string("Анна Смирнова"));
        CHECK_EQ(bound->username, std::string("anna_tg"));
        CHECK_EQ(bound->history.size(), std::size_t(0));
    }
    selfvoy::plan::setEnforced(false);

    // Пауза — запись закрыта, не привязывает; выключение поля — ссылка пустая.
    Json pause = Json::object();
    pause.set("page_id", Json(pageId));
    pause.set("status", Json("paused"));
    CHECK_EQ(fixture.call(owner, "booking.pageUpdate", pause).getString("type"), std::string("ok"));
    bot.handleUpdate(guestText(7104, "/start p_" + slug));
    bot.flush();
    CHECK(fake->last(7104).find("сейчас закрыта") != std::string::npos);
    CHECK(!db.findTelegramGuestChat(7104).has_value());
    Json disable = Json::object();
    disable.set("page_id", Json(pageId));
    disable.set("status", Json("active"));
    disable.set("ai_admin", Json(false));
    const Json disabled = integration::payloadOf(fixture.call(owner, "booking.pageUpdate", disable)).get("page");
    CHECK_EQ(disabled.getBool("ai_admin", true), false);
    CHECK_EQ(disabled.getString("telegram_ask_url", "x"), std::string());
    // Привязанный раньше чат: администратор выключен — ответ со ссылкой, без вызова ИИ.
    bot.handleUpdate(guestText(7103, "Когда можно записаться?"));
    bot.flush();
    CHECK(fake->last(7103).find("Записаться можно на странице: https://selfvoy.example.com/#/b/" + slug) !=
          std::string::npos);
    CHECK_EQ(ai.adminCalls(), std::size_t(0));
    // /stop — привязка к странице удаляется.
    bot.handleUpdate(guestText(7103, "/stop"));
    bot.flush();
    CHECK(!db.findTelegramGuestChat(7103).has_value());
}

// Ответ ИИ не источник истины: чистка reply, только времена из free_slots, неизвестный
// action → none, длинный ответ обрезается.
TEST(telegram_admin_sanitize_ai_reply) {
    using Admin = selfvoy::TelegramAdmin;
    const std::time_t base = invite_test::future(30);
    const std::vector<std::time_t> free = {base,         base + 3600,  base + 7200, base + 10800,
                                           base + 14400, base + 18000, base + 21600};
    const auto iso = [](std::time_t at) { return Json(selfvoy::isoUtc(at)); };

    Json answer = Json::object();
    answer.set("reply", Json(std::string("\x01Привет\tмир\r\n")));
    answer.set("action", Json("offer_slots"));
    Json slots = Json::array();
    slots.push(iso(base));
    slots.push(iso(base + 50 * 3600));  // не из списка
    slots.push(iso(base));              // повтор
    slots.push(Json(42LL));
    slots.push(Json("завтра в 10"));
    slots.push(iso(base + 3600));
    answer.set("slots", slots);
    Admin::Decision decision = Admin::sanitize(answer, free);
    CHECK(decision.usable);
    CHECK_EQ(decision.reply, std::string("Привет мир"));
    CHECK_EQ(decision.action, std::string("offer_slots"));
    CHECK(decision.slots == (std::vector<std::time_t>{base, base + 3600}));

    // Неизвестный action → none, слоты отбрасываются.
    answer.set("action", Json("delete_everything"));
    decision = Admin::sanitize(answer, free);
    CHECK_EQ(decision.action, std::string("none"));
    CHECK(decision.slots.empty());
    answer.set("action", Json(5LL));
    CHECK_EQ(Admin::sanitize(answer, free).action, std::string("none"));

    // Все времена чужие — offer_slots превращается в none; book с верным временем остаётся.
    Json foreign = Json::array();
    foreign.push(iso(base + 1800));
    answer.set("action", Json("offer_slots"));
    answer.set("slots", foreign);
    CHECK_EQ(Admin::sanitize(answer, free).action, std::string("none"));
    Json one = Json::array();
    one.push(iso(base + 7200));
    answer.set("action", Json("book"));
    answer.set("slots", one);
    decision = Admin::sanitize(answer, free);
    CHECK_EQ(decision.action, std::string("book"));
    CHECK(decision.slots == (std::vector<std::time_t>{base + 7200}));
    // Не больше 6 кнопок.
    Json all = Json::array();
    for (const std::time_t at : free) all.push(iso(at));
    answer.set("slots", all);
    CHECK_EQ(Admin::sanitize(answer, free).slots.size(), std::size_t(6));

    // Длинный ответ — ровно 1000 символов; escalate — причина в одну строку, без слотов.
    answer = Json::object();
    std::string longText;
    for (int i = 0; i < 1500; ++i) longText += "я";
    answer.set("reply", Json(longText));
    answer.set("action", Json("escalate"));
    answer.set("escalate_reason", Json("медицинский\nвопрос"));
    answer.set("slots", one);
    decision = Admin::sanitize(answer, free);
    CHECK_EQ(f7_test::utf8Count(decision.reply), std::size_t(1000));
    CHECK_EQ(decision.action, std::string("escalate"));
    CHECK(decision.slots.empty());
    CHECK_EQ(decision.reason, std::string("медицинский вопрос"));

    // Не объект / пустой ответ без действия — непригоден (гостю — ссылка на страницу).
    CHECK(!Admin::sanitize(Json("строка"), free).usable);
    Json empty = Json::object();
    empty.set("reply", Json("   "));
    empty.set("action", Json("none"));
    CHECK(!Admin::sanitize(empty, free).usable);
    empty.set("action", Json("escalate"));
    CHECK(Admin::sanitize(empty, free).usable);
}

// Диалог: запрос к ИИ (страница, free_slots, история), кнопки времени (подписаны и
// привязаны к чату), «Записать на …?» и запись обычным путём booking.book по второму нажатию.
TEST(telegram_admin_two_step_booking) {
    using namespace f7_test;
    Stage stage("f7book@example.com");
    auto& fake = *stage.fake;
    const std::time_t first = stage.ai.first;
    const std::time_t second = first + 7200;

    stage.ai.answer(R"({"reply":"Стоимость — 2 500 ₽ за 60 мин.","action":"none","slots":[]})");
    std::size_t mark = sendCount(fake);
    stage.say("Сколько стоит?");
    auto sent = sentTo(fake, Stage::kGuest, mark);
    CHECK_EQ(sent.size(), std::size_t(1));
    if (!sent.empty()) {
        CHECK_EQ(sent[0].body.getString("text"), std::string("Стоимость — 2 500 ₽ за 60 мин."));
        CHECK(!sent[0].body.get("reply_markup").isObject());
    }
    Json request = stage.ai.lastAdmin();
    CHECK_EQ(request.getString("message"), std::string("Сколько стоит?"));
    CHECK_EQ(request.get("page").getString("title"), std::string("Массаж"));
    CHECK_EQ(request.get("page").getInt("price"), 250000LL);
    CHECK_EQ(request.get("page").getString("price_text"), selfvoy::slotspost::moneyLabel(250000, "RUB"));
    CHECK_EQ(request.get("page").getString("owner_name"), std::string("Ольга"));
    CHECK(request.get("page").contains("timezone") && request.get("page").contains("policy_text"));
    CHECK_EQ(request.get("free_slots").size(), std::size_t(3));
    CHECK_EQ(request.get("free_slots").at(0).asString(), selfvoy::isoUtc(first));
    CHECK_EQ(request.get("history").size(), std::size_t(0));

    // offer_slots: время не из списка отброшено, кнопки — только свободные.
    Json offer = Json::object();
    offer.set("reply", Json("Есть такие окна:"));
    offer.set("action", Json("offer_slots"));
    Json slots = Json::array();
    slots.push(Json(selfvoy::isoUtc(first)));
    slots.push(Json(selfvoy::isoUtc(first + 50 * 3600)));
    slots.push(Json(selfvoy::isoUtc(second)));
    offer.set("slots", slots);
    stage.ai.answer(offer.dump());
    mark = sendCount(fake);
    stage.say("Когда можно записаться?");
    sent = sentTo(fake, Stage::kGuest, mark);
    CHECK_EQ(sent.size(), std::size_t(1));
    if (sent.empty()) return;
    const Json keyboard = sent[0].body.get("reply_markup").get("inline_keyboard");
    CHECK_EQ(keyboard.size(), std::size_t(2));
    const std::string pick = callbackOf(sent[0], 0);
    CHECK(pick.rfind("as:" + std::to_string(stage.pageId) + ":" + std::to_string(static_cast<long long>(first)) + ":",
                     0) == 0);
    CHECK(pick.size() <= 64);
    CHECK_EQ(stage.ai.lastAdmin().get("history").size(), std::size_t(2));  // вопрос о цене и ответ

    // Чужой чат и подделка подписи — отказ, вопроса «Записать?» нет.
    mark = sendCount(fake);
    stage.press(pick, 7299);
    CHECK(lastAnswer(fake).find("не для этого чата") != std::string::npos);
    std::string forged = pick;
    forged.back() = forged.back() == 'A' ? 'B' : 'A';
    stage.press(forged);
    CHECK(lastAnswer(fake).find("не для этого чата") != std::string::npos);
    CHECK_EQ(sendCount(fake), mark);

    // Первое нажатие — только вопрос «Записать на …?», записи ещё нет.
    stage.press(pick);
    CHECK_EQ(lastAnswer(fake), std::string("Подтвердите запись"));
    sent = sentTo(fake, Stage::kGuest, mark);
    CHECK_EQ(sent.size(), std::size_t(1));
    if (sent.empty()) return;
    CHECK(sent[0].body.getString("text").find("Записать вас на ") == 0);
    CHECK(sent[0].body.getString("text").find("Имя для записи: Анна Смирнова") != std::string::npos);
    CHECK(sent[0].body.getString("text").find("Стоимость: 2") != std::string::npos);
    const std::string confirm = callbackOf(sent[0], 0);
    CHECK(confirm.rfind("ab:", 0) == 0);
    CHECK_EQ(callbackOf(sent[0], 1), std::string("an"));
    CHECK(stage.db().listBookings(stage.ownerId, 0, false, 10).empty());

    // «Нет» — ничего не записано; подтверждение из чужого чата — отказ.
    stage.press("an");
    CHECK_EQ(lastAnswer(fake), std::string("Хорошо, не записываю"));
    stage.press(confirm, 7299);
    CHECK(lastAnswer(fake).find("не для этого чата") != std::string::npos);
    CHECK(stage.db().listBookings(stage.ownerId, 0, false, 10).empty());

    // Второе нажатие — запись обычным путём: имя из Telegram, без email, чат привязан к записи.
    mark = sendCount(fake);
    stage.press(confirm);
    CHECK_EQ(lastAnswer(fake), std::string("Записано"));
    const auto bookings = stage.db().listBookings(stage.ownerId, 0, false, 10);
    CHECK_EQ(bookings.size(), std::size_t(1));
    if (bookings.empty()) return;
    const auto& booking = bookings.front();
    CHECK_EQ(booking.guestName, std::string("Анна Смирнова"));
    CHECK_EQ(booking.guestEmail, std::string());
    CHECK(booking.guestComment.find("@anna_tg") != std::string::npos);
    CHECK_EQ(booking.occurrences.at(0).getString("start"), selfvoy::isoUtc(first));
    CHECK_EQ(booking.price, 250000LL);
    CHECK(growth_test::hasChat(stage.db(), booking.id, Stage::kGuest));
    CHECK_EQ(booking_test::countNotifications(stage.fixture, stage.owner, "booking.created"), 1);
    CHECK(!stage.db().listBookingReminders(booking.id).empty());
    sent = sentTo(fake, Stage::kGuest, mark);
    bool notice = false;
    bool done = false;
    for (const auto& item : sent) {
        const std::string text = item.body.getString("text");
        if (text.find("Вы записаны") == 0) notice = true;  // извещение booking.created — в этот чат
        if (text.find("Готово: вы записаны на ") == 0) done = true;
    }
    CHECK(notice);
    CHECK(done);
    CHECK(!fake.of("editMessageReplyMarkup").empty());

    // Повторное подтверждение — время уже занято, второй записи нет.
    stage.press(confirm);
    CHECK(lastAnswer(fake).find("уже заняли") != std::string::npos);
    CHECK_EQ(stage.db().listBookings(stage.ownerId, 0, false, 10).size(), std::size_t(1));
    // Занятое время больше не среди free_slots.
    stage.say("А ещё время есть?");
    const Json after = stage.ai.lastAdmin().get("free_slots");
    bool stillFree = false;
    for (const auto& item : after.items()) stillFree = stillFree || item.asString() == selfvoy::isoUtc(first);
    CHECK(!stillFree);
    CHECK(stage.ai.lastAdmin().get("history").size() <= selfvoy::TelegramAdmin::kMaxHistory);
}

// Предоплата: после записи бот присылает сумму и ссылку на оплату.
TEST(telegram_admin_booking_with_prepay_link) {
    using namespace f7_test;
    Stage stage("f7pay@example.com");
    Json prepay = Json::object();
    prepay.set("page_id", Json(stage.pageId));
    prepay.set("prepay", Json("full"));
    prepay.set("payment_url", Json("https://pay.example.com/olga"));
    CHECK_EQ(stage.fixture.call(stage.owner, "booking.pageUpdate", prepay).getString("type"), std::string("ok"));
    stage.press(stage.fixture.server->telegramAdmin().callbackData("as", Stage::kGuest, stage.pageId, stage.ai.first));
    const std::string question = stage.fake->last(Stage::kGuest);
    CHECK(question.find("Нужна предоплата") != std::string::npos);
    std::size_t mark = sendCount(*stage.fake);
    stage.press(stage.fixture.server->telegramAdmin().callbackData("ab", Stage::kGuest, stage.pageId, stage.ai.first));
    CHECK_EQ(lastAnswer(*stage.fake), std::string("Записано"));
    bool payment = false;
    for (const auto& item : sentTo(*stage.fake, Stage::kGuest, mark)) {
        const std::string text = item.body.getString("text");
        if (text.find("Готово: вы записаны") != 0) continue;
        payment = text.find("внесите предоплату 2") != std::string::npos &&
                  text.find("Оплатить: https://pay.example.com/olga") != std::string::npos;
        CHECK_EQ(item.body.get("reply_markup").get("inline_keyboard").at(0).at(0).getString("url"),
                 std::string("https://pay.example.com/olga"));
    }
    CHECK(payment);
    const auto bookings = stage.db().listBookings(stage.ownerId, 0, false, 10);
    CHECK(bookings.size() == 1 && bookings.front().payment.getString("status") == "pending");
}

// Лимиты (сообщения чата и вызовы ИИ владельца в сутки), обрезка длинного сообщения,
// сбой ИИ и неизвестное действие — ответ со ссылкой на страницу / без кнопок.
TEST(telegram_admin_limits_and_ai_failure) {
    using namespace f7_test;
    Stage stage("f7limits@example.com");
    auto& fake = *stage.fake;
    auto& admin = stage.fixture.server->telegramAdmin();
    const std::string link = "https://selfvoy.example.com/#/b/" + stage.slug;

    // Длинное сообщение обрезается до 1000 символов.
    std::string longText;
    for (int i = 0; i < 1500; ++i) longText += "ж";
    stage.say(longText);
    CHECK_EQ(utf8Count(stage.ai.lastAdmin().getString("message")), std::size_t(1000));

    // Сбой ИИ (не JSON) — ответ со ссылкой на страницу и url-кнопкой.
    stage.ai.answer("oops");
    stage.say("Есть ли парковка?");
    CHECK(fake.last(Stage::kGuest).find("Сейчас не получается ответить автоматически. Записаться можно на странице: " +
                                        link) == 0);
    const auto lastCall = sentTo(fake, Stage::kGuest).back();
    CHECK_EQ(lastCall.body.get("reply_markup").get("inline_keyboard").at(0).at(0).getString("url"), link);
    // Неизвестное действие → просто ответ, без кнопок.
    stage.ai.answer(R"({"reply":"Парковка есть во дворе.","action":"rm -rf","slots":["2030-01-01T10:00:00Z"]})");
    stage.say("Есть ли парковка?");
    CHECK_EQ(fake.last(Stage::kGuest), std::string("Парковка есть во дворе."));
    CHECK(!sentTo(fake, Stage::kGuest).back().body.get("reply_markup").isObject());

    // Лимит чата: 3 сообщения в сутки — 4-е без вызова ИИ, со ссылкой.
    admin.setDailyLimits(3, 100);  // уже засчитано 3 сообщения этого чата
    const std::size_t calls = stage.ai.adminCalls();
    stage.say("И ещё вопрос");
    CHECK_EQ(stage.ai.adminCalls(), calls);
    CHECK(fake.last(Stage::kGuest).find(link) != std::string::npos);

    // Лимит владельца: вызовов ИИ уже 3 — четвёртый (другой чат) проходит, пятый — нет.
    admin.setDailyLimits(100, 4);
    stage.bot().handleUpdate(guestText(7202, "/start p_" + stage.slug, "Борис", ""));
    CHECK_EQ(stage.db().findTelegramGuestChat(7202)->guestName, std::string("Борис"));
    stage.say("Здравствуйте", 7202);
    CHECK_EQ(stage.ai.adminCalls(), calls + 1);
    stage.say("Сколько стоит?", 7202);
    CHECK_EQ(stage.ai.adminCalls(), calls + 1);
    CHECK(fake.last(7202).find(link) != std::string::npos);
}

// escalate → вопрос и уведомление telegram.question владельцу; telegram.questions;
// telegram.reply доходит гостю через бота «<имя>: …»; /stop — ответить нельзя.
TEST(telegram_admin_escalate_and_owner_reply) {
    using namespace f7_test;
    Stage stage("f7esc@example.com");
    auto& fake = *stage.fake;
    stage.ai.answer(R"({"reply":"Уточню.","action":"escalate","slots":[],"escalate_reason":"медицина"})");
    stage.say("Можно ли при беременности?");
    CHECK_EQ(fake.last(Stage::kGuest), std::string(selfvoy::TelegramAdmin::kEscalated));
    CHECK_EQ(booking_test::countNotifications(stage.fixture, stage.owner, "telegram.question"), 1);
    long long questionId = 0;
    for (const auto& item : stage.db().listNotifications(stage.ownerId, false, 50)) {
        if (item.kind != "telegram.question") continue;
        questionId = item.payload.getInt("question_id");
        CHECK_EQ(item.payload.getInt("page_id"), stage.pageId);
        CHECK_EQ(item.payload.getString("guest_name"), std::string("Анна Смирнова"));
        CHECK_EQ(item.payload.getString("telegram_username"), std::string("anna_tg"));
        CHECK_EQ(item.payload.getString("text"), std::string("Можно ли при беременности?"));
    }
    CHECK(questionId > 0);

    Json open = Json::object();
    open.set("status", Json("open"));
    const Json listed = integration::payloadOf(stage.fixture.call(stage.owner, "telegram.questions", open));
    CHECK_EQ(listed.get("questions").size(), std::size_t(1));
    const Json first = listed.get("questions").at(0);
    CHECK_EQ(first.getInt("id"), questionId);
    CHECK_EQ(first.getString("page_title"), std::string("Массаж"));
    CHECK_EQ(first.getString("status"), std::string("open"));
    CHECK(first.getBool("can_reply"));
    Json badStatus = Json::object();
    badStatus.set("status", Json("closed"));
    CHECK_EQ(stage.fixture.call(stage.owner, "telegram.questions", badStatus).getString("code"),
             std::string("bad_request"));

    // Ответ: пустой — bad_request, чужой владелец — not_found, свой — гостю в Telegram.
    Json reply = Json::object();
    reply.set("question_id", Json(questionId));
    reply.set("text", Json("   "));
    CHECK_EQ(stage.fixture.call(stage.owner, "telegram.reply", reply).getString("code"), std::string("bad_request"));
    auto other = stage.sockets.open(stage.fixture, "f7-other");
    stage.fixture.registerUser(other, "f7other@example.com", "Чужой");
    reply.set("text", Json("Да, можно."));
    CHECK_EQ(stage.fixture.call(other, "telegram.reply", reply).getString("code"), std::string("not_found"));
    std::string tooLong;
    for (int i = 0; i < 1001; ++i) tooLong += "a";
    Json longReply = reply;
    longReply.set("text", Json(tooLong));
    CHECK_EQ(stage.fixture.call(stage.owner, "telegram.reply", longReply).getString("code"), std::string("bad_request"));
    const Json answered = stage.fixture.call(stage.owner, "telegram.reply", reply);
    CHECK_EQ(answered.getString("type"), std::string("ok"));
    CHECK_EQ(integration::payloadOf(answered).get("question").getString("status"), std::string("answered"));
    stage.bot().flush();
    CHECK_EQ(fake.last(Stage::kGuest), std::string("Ольга: Да, можно."));
    Json done = Json::object();
    done.set("status", Json("answered"));
    const Json answeredList =
        integration::payloadOf(stage.fixture.call(stage.owner, "telegram.questions", done)).get("questions");
    CHECK(answeredList.size() == 1 && answeredList.at(0).getString("answer") == "Да, можно." &&
          !answeredList.at(0).getString("answered_at").empty());
    // Ответ специалиста попадает в историю чата (контекст ИИ).
    const auto chat = stage.db().findTelegramGuestChat(Stage::kGuest);
    CHECK(chat && chat->history.size() > 0 &&
          chat->history.at(chat->history.size() - 1).getString("text") == "Ольга: Да, можно.");

    // Не больше 5 вопросов специалисту в сутки на чат.
    for (int i = 0; i < 5; ++i) stage.say("Вопрос " + std::to_string(i));
    CHECK(fake.last(Stage::kGuest).find("уже передал") != std::string::npos);
    CHECK_EQ(booking_test::countNotifications(stage.fixture, stage.owner, "telegram.question"), 5);

    // /stop — гость отключил бота: ответить нельзя, can_reply = false.
    stage.say("/stop");
    CHECK_EQ(stage.fixture.call(stage.owner, "telegram.reply", reply).getString("code"), std::string("conflict"));
    const Json closed = integration::payloadOf(stage.fixture.call(stage.owner, "telegram.questions")).get("questions");
    CHECK(closed.size() == 5 && !closed.at(0).getBool("can_reply", true));
}

#ifdef SELFVOY_WITH_LIBPQ
// PostgreSQL: ai_admin страницы, чат гостя (upsert, история, /stop), вопросы (SELFVOY_TEST_PG_URL).
TEST(pg_telegram_admin_methods) {
    const char* url = std::getenv("SELFVOY_TEST_PG_URL");
    if (url == nullptr || *url == '\0') {
        std::cout << "  (пропущен: SELFVOY_TEST_PG_URL не задан)\n";
        return;
    }
    auto db = selfvoy::makePostgresDatabase(url);
    std::string error;
    CHECK(db && db->connect(error));
    if (!db) return;
    const std::string suffix = selfvoy::crypto::randomHex(4);
    long long owner = 0;
    CHECK(db->createUser("pg-f7-" + suffix + "@example.com", "argon2id$x", "Ольга", owner).ok);
    selfvoy::BookingPageRecord page;
    page.ownerId = owner;
    page.slug = "f7" + suffix + "zz";
    page.title = "Массаж";
    page.aiAdmin = true;
    long long pageId = 0;
    CHECK(db->createBookingPage(page, pageId).ok);
    CHECK(db->findBookingPage(pageId)->aiAdmin);
    auto stored = *db->findBookingPage(pageId);
    stored.aiAdmin = false;
    CHECK(db->updateBookingPage(stored).ok);
    CHECK(!db->findBookingPage(pageId)->aiAdmin);
    CHECK(!db->findBookingPageBySlug(page.slug)->aiAdmin);

    const long long chat = 910000000000LL + (std::time(nullptr) % 100000);
    selfvoy::TelegramGuestChatRecord record;
    record.chatId = chat;
    record.pageId = pageId;
    record.ownerId = owner;
    record.guestName = "Анна";
    record.username = "anna";
    CHECK(db->saveTelegramGuestChat(record).ok);
    Json item = Json::object();
    item.set("role", Json("guest"));
    item.set("text", Json("Привет"));
    record.history.push(item);
    record.guestName = "Анна С.";
    CHECK(db->saveTelegramGuestChat(record).ok);  // upsert
    const auto found = db->findTelegramGuestChat(chat);
    CHECK(found && found->guestName == "Анна С." && found->history.size() == 1 && found->pageId == pageId &&
          !found->createdAt.empty());

    selfvoy::TelegramQuestionRecord question;
    question.ownerId = owner;
    question.pageId = pageId;
    question.chatId = chat;
    question.guestName = "Анна";
    question.username = "anna";
    question.text = "Можно ли?";
    long long qid = 0;
    CHECK(db->createTelegramQuestion(question, qid).ok && qid > 0);
    CHECK_EQ(db->listTelegramQuestions(owner, "open", 10).size(), std::size_t(1));
    CHECK_EQ(db->listTelegramQuestions(owner, "answered", 10).size(), std::size_t(0));
    CHECK(db->answerTelegramQuestion(qid, "Да"));
    CHECK(!db->answerTelegramQuestion(qid + 100000, "Да"));
    const auto answered = db->findTelegramQuestion(qid);
    CHECK(answered && answered->status == "answered" && answered->answer == "Да" && !answered->answeredAt.empty());
    CHECK_EQ(db->listTelegramQuestions(owner, "", 10).size(), std::size_t(1));

    CHECK(db->unlinkTelegramChat(chat).ok);
    CHECK(!db->findTelegramGuestChat(chat).has_value());
    CHECK(db->findTelegramQuestion(qid).has_value());  // вопросы владельца остаются
}
#endif

#include "../src/federation/address.h"
#include "../src/integrationmanager.h"

TEST(aura_federation_address_parsing) {
    auto addr = AgentAddress::parse("@alice:aura.org");
    CHECK_EQ(addr.username, "alice");
    CHECK_EQ(addr.domain, "aura.org");
    CHECK_EQ(addr.toString(), "@alice:aura.org");

    bool caught = false;
    try {
        AgentAddress::parse("invalid");
    } catch (...) {
        caught = true;
    }
    CHECK(caught);

    caught = false;
    try {
        AgentAddress::parse("@onlyuser");
    } catch (...) {
        caught = true;
    }
    CHECK(caught);
}

TEST(aura_caldav_integration_connection) {
    selfvoy::Config config;
    selfvoy::DatabaseManager dbm(selfvoy::makeEmbeddedDatabase(""));
    selfvoy::IntegrationManager im(dbm, config);

    long long userId = 0;
    CHECK(dbm.db().createUser("caldav_user@selfvoy.test", "argon2id$x", "CalDAV User", userId).ok);
    CHECK(userId > 0);

    const auto res = im.connectCalDav(userId, "apple_caldav", "https://caldav.icloud.com/user/calendars", "alice", "app-spec-pass");
    CHECK(res.ok);
    CHECK_EQ(res.payload.getString("provider"), "apple_caldav");

    const auto list = im.list(userId);
    CHECK(list.ok);
    const auto conns = list.payload.get("connections");
    CHECK(conns.isArray());
    CHECK_EQ(conns.size(), std::size_t(1));
    CHECK_EQ(conns.items()[0].getString("provider"), "apple_caldav");
}

TEST(oauth_auth_full_flow) {
    selfvoy::Config config;
    config.oauthDevMock = true;
    config.jwtSecret = "01234567890123456789012345678901";
    selfvoy::DatabaseManager dbm(selfvoy::makeEmbeddedDatabase(""));
    selfvoy::AuthManager am(dbm, config);
    selfvoy::OAuthAuthManager oam(dbm, am, config);

    // 1. Catalog
    const auto cat = oam.providerCatalog();
    CHECK(cat.isArray());
    CHECK(cat.size() >= 5);

    // 2. Begin Google
    const auto beginGoogle = oam.begin("google", "http://localhost:5173/#/oauth/callback");
    CHECK(beginGoogle.ok);
    const std::string state = beginGoogle.payload.getString("state");
    const std::string authUrl = beginGoogle.payload.getString("authorize_url");
    CHECK(!state.empty());
    CHECK(authUrl.find("accounts.google.com") != std::string::npos);
    CHECK(authUrl.find("prompt=select_account") != std::string::npos);

    // 3. Callback Google in mock mode
    const auto cbGoogle = oam.callback("google", "mock_code_123", state, "web", "dev-1", "127.0.0.1");
    CHECK(cbGoogle.ok);
    CHECK(cbGoogle.payload.contains("token"));
    CHECK(cbGoogle.payload.contains("refresh_token"));
    CHECK(cbGoogle.payload.contains("user"));
    const auto user = cbGoogle.payload.get("user");
    CHECK(user.getInt("id") > 0);
    CHECK_EQ(user.getString("email"), "user.google@example.com");

    // 4. Repeated callback with same state must fail (used)
    const auto cbReuse = oam.callback("google", "mock_code_123", state, "web", "dev-1", "127.0.0.1");
    CHECK(!cbReuse.ok);

    // 5. Begin & Callback Yandex
    const auto beginYandex = oam.begin("yandex", "http://localhost:5173/#/oauth/callback");
    CHECK(beginYandex.ok);
    const std::string stateY = beginYandex.payload.getString("state");
    const auto cbYandex = oam.callback("yandex", "mock_yandex_code", stateY, "web", "dev-1", "127.0.0.1");
    CHECK(cbYandex.ok);
    CHECK_EQ(cbYandex.payload.get("user").getString("email"), "user.yandex@example.com");

    // 6. Begin & Callback GitHub
    const auto beginGh = oam.begin("github", "http://localhost:5173/#/oauth/callback");
    CHECK(beginGh.ok);
    const std::string stateGh = beginGh.payload.getString("state");
    const auto cbGh = oam.callback("github", "mock_gh_code", stateGh, "web", "dev-1", "127.0.0.1");
    CHECK(cbGh.ok);
    CHECK_EQ(cbGh.payload.get("user").getString("email"), "user.github@example.com");
}



