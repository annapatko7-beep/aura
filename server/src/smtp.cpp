// selfvoy/smtp.cpp — SMTP-клиент: EHLO, STARTTLS/SMTPS, AUTH PLAIN/LOGIN, MIME-письмо.
#include "selfvoy/smtp.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <initializer_list>

#include "selfvoy/crypto.h"
#include "selfvoy/net.h"
#include "selfvoy/tls.h"

namespace selfvoy::smtp {

namespace {

constexpr std::size_t kMaxReply = 64 * 1024;

std::string lower(std::string value) {
    for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool isAscii(const std::string& value) {
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}

// base64 с переносом строк по 76 символов (RFC 2045).
std::string wrappedBase64(const std::string& data) {
    const std::string encoded = crypto::base64Encode(data);
    std::string out;
    for (std::size_t i = 0; i < encoded.size(); i += 76) out += encoded.substr(i, 76) + "\r\n";
    return out;
}

// RFC 2047: не-ASCII заголовок → =?UTF-8?B?…?= кусками по 45 байт без разрыва символов UTF-8.
std::string encodeHeader(const std::string& value) {
    if (isAscii(value)) return value;
    std::string out;
    std::size_t pos = 0;
    while (pos < value.size()) {
        std::size_t end = std::min(pos + 45, value.size());
        while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + crypto::base64Encode(value.substr(pos, end - pos)) + "?=";
        pos = end;
    }
    return out;
}

std::string domainOf(const std::string& address) {
    const auto at = address.rfind('@');
    return at == std::string::npos ? std::string("localhost") : address.substr(at + 1);
}

// RFC 5322 Date в UTC без зависимости от локали процесса.
std::string rfc5322Now() {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d +0000", kDays[tm.tm_wday], tm.tm_mday,
                  kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

bool isLoopback(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

// Соединение: обычный сокет или TLS поверх него (общая обвязка selfvoy/tls.h).
class Connection {
public:
    ~Connection() {
        tls_.shutdown();  // close_notify — до закрытия сокета
        if (fd_ >= 0) net::closeSocket(fd_);
    }

    bool open(const Settings& settings, std::string& error) {
        fd_ = net::connectTcp(settings.host, settings.port, settings.timeoutMs, error);
        if (fd_ < 0) return false;
        timeval tv{};
        tv.tv_sec = settings.timeoutMs / 1000;
        tv.tv_usec = (settings.timeoutMs % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        return true;
    }

    bool secure() const { return tls_.active(); }

    bool startTls(const Settings& settings, std::string& error) {
        if (!tls::available()) {
            error = "сервер собран без OpenSSL — TLS для SMTP недоступен (SELFVOY_SMTP_TLS=none только для локального ретранслятора)";
            return false;
        }
        return tls_.connect(fd_, settings.host, settings.caFile, "SELFVOY_SMTP_CA_FILE", error);
    }

    bool write(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const long n = tls_.active() ? tls_.write(data.data() + sent, data.size() - sent)
                                         : ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Ответ сервера: код и текст (многострочный «250-…» до «250 …»).
    bool reply(int& code, std::string& text) {
        text.clear();
        for (;;) {
            std::string line;
            if (!readLine(line)) return false;
            if (line.size() < 3 || !std::isdigit(static_cast<unsigned char>(line[0]))) return false;
            code = std::stoi(line.substr(0, 3));
            text += line.size() > 4 ? line.substr(4) : std::string();
            text += "\n";
            if (text.size() > kMaxReply) return false;
            if (line.size() < 4 || line[3] != '-') return true;
        }
    }

private:
    bool readLine(std::string& line) {
        for (;;) {
            const auto eol = buffer_.find("\r\n");
            if (eol != std::string::npos) {
                line = buffer_.substr(0, eol);
                buffer_.erase(0, eol + 2);
                return true;
            }
            if (buffer_.size() > kMaxReply) return false;
            char chunk[4096];
            const long n = tls_.active() ? tls_.read(chunk, sizeof chunk) : ::recv(fd_, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
    }

    tls::Client tls_;
    int fd_ = -1;
    std::string buffer_;
};

// Команда → ожидаемые коды ответа.
bool command(Connection& conn, const std::string& line, std::initializer_list<int> expected, std::string& text,
             std::string& error, const std::string& shown = "") {
    if (!line.empty() && !conn.write(line + "\r\n")) {
        error = "SMTP: соединение оборвалось";
        return false;
    }
    int code = 0;
    if (!conn.reply(code, text)) {
        error = "SMTP: сервер не ответил";
        return false;
    }
    for (int ok : expected) {
        if (code == ok) return true;
    }
    std::string first = text.substr(0, text.find('\n'));
    error = "SMTP " + (shown.empty() ? line.substr(0, line.find(' ')) : shown) + ": " + std::to_string(code) +
            (first.empty() ? "" : " " + first);
    return false;
}

bool hasCapability(const std::string& ehlo, const std::string& name) {
    const std::string caps = "\n" + lower(ehlo);
    return caps.find("\n" + lower(name)) != std::string::npos;
}

std::string authMechanisms(const std::string& ehlo) {
    const std::string caps = lower(ehlo);
    const auto pos = caps.find("auth");
    if (pos == std::string::npos) return "";
    return caps.substr(pos, caps.find('\n', pos) - pos);
}

}  // namespace

bool tlsAvailable() { return tls::available(); }

bool safeHeaderValue(const std::string& value) {
    return value.find('\r') == std::string::npos && value.find('\n') == std::string::npos;
}

std::string addressOf(const std::string& mailbox) {
    const auto open = mailbox.rfind('<');
    const auto close = mailbox.rfind('>');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        return mailbox.substr(open + 1, close - open - 1);
    }
    std::string trimmed = mailbox;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(0, 1);
    return trimmed;
}

std::string buildMessage(const Settings& settings, const Message& message, const std::string& date,
                         const std::string& messageId) {
    // From: «Имя <addr>» — имя кодируется отдельно, адрес остаётся как есть.
    std::string from = settings.from;
    const auto open = from.rfind('<');
    if (open != std::string::npos && open > 0) {
        std::string name = from.substr(0, open);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        from = encodeHeader(name) + " " + from.substr(open);
    }
    const std::string boundary = "=_selfvoy_" + crypto::toHex(crypto::sha256(messageId)).substr(0, 24);

    std::string out;
    out += "From: " + from + "\r\n";
    out += "To: <" + message.to + ">\r\n";
    out += "Subject: " + encodeHeader(message.subject) + "\r\n";
    out += "Date: " + date + "\r\n";
    out += "Message-ID: " + messageId + "\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Auto-Submitted: auto-generated\r\n";
    if (message.html.empty()) {
        out += "Content-Type: text/plain; charset=UTF-8\r\n";
        out += "Content-Transfer-Encoding: base64\r\n\r\n";
        out += wrappedBase64(message.text);
        return out;
    }
    out += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
    out += "--" + boundary + "\r\n";
    out += "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    out += wrappedBase64(message.text);
    out += "--" + boundary + "\r\n";
    out += "Content-Type: text/html; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    out += wrappedBase64(message.html);
    out += "--" + boundary + "--\r\n";
    return out;
}

bool send(const Settings& settings, const Message& message, std::string& error) {
    const std::string fromAddress = addressOf(settings.from);
    if (settings.host.empty() || fromAddress.find('@') == std::string::npos) {
        error = "SMTP не настроен: нужны SELFVOY_SMTP_HOST и SELFVOY_SMTP_FROM";
        return false;
    }
    if (!safeHeaderValue(message.to) || !safeHeaderValue(message.subject) || !safeHeaderValue(settings.from) ||
        message.to.find('@') == std::string::npos || message.to.find_first_of("<> ") != std::string::npos) {
        error = "SMTP: недопустимый адрес или тема";
        return false;
    }
    const std::string mode = lower(settings.tls);
    if (mode != "starttls" && mode != "tls" && mode != "none") {
        error = "SELFVOY_SMTP_TLS: starttls, tls или none";
        return false;
    }

    Connection conn;
    if (!conn.open(settings, error)) return false;
    if (mode == "tls" && !conn.startTls(settings, error)) return false;

    std::string text;
    if (!command(conn, "", {220}, text, error, "приветствие")) return false;
    const std::string helo = "EHLO " + domainOf(fromAddress);
    if (!command(conn, helo, {250}, text, error)) return false;

    if (mode == "starttls") {
        if (!hasCapability(text, "STARTTLS")) {
            error = "SMTP-сервер не поддерживает STARTTLS (укажите SELFVOY_SMTP_TLS=tls для порта 465)";
            return false;
        }
        if (!command(conn, "STARTTLS", {220}, text, error)) return false;
        if (!conn.startTls(settings, error)) return false;
        if (!command(conn, helo, {250}, text, error)) return false;
    }

    if (!settings.user.empty()) {
        // Пароль по открытому каналу — только к ретранслятору на этой же машине.
        if (!conn.secure() && !isLoopback(settings.host)) {
            error = "SMTP: авторизация без TLS запрещена (SELFVOY_SMTP_TLS=none допустим только для localhost)";
            return false;
        }
        const std::string mechanisms = authMechanisms(text);
        if (mechanisms.find("plain") != std::string::npos) {
            const std::string token = crypto::base64Encode(std::string("\0", 1) + settings.user +
                                                           std::string("\0", 1) + settings.password);
            if (!command(conn, "AUTH PLAIN " + token, {235}, text, error, "AUTH")) return false;
        } else if (mechanisms.find("login") != std::string::npos) {
            if (!command(conn, "AUTH LOGIN", {334}, text, error)) return false;
            if (!command(conn, crypto::base64Encode(settings.user), {334}, text, error, "AUTH")) return false;
            if (!command(conn, crypto::base64Encode(settings.password), {235}, text, error, "AUTH")) return false;
        } else {
            error = "SMTP-сервер не предлагает AUTH PLAIN/LOGIN";
            return false;
        }
    }

    if (!command(conn, "MAIL FROM:<" + fromAddress + ">", {250}, text, error, "MAIL FROM")) return false;
    if (!command(conn, "RCPT TO:<" + message.to + ">", {250, 251}, text, error, "RCPT TO")) return false;
    if (!command(conn, "DATA", {354}, text, error)) return false;

    const std::string messageId = "<" + crypto::randomHex(16) + "@" + domainOf(fromAddress) + ">";
    std::string body = buildMessage(settings, message, rfc5322Now(), messageId);
    // Dot-stuffing: строка, начинающаяся с «.», удваивает точку.
    std::string stuffed;
    std::size_t start = 0;
    while (start < body.size()) {
        const auto eol = body.find("\r\n", start);
        const std::string line = body.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
        stuffed += (!line.empty() && line[0] == '.' ? "." : "") + line + "\r\n";
        if (eol == std::string::npos) break;
        start = eol + 2;
    }
    if (!conn.write(stuffed + ".\r\n")) {
        error = "SMTP: соединение оборвалось при передаче письма";
        return false;
    }
    if (!command(conn, "", {250}, text, error, "DATA")) return false;
    command(conn, "QUIT", {221}, text, error);
    error.clear();
    return true;
}

}  // namespace selfvoy::smtp
