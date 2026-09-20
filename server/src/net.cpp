// aura/net.cpp
#include "aura/net.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>

#include "aura/log.h"

namespace aura::net {

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

}  // namespace

bool parseUrl(const std::string& text, Url& out, std::string& error) {
    out = Url();
    std::string rest = text;
    const std::size_t schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        out.scheme = toLower(rest.substr(0, schemeEnd));
        rest = rest.substr(schemeEnd + 3);
    }
    if (out.scheme != "http" && out.scheme != "ws") {
        error = "поддерживаются только http/ws URL, получено: " + text;
        return false;
    }
    const std::size_t pathStart = rest.find('/');
    std::string authority = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    out.path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    const std::size_t portStart = authority.find(':');
    if (portStart == std::string::npos) {
        out.host = authority;
        out.port = 80;
    } else {
        out.host = authority.substr(0, portStart);
        try {
            out.port = std::stoi(authority.substr(portStart + 1));
        } catch (...) {
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
    HttpResponse response;
    Url parsed;
    if (!parseUrl(url, parsed, error)) return response;

    const int fd = connectTcp(parsed.host, parsed.port, timeoutMs, error);
    if (fd < 0) return response;

    std::ostringstream request;
    request << method << ' ' << parsed.path << " HTTP/1.1\r\n"
            << "Host: " << parsed.host << ':' << parsed.port << "\r\n"
            << "Connection: close\r\n"
            << "Accept: application/json\r\n"
            << "User-Agent: aura-server/0.1\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    for (const auto& header : headers) request << header.first << ": " << header.second << "\r\n";
    request << "\r\n" << body;

    if (!sendAll(fd, request.str(), timeoutMs)) {
        error = "не удалось отправить запрос";
        closeSocket(fd);
        return response;
    }

    std::string raw;
    char buffer[8192];
    while (true) {
        if (!waitReady(fd, timeoutMs, false)) {
            error = "таймаут чтения ответа";
            break;
        }
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            error = std::string("ошибка чтения: ") + std::strerror(errno);
            break;
        }
        if (received == 0) break;
        raw.append(buffer, static_cast<std::size_t>(received));
        if (raw.size() > 16 * 1024 * 1024) {
            error = "ответ слишком большой";
            break;
        }
    }
    closeSocket(fd);

    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (error.empty()) error = "некорректный HTTP-ответ";
        return response;
    }

    std::istringstream statusStream(raw.substr(0, raw.find("\r\n")));
    std::string version;
    statusStream >> version >> response.status;

    std::size_t cursor = raw.find("\r\n") + 2;
    std::string rest = raw.substr(cursor, headerEnd - cursor);
    std::istringstream headerStream(rest);
    std::string line;
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        response.headers[name] = value;
    }

    const std::string payload = raw.substr(headerEnd + 4);
    const auto transfer = response.headers.find("transfer-encoding");
    if (transfer != response.headers.end() && toLower(transfer->second).find("chunked") != std::string::npos) {
        std::size_t position = 0;
        while (position < payload.size()) {
            const std::size_t lineEnd = payload.find("\r\n", position);
            if (lineEnd == std::string::npos) break;
            const std::size_t chunkSize = std::strtoul(payload.substr(position, lineEnd - position).c_str(), nullptr, 16);
            if (chunkSize == 0) break;
            const std::size_t chunkStart = lineEnd + 2;
            if (chunkStart + chunkSize > payload.size()) break;
            response.body.append(payload, chunkStart, chunkSize);
            position = chunkStart + chunkSize + 2;
        }
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

}  // namespace aura::net
