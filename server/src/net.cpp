// selfvoy/net.cpp
#include "selfvoy/net.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>

#include "selfvoy/log.h"
#include "selfvoy/tls.h"

// macOS: MSG_NOSIGNAL может отсутствовать; SIGPIPE там всё равно игнорируется
// процессом (main.cpp: signal(SIGPIPE, SIG_IGN)), поэтому флаг можно опустить.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace selfvoy::net {

namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool waitReady(int fd, int timeoutMs, bool forWrite) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = forWrite ? POLLOUT : POLLIN;
    const int ready = ::poll(&descriptor, 1, timeoutMs);
    return ready > 0 && (descriptor.revents & (forWrite ? POLLOUT : POLLIN)) != 0;
}

// Ожидание готовности сокета кусками по 200 мс: между ними проверяется *cancel.
// POLLHUP/POLLERR тоже «готовность» — recv тогда вернёт 0/ошибку.
bool waitReadyCancellable(int fd, int timeoutMs, bool forWrite, const std::atomic<bool>* cancel, bool& cancelled) {
    cancelled = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (cancel && cancel->load()) {
            cancelled = true;
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = forWrite ? POLLOUT : POLLIN;
        const int slice = static_cast<int>(std::min<long long>(left.count(), cancel ? 200 : left.count()));
        const int ready = ::poll(&descriptor, 1, slice);
        if (ready > 0) return true;
        if (ready < 0 && errno != EINTR) return false;
    }
}

std::mutex caMutex;
std::string caFilePath;

std::string caFile() {
    std::lock_guard<std::mutex> lock(caMutex);
    return caFilePath;
}

// Ответ прочитан целиком: по Content-Length или по завершающему чанку. Сервер
// может не закрыть соединение (или закрыть TLS без close_notify) — ждать EOF незачем.
bool responseComplete(const std::string& raw) {
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    const std::string head = toLower(raw.substr(0, headerEnd));
    const std::size_t body = headerEnd + 4;
    if (head.find("\r\ntransfer-encoding:") != std::string::npos && head.find("chunked") != std::string::npos) {
        return raw.size() >= body + 5 && raw.compare(raw.size() - 5, 5, "0\r\n\r\n") == 0;
    }
    const std::size_t length = head.find("\r\ncontent-length:");
    if (length == std::string::npos) return false;
    const unsigned long long expected = std::strtoull(head.c_str() + length + 17, nullptr, 10);
    return raw.size() - body >= expected;
}

}  // namespace

void setCaFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(caMutex);
    caFilePath = path;
}

bool parseUrl(const std::string& text, Url& out, std::string& error) {
    out = Url();
    std::string rest = text;
    const std::size_t schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        out.scheme = toLower(rest.substr(0, schemeEnd));
        rest = rest.substr(schemeEnd + 3);
    }
    if (out.scheme != "http" && out.scheme != "https" && out.scheme != "ws") {
        error = "поддерживаются только http/https/ws URL, получено: " + text;
        return false;
    }
    const int defaultPort = out.scheme == "https" ? 443 : 80;
    const std::size_t pathStart = rest.find('/');
    std::string authority = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    out.path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    const std::size_t portStart = authority.find(':');
    if (portStart == std::string::npos) {
        out.host = authority;
        out.port = defaultPort;
    } else {
        out.host = authority.substr(0, portStart);
        try {
            out.port = std::stoi(authority.substr(portStart + 1));
        } catch (...) {
            error = "некорректный порт в URL: " + text;
            return false;
        }
        if (out.port <= 0 || out.port > 65535) {
            error = "некорректный порт в URL: " + text;
            return false;
        }
    }
    if (out.host.empty()) {
        error = "пустой хост в URL: " + text;
        return false;
    }
    return true;
}

int connectTcp(const std::string& host, int port, int timeoutMs, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const int status = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved);
    if (status != 0) {
        error = std::string("не удалось разрешить адрес: ") + gai_strerror(status);
        return -1;
    }

    int fd = -1;
    for (addrinfo* info = resolved; info != nullptr; info = info->ai_next) {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) continue;

        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int connectResult = ::connect(fd, info->ai_addr, info->ai_addrlen);
        if (connectResult == 0) {
            ::fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            if (waitReady(fd, timeoutMs, true)) {
                int socketError = 0;
                socklen_t length = sizeof(socketError);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length);
                if (socketError == 0) {
                    ::fcntl(fd, F_SETFL, flags);
                    break;
                }
                errno = socketError;
            }
        }
        const int savedErrno = errno;
        ::close(fd);
        fd = -1;
        error = std::string("не удалось подключиться к ") + host + ":" + std::to_string(port) + " — " +
                std::strerror(savedErrno);
    }
    ::freeaddrinfo(resolved);
    if (fd < 0 && error.empty()) error = "не удалось подключиться к " + host;
    return fd;
}

bool sendAll(int fd, const std::string& data, int timeoutMs) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (!waitReady(fd, timeoutMs, true)) return false;
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            if (written < 0 && (errno == EINTR)) continue;
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

void closeSocket(int fd) {
    if (fd >= 0) ::close(fd);
}

HttpResponse httpRequest(const std::string& method,
                         const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body,
                         int timeoutMs,
                         std::string& error) {
    return httpRequest(method, url, headers, body, timeoutMs, error, nullptr);
}

HttpResponse httpRequest(const std::string& method,
                         const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body,
                         int timeoutMs,
                         std::string& error,
                         const std::atomic<bool>* cancel) {
    HttpResponse response;
    Url parsed;
    if (!parseUrl(url, parsed, error)) return response;
    const bool secure = parsed.scheme == "https";
    if (secure && !tls::available()) {
        error = "сборка без TLS: https недоступен (пересоберите сервер с OpenSSL) — " + parsed.host;
        return response;
    }

    const int fd = connectTcp(parsed.host, parsed.port, timeoutMs, error);
    if (fd < 0) return response;

    tls::Client tlsClient;
    if (secure) {
        // Рукопожатие и чтение TLS-записей — с таймаутом сокета (SSL_* блокирующие).
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (!tlsClient.connect(fd, parsed.host, caFile(), "SELFVOY_CA_FILE", error)) {
            closeSocket(fd);
            return response;
        }
    }

    const bool defaultPort = parsed.port == (secure ? 443 : 80);
    std::ostringstream request;
    request << method << ' ' << parsed.path << " HTTP/1.1\r\n"
            << "Host: " << parsed.host;
    if (!defaultPort) request << ':' << parsed.port;
    request << "\r\n"
            << "Connection: close\r\n"
            << "Accept: application/json\r\n"
            << "User-Agent: selfvoy-server/0.1\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    for (const auto& header : headers) request << header.first << ": " << header.second << "\r\n";
    request << "\r\n" << body;

    const std::string wire = request.str();
    bool sent = false;
    if (secure) {
        std::size_t offset = 0;
        while (offset < wire.size()) {
            const long n = tlsClient.write(wire.data() + offset, wire.size() - offset);
            if (n <= 0) break;
            offset += static_cast<std::size_t>(n);
        }
        sent = offset == wire.size();
    } else {
        sent = sendAll(fd, wire, timeoutMs);
    }
    if (!sent) {
        error = "не удалось отправить запрос";
        tlsClient.shutdown();
        closeSocket(fd);
        return response;
    }

    std::string raw;
    char buffer[8192];
    while (true) {
        if (!(secure && tlsClient.pending())) {
            bool cancelled = false;
            if (!waitReadyCancellable(fd, timeoutMs, false, cancel, cancelled)) {
                error = cancelled ? "запрос прерван" : "таймаут чтения ответа";
                break;
            }
        }
        long received = 0;
        if (secure) {
            received = tlsClient.read(buffer, sizeof(buffer));
        } else {
            received = ::recv(fd, buffer, sizeof(buffer), 0);
            if (received < 0 && errno == EINTR) continue;
        }
        if (received < 0) {
            if (!responseComplete(raw)) {
                error = secure ? std::string("ошибка чтения TLS") : std::string("ошибка чтения: ") + std::strerror(errno);
            }
            break;
        }
        if (received == 0) break;
        raw.append(buffer, static_cast<std::size_t>(received));
        if (raw.size() > 16 * 1024 * 1024) {
            error = "ответ слишком большой";
            break;
        }
        if (responseComplete(raw)) break;
    }
    tlsClient.shutdown();
    closeSocket(fd);
    if (!error.empty() && error != "ответ слишком большой" && responseComplete(raw)) error.clear();

    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (error.empty()) error = "некорректный HTTP-ответ";
        return response;
    }
    if (!parseResponseHead(raw.substr(0, headerEnd + 2), response)) {
        if (error.empty()) error = "некорректный HTTP-ответ";
        return response;
    }

    const std::string payload = raw.substr(headerEnd + 4);
    const auto transfer = response.headers.find("transfer-encoding");
    if (transfer != response.headers.end() && toLower(transfer->second).find("chunked") != std::string::npos) {
        response.body = decodeChunked(payload);
    } else {
        response.body = payload;
    }
    return response;
}

bool pingAiService(const std::string& baseUrl, int timeoutMs, std::string& error) {
    std::string url = baseUrl;
    if (!url.empty() && url.back() == '/') url.pop_back();
    const HttpResponse response = httpRequest("GET", url + "/healthz", {}, "", timeoutMs, error);
    return response.ok();
}

bool parseResponseHead(const std::string& head, HttpResponse& out) {
    const std::size_t firstLineEnd = head.find("\r\n");
    if (firstLineEnd == std::string::npos) return false;
    std::istringstream statusStream(head.substr(0, firstLineEnd));
    std::string version;
    statusStream >> version >> out.status;
    if (out.status == 0) return false;

    std::istringstream headerStream(head.substr(firstLineEnd + 2));
    std::string line;
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        const std::size_t start = value.find_first_not_of(" \t");
        if (start != std::string::npos) value = value.substr(start);
        out.headers[name] = value;
    }
    return true;
}

std::string decodeChunked(const std::string& raw) {
    std::string body;
    std::size_t position = 0;
    while (position < raw.size()) {
        const std::size_t lineEnd = raw.find("\r\n", position);
        if (lineEnd == std::string::npos) break;
        const std::size_t chunkSize =
            std::strtoul(raw.substr(position, lineEnd - position).c_str(), nullptr, 16);
        if (chunkSize == 0) break;
        const std::size_t chunkStart = lineEnd + 2;
        if (chunkStart + chunkSize > raw.size()) break;
        body.append(raw, chunkStart, chunkSize);
        position = chunkStart + chunkSize + 2;
    }
    return body;
}

std::string HttpResponse::header(const std::string& name) const {
    const auto it = headers.find(toLower(name));
    return it == headers.end() ? std::string() : it->second;
}

}  // namespace selfvoy::net
