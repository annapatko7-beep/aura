// aura/mailprovider.cpp — драйверы отправки писем.
#include "aura/mailprovider.h"

#include "aura/config.h"
#include "aura/log.h"

namespace aura {

namespace {

// Логирует письмо целиком: локальная разработка без SMTP.
class LogMailProvider : public MailProvider {
public:
    explicit LogMailProvider(bool expose) : expose_(expose) {}

    bool exposesCodes() const override { return expose_; }

    bool sendVerificationCode(const std::string& to, const std::string& code) override {
        AURA_LOG(log::Level::Info, "mail")
            << "КОД ПОДТВЕРЖДЕНИЯ для " << to << ": " << code
            << (expose_ ? "" : " (код также вернётся в ответе API — режим dev выключен)");
        return true;
    }

    bool sendPasswordReset(const std::string& to, const std::string& code) override {
        AURA_LOG(log::Level::Info, "mail") << "КОД СБРОСА ПАРОЛЯ для " << to << ": " << code;
        return true;
    }

private:
    bool expose_;
};

}  // namespace

std::unique_ptr<MailProvider> makeMailProvider(const Config& config) {
    if (config.mailDriver == "dev") return std::make_unique<LogMailProvider>(true);
    // "log" и неизвестные значения — безопасный режим без раскрытия кодов.
    return std::make_unique<LogMailProvider>(false);
}

}  // namespace aura
