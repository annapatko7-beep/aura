// aura/listener.cpp
#include "aura/listener.h"

#include "aura/net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "aura/config.h"
#include "aura/crypto.h"
#include "aura/log.h"

namespace aura {

Listener::Listener(const Config& config, ConnectionHandler handler)
    : config_(config), handler_(std::move(handler)) {}

Listener::~Listener() { stop(); }

bool Listener::start(std::string& error) {
    listenSocket_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    bool ipv6 = listenSocket_ >= 0;
    if (!ipv6) listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) {
        error = std::string("не удалось создать сокет: ") + std::strerror(errno);
        return false;
    }

    const int reuse = 1;
    ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (ipv6) {
        const int off = 0;  // dual-stack: принимаем и IPv4, и IPv6
        ::setsockopt(listenSocket_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(static_cast<unsigned short>(config_.port));
        if (config_.host == "0.0.0.0" || config_.host == "::" || config_.host.empty()) {
            address.sin6_addr = in6addr_any;
        } else {
            ::inet_pton(AF_INET6, config_.host.c_str(), &address.sin6_addr);
        }
        if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = std::string("bind: ") + std::strerror(errno);
            ::close(listenSocket_);
            listenSocket_ = -1;
            return false;
        }
    } else {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(config_.port));
        if (config_.host.empty() || config_.host == "0.0.0.0") {
            address.sin_addr.s_addr = INADDR_ANY;
        } else {
            ::inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr);
        }
        if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = std::string("bind: ") + std::strerror(errno);
            ::close(listenSocket_);
            listenSocket_ = -1;
            return false;
        }
    }

    if (::listen(listenSocket_, 64) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        ::close(listenSocket_);
        listenSocket_ = -1;
        return false;
    }

    socklen_t length = sizeof(sockaddr_in6);
    sockaddr_in6 bound{};
    if (::getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        boundPort_ = ntohs(bound.sin6_port);
    } else {
        boundPort_ = config_.port;
    }

    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    AURA_LOG(log::Level::Info, "listener") << "слушаю " << config_.host << ':' << boundPort_;
    return true;
}

void Listener::stop() {
    if (!running_.exchange(false)) {
        if (listenSocket_ >= 0) {
            ::close(listenSocket_);
            listenSocket_ = -1;
        }
        return;
    }
    if (listenSocket_ >= 0) {
        ::shutdown(listenSocket_, SHUT_RDWR);
        ::close(listenSocket_);
        listenSocket_ = -1;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    // Рабочие потоки завершаются сами: Server закрывает сессии в stop().
}

void Listener::acceptLoop() {
    while (running_.load()) {
        sockaddr_in6 peer{};
        socklen_t peerLength = sizeof(peer);
        const int client = ::accept(listenSocket_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client < 0) {
            if (!running_.load()) break;
            if (errno == EINTR || errno == EAGAIN) continue;
            AURA_LOG(log::Level::Warn, "listener") << "accept: " << std::strerror(errno);
            break;
        }

        char addressText[INET6_ADDRSTRLEN] = {0};
        if (peer.sin6_family == AF_INET6) {
            const auto* ipv4Mapped = reinterpret_cast<const unsigned char*>(&peer.sin6_addr);
            static const unsigned char prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
            if (std::memcmp(ipv4Mapped, prefix, 12) == 0) {
                ::inet_ntop(AF_INET, ipv4Mapped + 12, addressText, sizeof(addressText));
            } else {
                ::inet_ntop(AF_INET6, &peer.sin6_addr, addressText, sizeof(addressText));
            }
        }

        // Поток на каждое соединение: без потолка клиент открывает тысячи
        // соединений и исчерпывает потоки/память сервера.
        if (config_.maxConnections > 0 && activeWorkers_.load() >= config_.maxConnections) {
            AURA_LOG(log::Level::Warn, "listener")
                << "превышен лимит соединений (" << config_.maxConnections << "), отказываю";
            const std::string busy =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            net::sendAll(client, busy, 1000);
            ::close(client);
            continue;
        }

        const int noDelay = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

            const std::string remote = std::string(addressText) + ":" + std::to_string(ntohs(peer.sin6_port));
        auto session = std::make_shared<Session>(client, crypto::randomHex(8), remote, config_);
        accepted_.fetch_add(1);
        AURA_LOG(log::Level::Info, "listener") << "новое подключение от " << remote;

        // Поток на соединение: их количество ограничено числом клиентов,
        // а завершившиеся освобождаются сразу (detach).
        activeWorkers_.fetch_add(1);
        std::thread([this, session] {
            handle(session);
            activeWorkers_.fetch_sub(1);
        }).detach();
    }
}

void Listener::handle(std::shared_ptr<Session> session) {
    if (handler_) handler_(session);
}

}  // namespace aura
