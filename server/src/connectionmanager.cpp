// selfvoy/connectionmanager.cpp
#include "selfvoy/connectionmanager.h"

#include <algorithm>

#include "selfvoy/log.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

void ConnectionManager::add(std::shared_ptr<Session> session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->id()] = std::move(session);
}

void ConnectionManager::remove(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

std::shared_ptr<Session> ConnectionManager::find(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Session>> ConnectionManager::forUser(long long userId) const {
    std::vector<std::shared_ptr<Session>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : sessions_) {
        if (entry.second->userId() == userId && entry.second->alive()) result.push_back(entry.second);
    }
    return result;
}

std::size_t ConnectionManager::deliverToUser(long long userId, const Json& message) {
    const auto targets = forUser(userId);
    for (const auto& session : targets) session->send(message);
    return targets.size();
}

std::size_t ConnectionManager::broadcast(const Json& message) {
    std::vector<std::shared_ptr<Session>> alive;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alive.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            if (entry.second->alive()) alive.push_back(entry.second);
        }
    }
    for (const auto& session : alive) session->send(message);
    return alive.size();
}

bool ConnectionManager::isOnline(long long userId) const { return !forUser(userId).empty(); }

std::size_t ConnectionManager::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

Json ConnectionManager::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t authenticated = 0;
    for (const auto& entry : sessions_) {
        if (entry.second->authenticated()) ++authenticated;
    }
    Json json = Json::object();
    json.set("connections", Json(static_cast<long long>(sessions_.size())));
    json.set("authenticated", Json(static_cast<long long>(authenticated)));
    return json;
}

std::vector<Json> ConnectionManager::describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Json> result;
    result.reserve(sessions_.size());
    for (const auto& entry : sessions_) result.push_back(entry.second->describe());
    return result;
}

void ConnectionManager::closeAll(std::uint16_t code, const std::string& reason) {
    std::vector<std::shared_ptr<Session>> alive;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : sessions_) alive.push_back(entry.second);
    }
    for (const auto& session : alive) session->close(code, reason);
    SELFVOY_LOG(log::Level::Info, "connections") << "закрыто соединений: " << alive.size();
}

std::size_t ConnectionManager::kickUser(long long userId,
                                        const std::string& reason,
                                        const std::function<bool(const Session&)>& match) {
    if (userId == 0) return 0;
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const auto& session = it->second;
            if (session->userId() == userId && (!match || match(*session))) {
                targets.push_back(session);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& session : targets) {
        Json payload = Json::object();
        payload.set("reason", Json(reason));
        session->send(protocol::event("session.revoked", payload));
        session->resetAuth();
        session->close(4401, "session revoked");
    }
    if (!targets.empty()) {
        SELFVOY_LOG(log::Level::Info, "connections")
            << "user=" << userId << ": закрыто соединений " << targets.size() << " (" << reason << ")";
    }
    return targets.size();
}

}  // namespace selfvoy
