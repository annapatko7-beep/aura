// aura/ws.cpp
#include "aura/ws.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "aura/crypto.h"

namespace aura::ws {

namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

}  // namespace

Handshake parseHandshake(const std::string& request) {
    Handshake result;
    const std::size_t headerEnd = request.find("\r\n\r\n");
    const std::string head = headerEnd == std::string::npos ? request : request.substr(0, headerEnd);
    const std::vector<std::string> lines = splitLines(head);
    if (lines.empty()) {
        result.error = "пустой запрос";
        return result;
    }

    std::istringstream request_line(lines[0]);
    std::string method, target, version;
    request_line >> method >> target >> version;
    if (method != "GET") {
        result.error = "ожидался GET, получено " + method;
        return result;
    }
    result.path = target;

    std::map<std::string, std::string> headers;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        headers[toLower(trim(lines[i].substr(0, colon)))] = trim(lines[i].substr(colon + 1));
    }

    const auto upgrade = headers.find("upgrade");
    if (upgrade == headers.end() || toLower(upgrade->second).find("websocket") == std::string::npos) {
        result.error = "нет заголовка Upgrade: websocket";
        return result;
    }
    const auto key = headers.find("sec-websocket-key");
    if (key == headers.end()) {
        result.error = "нет Sec-WebSocket-Key";
        return result;
    }
    result.key = key->second;
    // Принимаем только версию 13 — иначе рукопожатие несовместимо (RFC 6455 §4.2.1).
    if (const auto it = headers.find("sec-websocket-version");
        it == headers.end() || it->second != "13") {
        result.error = "ожидался Sec-WebSocket-Version: 13";
        return result;
    }
    if (const auto it = headers.find("origin"); it != headers.end()) result.origin = it->second;
    if (const auto it = headers.find("authorization"); it != headers.end()) result.authorization = it->second;
    if (const auto it = headers.find("user-agent"); it != headers.end()) result.userAgent = it->second;
    result.valid = true;
    return result;
}

std::string acceptToken(const std::string& key) {
    return crypto::base64Encode(crypto::sha1(key + kGuid));
}

std::string handshakeResponse(const std::string& key) {
    std::ostringstream out;
    out << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << acceptToken(key) << "\r\n\r\n";
    return out.str();
}

std::string encodeFrame(Opcode opcode, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(opcode)));
    const std::size_t size = payload.size();
    if (size < 126) {
        out.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((size >> 8) & 0xFF));
        out.push_back(static_cast<char>(size & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((size >> (i * 8)) & 0xFF));
    }
    out += payload;
    return out;
}

std::string closeFrame(std::uint16_t code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason;
    return encodeFrame(Opcode::Close, payload);
}

DecodeResult decodeFrame(const std::string& buffer,
                         std::size_t maxBytes,
                         std::size_t& consumed,
                         Frame& frame,
                         std::string& error,
                         bool requireMask) {
    consumed = 0;
    if (buffer.size() < 2) return DecodeResult::Incomplete;

    const auto first = static_cast<unsigned char>(buffer[0]);
    const auto second = static_cast<unsigned char>(buffer[1]);
    // Зарезервированные биты без расширений должны быть нулевыми (RFC 6455 §5.2).
    if ((first & 0x70) != 0) {
        error = "зарезервированные биты RSV должны быть нулевыми";
        return DecodeResult::ProtocolError;
    }
    frame.fin = (first & 0x80) != 0;
    frame.opcode = static_cast<Opcode>(first & 0x0F);
    const bool masked = (second & 0x80) != 0;
    std::uint64_t length = second & 0x7F;
    std::size_t offset = 2;

    // Кадр от клиента обязан быть маскированным (RFC 6455 §5.1);
    // кадр от сервера, наоборот, маски не имеет.
    if (requireMask && !masked) {
        error = "кадр клиента без маски";
        return DecodeResult::ProtocolError;
    }
    // Управляющие кадры не фрагментируются и не длиннее 125 байт (RFC 6455 §5.5).
    const bool control = (first & 0x08) != 0;
    if (control && (!frame.fin || length > 125)) {
        error = "некорректный управляющий кадр";
        return DecodeResult::ProtocolError;
    }

    if (length == 126) {
        if (buffer.size() < offset + 2) return DecodeResult::Incomplete;
        length = (static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[offset])) << 8) |
                 static_cast<unsigned char>(buffer[offset + 1]);
        offset += 2;
    } else if (length == 127) {
        if (buffer.size() < offset + 8) return DecodeResult::Incomplete;
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<unsigned char>(buffer[offset + i]);
        }
        offset += 8;
    }

    if (length > maxBytes) {
        error = "фрейм больше допустимого размера";
        return DecodeResult::ProtocolError;
    }

    std::uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buffer.size() < offset + 4) return DecodeResult::Incomplete;
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(buffer[offset + i]);
        offset += 4;
    }

    if (buffer.size() < offset + length) return DecodeResult::Incomplete;

    frame.payload.assign(buffer, offset, static_cast<std::size_t>(length));
    if (masked) {
        for (std::size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] = static_cast<char>(static_cast<unsigned char>(frame.payload[i]) ^ mask[i % 4]);
        }
    }

    consumed = offset + static_cast<std::size_t>(length);
    return DecodeResult::Complete;
}

}  // namespace aura::ws
