// selfvoy/tls.h — клиентский TLS поверх открытого TCP-сокета (OpenSSL).
//
// Общая обвязка для SMTP (STARTTLS/SMTPS) и https в net::httpRequest:
// TLS не ниже 1.2, проверка цепочки сертификата (системные CA + свой корневой
// сертификат, если задан), SNI и сверка имени хоста (или IP-адреса).
// Без OpenSSL (сборка без SELFVOY_HAVE_OPENSSL) available() == false, а
// Client::connect возвращает понятную ошибку.
#pragma once

#include <cstddef>
#include <string>

struct ssl_st;
struct ssl_ctx_st;

namespace selfvoy::tls {

// Собран ли сервер с поддержкой TLS.
bool available();

class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Рукопожатие на уже подключённом сокете fd (сокет Client не закрывает).
    // caFile — дополнительный корневой сертификат ("" — только системные CA);
    // caFileName — имя переменной окружения для текста ошибки его чтения.
    bool connect(int fd, const std::string& host, const std::string& caFile, const std::string& caFileName,
                 std::string& error);
    bool active() const { return ssl_ != nullptr; }

    // > 0 — записано/прочитано байт; 0 у read — соединение закрыто; < 0 — ошибка.
    long write(const void* data, std::size_t size);
    long read(void* buffer, std::size_t size);
    // Есть расшифрованные, ещё не прочитанные байты (poll по сокету их не видит).
    bool pending() const;
    // close_notify и освобождение сессии (идемпотентно).
    void shutdown();

private:
    ssl_ctx_st* ctx_ = nullptr;
    ssl_st* ssl_ = nullptr;
};

}  // namespace selfvoy::tls
