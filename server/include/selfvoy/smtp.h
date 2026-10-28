// selfvoy/smtp.h — отправка письма по SMTP (RFC 5321) с TLS.
//
// Режимы шифрования (SELFVOY_SMTP_TLS):
//   starttls — порт 587: открытое соединение, затем STARTTLS (по умолчанию);
//   tls      — порт 465: TLS сразу (SMTPS);
//   none     — без шифрования: только локальный ретранслятор (postfix на том же хосте).
// Сертификат сервера проверяется (системные CA + SELFVOY_SMTP_CA_FILE), имя хоста сверяется.
// Без OpenSSL сервер собирается, но режимы с TLS возвращают ошибку.
#pragma once

#include <string>

namespace selfvoy::smtp {

struct Settings {
    std::string host;
    int port = 587;
    std::string user;
    std::string password;
    std::string from;          // «Selfvoy <no-reply@example.com>» или просто адрес
    std::string tls = "starttls";
    std::string caFile;        // дополнительный корневой сертификат (свой CA, тесты)
    int timeoutMs = 15000;
};

struct Message {
    std::string to;
    std::string subject;
    std::string text;          // text/plain
    std::string html;          // text/html (пусто — только текст)
};

// true, если в строке нет CR/LF (защита от подстановки заголовков и команд SMTP).
bool safeHeaderValue(const std::string& value);

// Адрес из «Имя <addr@host>» или сам addr@host.
std::string addressOf(const std::string& mailbox);

// Готовое письмо (заголовки + MIME, строки через CRLF, без завершающей точки).
// date и messageId передаются явно — для детерминированных тестов.
std::string buildMessage(const Settings& settings, const Message& message, const std::string& date,
                         const std::string& messageId);

// Отправить письмо. false + error — с понятной причиной (без пароля в тексте).
bool send(const Settings& settings, const Message& message, std::string& error);

// Собран ли сервер с поддержкой TLS.
bool tlsAvailable();

}  // namespace selfvoy::smtp
