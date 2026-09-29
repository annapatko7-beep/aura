// aura/ws.h — RFC 6455: рукопожатие и фреймы WebSocket.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace aura::ws {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Handshake {
    bool valid = false;
    std::string path = "/";
    std::string key;
    std::string origin;
    std::string authorization;  // заголовок Authorization (JWT при входе без auth.login)
    std::string userAgent;
    std::string error;
};

// Разбирает HTTP-запрос апгрейда. Имена заголовков приводятся к нижнему регистру.
Handshake parseHandshake(const std::string& request);

// Ответ 101 Switching Protocols для конкретного Sec-WebSocket-Key.
std::string handshakeResponse(const std::string& key);
std::string acceptToken(const std::string& key);  // base64(sha1(key + GUID))

struct Frame {
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;
};

enum class DecodeResult { Complete, Incomplete, ProtocolError };

// Сервер → клиент (без маски).
std::string encodeFrame(Opcode opcode, const std::string& payload);
std::string closeFrame(std::uint16_t code, const std::string& reason);

// Разбор кадра. consumed = сколько байт изъято из буфера.
// requireMask=true — кадр от клиента (RFC 6455 §5.1 требует маску);
// false — кадр от сервера, маска там запрещена.
DecodeResult decodeFrame(const std::string& buffer,
                         std::size_t maxBytes,
                         std::size_t& consumed,
                         Frame& frame,
                         std::string& error,
                         bool requireMask = true);

}  // namespace aura::ws
