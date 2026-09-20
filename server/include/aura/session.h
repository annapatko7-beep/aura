// aura/session.h — одно подключённое соединение (WebSocket + состояние пользователя).
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "aura/json.h"

namespace aura {

struct Config;

class Session : public std::enable_shared_from_this<Session> {
public:
    using MessageHandler = std::function<void(std::shared_ptr<Session>, const Json&)>;

    Session(int socket, std::string sessionId, std::string remoteAddr, const Config& config);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Читает HTTP-апгрейд и отвечает 101. Возвращает false при ошибке рукопожатия.
    bool performHandshake(std::string& error);

    // Основной цикл чтения фреймов; завершается при закрытии соединения.
    void readLoop(const MessageHandler& handler);

    void send(const Json& message);
    void sendText(const std::string& text);
    void close(std::uint16_t code, const std::string& reason);

    // --- состояние авторизации ---
    void authenticate(long long userId, std::string displayName, std::string email, std::string jwtId);
    void resetAuth();
    bool authenticated() const { return userId_ != 0; }
    long long userId() const { return userId_; }
    const std::string& displayName() const { return displayName_; }
    const std::string& email() const { return email_; }
    const std::string& jwtId() const { return jwtId_; }

    const std::string& id() const { return sessionId_; }
    const std::string& remoteAddr() const { return remoteAddr_; }
    const std::string& userAgent() const { return userAgent_; }
    bool alive() const { return alive_.load(); }
    std::string authorizationHeader() const { return authorization_; }

    Json describe() const;

private:
    bool sendRaw(const std::string& frame);
    bool recvWithTimeout(std::string& buffer, int timeoutMs, bool& closed);

    const int socket_;
    const std::string sessionId_;
    const std::string remoteAddr_;
    const Config& config_;

    mutable std::mutex sendMutex_;
    std::atomic<bool> alive_{true};

    long long userId_ = 0;
    std::string displayName_;
    std::string email_;
    std::string jwtId_;
    std::string userAgent_;
    std::string authorization_;
    std::chrono::steady_clock::time_point lastActivity_;
};

}  // namespace aura
