// selfvoy/session.h — одно подключённое соединение (WebSocket + состояние пользователя).
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "selfvoy/json.h"

namespace selfvoy {

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

    // Обычный HTTP-запрос (без Upgrade: websocket) на тот же порт — /healthz, /ics/<токен>.ics.
    // Обработчик получает метод, цель запроса и IP клиента (с учётом X-Forwarded-For
    // доверенного прокси) и возвращает полный ответ HTTP/1.1 или "" — маршрута нет
    // (тогда 400, как без обработчика). После ответа performHandshake возвращает false,
    // servedHttp() — true: соединение закрывается.
    using HttpHandler =
        std::function<std::string(const std::string& method, const std::string& target, const std::string& ip)>;
    void setHttpHandler(HttpHandler handler) { httpHandler_ = std::move(handler); }
    bool servedHttp() const { return servedHttp_; }

    // Основной цикл чтения фреймов; завершается при закрытии соединения.
    void readLoop(const MessageHandler& handler);

    void send(const Json& message);
    void sendText(const std::string& text);
    void close(std::uint16_t code, const std::string& reason);

    // --- состояние авторизации ---
    // Потокобезопасно: соединение может «выбить» другой поток (отзыв сессии,
    // смена пароля, блокировка пользователя), пока этот читает кадры.
    void authenticate(long long userId, std::string displayName, std::string email, std::string jwtId);
    void resetAuth();
    bool authenticated() const { return userId_.load() != 0; }
    // Соединение гостя (страница приглашения или записи без входа): ему положено
    // жить дольше SELFVOY_AUTH_TIMEOUT — человек читает страницу и выбирает время.
    // Отмечается только успешным гостевым вызовом; срок считается от последнего
    // такого вызова (мусорный запрос не продлевает анонимный сокет).
    void markGuest();
    static constexpr long long kGuestIdleSec = 10 * 60;
    bool guest() const { return lastGuestMs_.load() != 0; }
    long long userId() const { return userId_.load(); }
    std::string displayName() const;
    std::string email() const;
    std::string jwtId() const;

    // Когда сессию в последний раз сверяли с БД (отзыв, срок, блокировка).
    std::chrono::steady_clock::time_point lastAuthCheck() const;
    void markAuthChecked();

    const std::string& id() const { return sessionId_; }
    const std::string& remoteAddr() const { return remoteAddr_; }
    // IP без порта: ключ для лимитов (переподключение не обнуляет счётчики).
    const std::string& remoteIp() const { return remoteIp_; }
    // «ip:port» → ключ лимитов: IPv4 как есть, IPv6 — сеть /64.
    static std::string ipOf(const std::string& remoteAddr);
    // IP без порта → ключ лимитов (IPv6 → «a:b:c:d::/64», IPv4-mapped → IPv4).
    static std::string limitKey(const std::string& ip);
    const std::string& userAgent() const { return userAgent_; }
    bool alive() const { return alive_.load(); }
    std::string authorizationHeader() const { return authorization_; }

    Json describe() const;

private:
    bool sendRaw(const std::string& frame);
    // Адрес клиента из X-Forwarded-For, если соединение пришло от доверенного прокси.
    void applyForwardedFor(const std::string& forwardedFor);
    bool recvWithTimeout(std::string& buffer, int timeoutMs, bool& closed);

    const int socket_;
    const std::string sessionId_;
    std::string remoteAddr_;  // меняется только в рукопожатии (X-Forwarded-For доверенного прокси)
    std::string remoteIp_;
    const Config& config_;

    mutable std::mutex sendMutex_;
    std::atomic<bool> alive_{true};

    mutable std::mutex authMutex_;
    std::atomic<long long> userId_{0};
    std::string displayName_;
    std::string email_;
    std::string jwtId_;
    std::chrono::steady_clock::time_point lastAuthCheck_;
    std::chrono::steady_clock::time_point unauthSince_;  // с какого момента соединение без входа
    std::atomic<long long> lastGuestMs_{0};              // steady_clock, мс: последний гостевой вызов
    std::string userAgent_;
    std::string authorization_;
    std::chrono::steady_clock::time_point lastActivity_;
    HttpHandler httpHandler_;
    bool servedHttp_ = false;
};

}  // namespace selfvoy
