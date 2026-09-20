// aura/connectionmanager.h — реестр активных сессий и доставка событий.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aura/json.h"
#include "aura/session.h"

namespace aura {

class ConnectionManager {
public:
    void add(std::shared_ptr<Session> session);
    void remove(const std::string& sessionId);

    std::shared_ptr<Session> find(const std::string& sessionId) const;
    std::vector<std::shared_ptr<Session>> forUser(long long userId) const;

    // Отправляет событие всем живым соединениям пользователя.
    std::size_t deliverToUser(long long userId, const Json& message);
    std::size_t broadcast(const Json& message);
    bool isOnline(long long userId) const;

    std::size_t count() const;
    Json stats() const;
    std::vector<Json> describe() const;
    void closeAll(std::uint16_t code, const std::string& reason);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace aura
