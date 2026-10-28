// selfvoy/net.h — минимальный TCP/HTTP(S)-клиент.
//
// Запросы в Python AI Service (внутри docker-сети — обычный http), во внешние
// API (Google, Telegram Bot API, push-шлюз). https работает напрямую, если сервер
// собран с OpenSSL (SELFVOY_HAVE_OPENSSL): TLS ≥ 1.2, проверка сертификата
// (системные CA + SELFVOY_CA_FILE), SNI и имени хоста — общая обвязка
// selfvoy/tls.h, та же, что у SMTP. Без OpenSSL https-запрос возвращает ошибку
// «сборка без TLS».
#pragma once

#include <atomic>
#include <map>
#include <string>

namespace selfvoy::net {

struct Url {
    std::string scheme = "http";  // http | https | ws
    std::string host;
    int port = 80;                // по умолчанию 80 (http/ws) или 443 (https)
    std::string path = "/";
};

bool parseUrl(const std::string& text, Url& out, std::string& error);

// Дополнительный корневой сертификат для https (SELFVOY_CA_FILE; "" — только системные CA).
void setCaFile(const std::string& path);

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

// То же с возможностью прервать ожидание: *cancel == true — запрос завершается
// (не позже чем через ~200 мс) с ошибкой «запрос прерван». Нужно долгим
// запросам (long polling Telegram), чтобы остановка сервера их не ждала.
HttpResponse httpRequest(const std::string& method,
                         const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body,
                         int timeoutMs,
                         std::string& error,
                         const std::atomic<bool>* cancel);

// Проверка доступности AI-сервиса (используется в healthcheck'е сервера).
bool pingAiService(const std::string& baseUrl, int timeoutMs, std::string& error);

}  // namespace selfvoy::net
