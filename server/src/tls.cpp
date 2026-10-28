// selfvoy/tls.cpp — клиентский TLS (OpenSSL): общий для SMTP и https.
#include "selfvoy/tls.h"

#ifdef SELFVOY_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace selfvoy::tls {

bool available() {
#ifdef SELFVOY_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

Client::~Client() { shutdown(); }

#ifdef SELFVOY_HAVE_OPENSSL

namespace {

std::string lastSslError() {
    char buf[256] = {0};
    const unsigned long code = ERR_get_error();
    if (code == 0) return "соединение закрыто";
    ERR_error_string_n(code, buf, sizeof buf);
    return buf;
}

bool fail(const std::string& reason, std::string& error) {
    error = reason;
    return false;
}

}  // namespace

bool Client::connect(int fd, const std::string& host, const std::string& caFile, const std::string& caFileName,
                     std::string& error) {
    shutdown();
    ERR_clear_error();
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) return fail("TLS: не удалось создать контекст", error);
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ctx_);
    if (!caFile.empty() && SSL_CTX_load_verify_locations(ctx_, caFile.c_str(), nullptr) != 1) {
        return fail("TLS: не удалось прочитать " + (caFileName.empty() ? std::string("файл CA") : caFileName),
                    error);
    }
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    ssl_ = SSL_new(ctx_);
    if (!ssl_) return fail("TLS: не удалось создать сессию", error);
    SSL_set_fd(ssl_, fd);
    const bool ip = host.find_first_not_of("0123456789.:") == std::string::npos;
    if (ip) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), host.c_str());
    } else {
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
    }
    if (SSL_connect(ssl_) != 1) {
        const long verify = SSL_get_verify_result(ssl_);
        const std::string reason =
            verify != X509_V_OK
                ? std::string("TLS: сертификат сервера не прошёл проверку — ") + X509_verify_cert_error_string(verify)
                : "TLS: рукопожатие не удалось — " + lastSslError();
        // Сессию после неудачного рукопожатия не закрываем «вежливо»: close_notify некуда слать.
        SSL_free(ssl_);
        ssl_ = nullptr;
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return fail(reason, error);
    }
    return true;
}

long Client::write(const void* data, std::size_t size) {
    if (!ssl_) return -1;
    return SSL_write(ssl_, data, static_cast<int>(size));
}

long Client::read(void* buffer, std::size_t size) {
    if (!ssl_) return -1;
    const int n = SSL_read(ssl_, buffer, static_cast<int>(size));
    if (n > 0) return n;
    return SSL_get_error(ssl_, n) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

bool Client::pending() const { return ssl_ != nullptr && SSL_pending(ssl_) > 0; }

void Client::shutdown() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

#else  // !SELFVOY_HAVE_OPENSSL

bool Client::connect(int, const std::string&, const std::string&, const std::string&, std::string& error) {
    error = "сборка без TLS: сервер собран без OpenSSL";
    return false;
}

long Client::write(const void*, std::size_t) { return -1; }
long Client::read(void*, std::size_t) { return -1; }
bool Client::pending() const { return false; }
void Client::shutdown() {}

#endif  // SELFVOY_HAVE_OPENSSL

}  // namespace selfvoy::tls
