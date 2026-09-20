// aura/listener.h — ожидает новые подключения и передаёт их в Server.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aura/session.h"

namespace aura {

struct Config;

class Listener {
public:
    // Обработчик вызывается в отдельном потоке на каждое подключение.
    using ConnectionHandler = std::function<void(std::shared_ptr<Session>)>;

    Listener(const Config& config, ConnectionHandler handler);
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    bool start(std::string& error);
    void stop();

    int port() const { return boundPort_; }
    bool running() const { return running_.load(); }
    unsigned long long accepted() const { return accepted_.load(); }

private:
    void acceptLoop();
    void handle(std::shared_ptr<Session> session);

    const Config& config_;
    ConnectionHandler handler_;
    int listenSocket_ = -1;
    int boundPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<unsigned long long> accepted_{0};
    std::thread acceptThread_;
    std::atomic<unsigned long long> activeWorkers_{0};
};

}  // namespace aura
