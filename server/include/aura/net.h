// aura/net.h — минимальный TCP/HTTP-клиент.
//
// Нужен двум модулям: AgentManager (запросы в Python AI Service) и ToolManager
// (вызовы внешних API). TLS намеренно не поддерживается: внутри docker-сети
// сервисы общаются по HTTP, а наружу трафик терминирует обратный прокси.
#pragma once

#include <map>
#include <string>

namespace aura::net {

struct Url {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parseUrl(const std::string& text, Url& out, std::string& error);

int connectTcp(const std::string& host, int port, int timeoutMs, std::string& error);
bool sendAll(int fd, const std::string& data, int timeoutMs);
void closeSocket(int fd);

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // ключи в нижнем регистре
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& name) const;
};

// Разбор заголовков ответа и сборка тела из chunked-кодирования (вынесено для тестов).
bool parseResponseHead(const std::string& head, HttpResponse& out);
std::string decodeChunked(const std::string& raw);

// Выполняет запрос и полностью читает ответ (Content-Length и chunked).
HttpResponse httpRequest(const std::string& method,
                         const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body,
                         int timeoutMs,
                         std::string& error);

// Проверка доступности AI-сервиса (используется в healthcheck'е сервера).
bool pingAiService(const std::string& baseUrl, int timeoutMs, std::string& error);

}  // namespace aura::net
