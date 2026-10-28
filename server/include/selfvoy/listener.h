// selfvoy/listener.h — ожидает новые подключения и передаёт их в Server.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "selfvoy/session.h"

namespace selfvoy {

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
    // После stop(): закрыть ещё живые соединения (в т.ч. не прошедшие рукопожатие)
    // и дождаться рабочих потоков — они вызывают обработчик Server и трогают
    // поля Listener, поэтому не должны пережить их владельца. false — не успели.
    bool drain(std::chrono::milliseconds timeout);

    int port() const { return boundPort_; }
    bool running() const { return running_.load(); }
    unsigned long long accepted() const { return accepted_.load(); }

private:
    void acceptLoop();
    void handle(std::shared_ptr<Session> session);
    // Лимит соединений с одного IP: true — слот занят, false — лимит исчерпан.
    bool acquireIpSlot(const std::string& ip);
    void releaseIpSlot(const std::string& ip);

    const Config& config_;
    ConnectionHandler handler_;
    int listenSocket_ = -1;
    int boundPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<unsigned long long> accepted_{0};
    std::thread acceptThread_;
    std::atomic<unsigned long long> activeWorkers_{0};
    std::mutex workersMutex_;
    std::condition_variable workersDone_;
    std::map<const Session*, std::weak_ptr<Session>> live_;  // под workersMutex_
    std::mutex perIpMutex_;
    std::map<std::string, std::size_t> perIp_;
};

}  // namespace selfvoy
