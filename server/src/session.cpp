// selfvoy/session.cpp
#include "selfvoy/session.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <utility>

#include "selfvoy/config.h"
#include "selfvoy/log.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"
#include "selfvoy/ws.h"

namespace selfvoy {

Session::Session(int socket, std::string sessionId, std::string remoteAddr, const Config& config)
    : socket_(socket),
      sessionId_(std::move(sessionId)),
      remoteAddr_(std::move(remoteAddr)),
      remoteIp_(ipOf(remoteAddr_)),
      config_(config),
      lastAuthCheck_(std::chrono::steady_clock::now()),
      unauthSince_(std::chrono::steady_clock::now()),
      lastActivity_(std::chrono::steady_clock::now()) {}

std::string Session::ipOf(const std::string& remoteAddr) {
    // Listener формирует «ip:port» (IPv6 — без скобок). Отрезаем порт, если
    // после последнего ':' только цифры и слева остался адрес.
    const std::size_t colon = remoteAddr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= remoteAddr.size()) return remoteAddr;
    for (std::size_t i = colon + 1; i < remoteAddr.size(); ++i) {
        if (remoteAddr[i] < '0' || remoteAddr[i] > '9') return remoteAddr;
    }
    std::string ip = remoteAddr.substr(0, colon);
    if (ip.size() > 2 && ip.front() == '[' && ip.back() == ']') ip = ip.substr(1, ip.size() - 2);
    return limitKey(ip);
}

std::string Session::limitKey(const std::string& ip) {
    // IPv6: у абонента обычно целая сеть /64 — лимиты считаем по ней, иначе
    // смена адреса внутри своей сети обходила бы любой лимит «на IP».
    in6_addr address{};
    if (ip.find(':') == std::string::npos || ::inet_pton(AF_INET6, ip.c_str(), &address) != 1) return ip;
    const unsigned char* bytes = address.s6_addr;
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(bytes, kMapped, sizeof(kMapped)) == 0) {
        char text[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, bytes + 12, text, sizeof(text));
        return text;
    }
    char prefix[48] = {0};
    std::snprintf(prefix, sizeof(prefix), "%x:%x:%x:%x::/64", (bytes[0] << 8) | bytes[1], (bytes[2] << 8) | bytes[3],
                  (bytes[4] << 8) | bytes[5], (bytes[6] << 8) | bytes[7]);
    return prefix;
}

Session::~Session() {
    alive_.store(false);
    net::closeSocket(socket_);
}

void Session::applyForwardedFor(const std::string& forwardedFor) {
    // За доверенным прокси адрес клиента — правый адрес X-Forwarded-For, не
    // принадлежащий доверенным прокси (левые части клиент может подделать).
    if (!forwardedFor.empty() && !config_.trustedProxies.empty()) {
        const auto trusted = [this](const std::string& ip) {
            std::stringstream list(config_.trustedProxies);
            std::string item;
            while (std::getline(list, item, ',')) {
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                if (!item.empty() && item == ip) return true;
            }
            return false;
        };
        // Сравниваем «сырой» адрес соединения: remoteIp_ для IPv6 — уже сеть /64.
        std::string peer = remoteAddr_.substr(0, remoteAddr_.rfind(':'));
        if (peer.size() > 2 && peer.front() == '[' && peer.back() == ']') peer = peer.substr(1, peer.size() - 2);
        if (trusted(peer)) {
            std::vector<std::string> hops;
            std::stringstream chain(forwardedFor);
            std::string hop;
            while (std::getline(chain, hop, ',')) {
                hop.erase(0, hop.find_first_not_of(" \t"));
                hop.erase(hop.find_last_not_of(" \t") + 1);
                hops.push_back(hop);
            }
            for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
                const bool plausible = !it->empty() && it->size() <= 45 &&
                                       std::all_of(it->begin(), it->end(), [](unsigned char ch) {
                                           return std::isxdigit(ch) != 0 || ch == '.' || ch == ':';
                                       });
                if (!plausible) break;
                if (trusted(*it)) continue;
                remoteIp_ = limitKey(*it);
                remoteAddr_ = (it->find(':') != std::string::npos ? "[" + *it + "]" : *it) + ":0";
                break;
            }
        }
    }
}

bool Session::performHandshake(std::string& error) {
    std::string request;
    char buffer[2048];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline) {
            error = "таймаут рукопожатия";
            return false;
        }
        pollfd descriptor{};
        descriptor.fd = socket_;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, 1000) <= 0) continue;
        const ssize_t received = ::recv(socket_, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            error = "соединение закрыто до рукопожатия";
            return false;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        if (request.size() > 64 * 1024) {
            error = "слишком большой HTTP-заголовок";
            return false;
        }
    }

    const ws::Handshake handshake = ws::parseHandshake(request);
    if (!handshake.valid && !handshake.upgrade && !handshake.method.empty() && httpHandler_) {
        applyForwardedFor(handshake.forwardedFor);
        const std::string response = httpHandler_(handshake.method, handshake.path, remoteIp_);
        if (!response.empty()) {
            net::sendAll(socket_, response, 5000);
            servedHttp_ = true;
            error = "HTTP " + handshake.method + " (не WebSocket)";
            return false;
        }
    }
    if (!handshake.valid) {
        const std::string response =
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        net::sendAll(socket_, response, 2000);
        error = handshake.error;
        return false;
    }

    userAgent_ = handshake.userAgent;
    authorization_ = handshake.authorization;
    applyForwardedFor(handshake.forwardedFor);
    if (!net::sendAll(socket_, ws::handshakeResponse(handshake.key), 2000)) {
        error = "не удалось отправить 101";
        return false;
    }
    SELFVOY_LOG(log::Level::Debug, "session") << sessionId_ << " рукопожатие выполнено, path=" << handshake.path;
    return true;
}

bool Session::recvWithTimeout(std::string& buffer, int timeoutMs, bool& closed) {
    pollfd descriptor{};
    descriptor.fd = socket_;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, timeoutMs);
    if (ready == 0) return true;  // просто нет данных
    if (ready < 0) {
        if (errno == EINTR) return true;
        closed = true;
        return false;
    }
    char chunk[8192];
    const ssize_t received = ::recv(socket_, chunk, sizeof(chunk), 0);
    if (received == 0) {
        closed = true;
        return false;
    }
    if (received < 0) {
        if (errno == EINTR || errno == EAGAIN) return true;
        closed = true;
        return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
    return true;
}

void Session::readLoop(const MessageHandler& handler) {
    std::string buffer;
    std::string fragments;          // склейка continuation-фреймов
    auto lastPing = std::chrono::steady_clock::now();

    // Склеенное сообщение не может быть больше лимита одного кадра: иначе
    // клиент шлёт бесконечные фрагменты без FIN и исчерпывает память сервера.
    // До входа лимит кадра намного меньше (SELFVOY_MAX_PREAUTH_FRAME): анонимный
    // клиент не должен заставлять сервер разбирать мегабайтные сообщения.
    auto frameLimit = [this]() {
        return authenticated() ? config_.maxFrameBytes
                               : std::min(config_.maxFrameBytes, config_.maxPreAuthFrameBytes);
    };
    auto fragmentsTooBig = [this, &fragments, &frameLimit]() {
        const std::size_t limit = frameLimit();
        if (fragments.size() <= limit) return false;
        SELFVOY_LOG(log::Level::Warn, "session")
            << sessionId_ << " фрагментированное сообщение больше " << limit << " байт";
        close(1009, "message too big");
        return true;
    };

    while (alive_.load()) {
        bool closed = false;
        if (!recvWithTimeout(buffer, 1000, closed)) {
            if (closed) {
                SELFVOY_LOG(log::Level::Debug, "session") << sessionId_ << " соединение закрыто клиентом";
            }
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        // Неавторизованное соединение живёт не дольше SELFVOY_AUTH_TIMEOUT: иначе
        // анонимные сокеты, отвечающие на ping, занимают потоки бесконечно.
        if (config_.authTimeoutSec > 0 && !authenticated()) {
            std::chrono::steady_clock::time_point since;
            {
                std::lock_guard<std::mutex> lock(authMutex_);
                since = unauthSince_;
            }
            // Гостевые страницы (invite.*, booking.*) — пока гость пользуется
            // страницей: kGuestIdleSec с последнего успешного гостевого вызова.
            bool overdue = std::chrono::duration_cast<std::chrono::seconds>(now - since).count() >=
                           config_.authTimeoutSec;
            if (overdue && guest()) {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                overdue = nowMs - lastGuestMs_.load() >= kGuestIdleSec * 1000;
            }
            if (overdue) {
                SELFVOY_LOG(log::Level::Info, "session") << sessionId_ << " не авторизовался вовремя, закрываю";
                close(4408, "authentication timeout");
                break;
            }
        }
        if (config_.pingIntervalSec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - lastPing).count() >=
                config_.pingIntervalSec) {
            lastPing = now;
            sendRaw(ws::encodeFrame(ws::Opcode::Ping, "selfvoy"));
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity_).count() >
                config_.socketTimeoutSec) {
                SELFVOY_LOG(log::Level::Info, "session") << sessionId_ << " неактивен, закрываю";
                close(1001, "idle timeout");
                break;
            }
        }

        while (true) {
            std::size_t consumed = 0;
            ws::Frame frame;
            std::string error;
            const ws::DecodeResult result =
                ws::decodeFrame(buffer, frameLimit(), consumed, frame, error);
            if (result == ws::DecodeResult::Incomplete) break;
            if (result == ws::DecodeResult::ProtocolError) {
                SELFVOY_LOG(log::Level::Warn, "session") << sessionId_ << " " << error;
                close(1002, error);
                return;
            }
            buffer.erase(0, consumed);
            lastActivity_ = std::chrono::steady_clock::now();

            switch (frame.opcode) {
                case ws::Opcode::Close:
                    sendRaw(ws::closeFrame(1000, "bye"));
                    alive_.store(false);
                    return;
                case ws::Opcode::Ping:
                    sendRaw(ws::encodeFrame(ws::Opcode::Pong, frame.payload));
                    break;
                case ws::Opcode::Pong:
                    break;
                case ws::Opcode::Text:
                case ws::Opcode::Binary: {
                    if (frame.fin) {
                        const std::string text = fragments + frame.payload;
                        fragments.clear();
                        std::string parseError;
                        const Json message = Json::parse(text, &parseError);
                        if (message.isNull()) {
                            send(protocol::error("", protocol::code::kBadRequest, "некорректный JSON: " + parseError));
                            break;
                        }
                        if (handler) handler(shared_from_this(), message);
                    } else {
                        fragments += frame.payload;
                        if (fragmentsTooBig()) return;
                    }
                    break;
                }
                case ws::Opcode::Continuation:
                    fragments += frame.payload;
                    if (fragmentsTooBig()) return;
                    if (frame.fin) {
                        const std::string text = fragments;
                        fragments.clear();
                        std::string parseError;
                        const Json message = Json::parse(text, &parseError);
                        if (message.isNull()) {
                            send(protocol::error("", protocol::code::kBadRequest, "некорректный JSON: " + parseError));
                            break;
                        }
                        if (handler) handler(shared_from_this(), message);
                    }
                    break;
            }
        }
    }
    alive_.store(false);
}

void Session::send(const Json& message) { sendText(message.dump()); }

void Session::sendText(const std::string& text) {
    if (!alive_.load()) return;
    sendRaw(ws::encodeFrame(ws::Opcode::Text, text));
}

bool Session::sendRaw(const std::string& frame) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!alive_.load()) return false;
    if (!net::sendAll(socket_, frame, 5000)) {
        SELFVOY_LOG(log::Level::Debug, "session") << sessionId_ << " не удалось отправить кадр";
        alive_.store(false);
        return false;
    }
    return true;
}

void Session::markGuest() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    lastGuestMs_.store(std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
}

void Session::close(std::uint16_t code, const std::string& reason) {
    if (!alive_.exchange(false)) return;
    // Кадр уходит до погашения alive_: sendRaw не пишет в мёртвую сессию.
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        net::sendAll(socket_, ws::closeFrame(code, reason), 2000);
    }
    ::shutdown(socket_, SHUT_RDWR);
}

void Session::authenticate(long long userId, std::string displayName, std::string email, std::string jwtId) {
    std::lock_guard<std::mutex> lock(authMutex_);
    displayName_ = std::move(displayName);
    email_ = std::move(email);
    jwtId_ = std::move(jwtId);
    lastAuthCheck_ = std::chrono::steady_clock::now();
    userId_.store(userId);
}

void Session::resetAuth() {
    std::lock_guard<std::mutex> lock(authMutex_);
    userId_.store(0);
    displayName_.clear();
    email_.clear();
    jwtId_.clear();
    unauthSince_ = std::chrono::steady_clock::now();
}

std::string Session::displayName() const {
    std::lock_guard<std::mutex> lock(authMutex_);
    return displayName_;
}

std::string Session::email() const {
    std::lock_guard<std::mutex> lock(authMutex_);
    return email_;
}

std::string Session::jwtId() const {
    std::lock_guard<std::mutex> lock(authMutex_);
    return jwtId_;
}

std::chrono::steady_clock::time_point Session::lastAuthCheck() const {
    std::lock_guard<std::mutex> lock(authMutex_);
    return lastAuthCheck_;
}

void Session::markAuthChecked() {
    std::lock_guard<std::mutex> lock(authMutex_);
    lastAuthCheck_ = std::chrono::steady_clock::now();
}

Json Session::describe() const {
    Json json = Json::object();
    json.set("session_id", Json(sessionId_));
    json.set("user_id", Json(userId()));
    json.set("display_name", Json(displayName()));
    json.set("email", Json(email()));
    json.set("remote_addr", Json(remoteAddr_));
    json.set("alive", Json(alive_.load()));
    return json;
}

}  // namespace selfvoy
