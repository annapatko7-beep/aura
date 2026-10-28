// selfvoy/mailprovider.h — отправка писем (подтверждение email, сброс пароля).
//
// Драйверы:
//   dev — пишет код в лог И возвращает его в ответе API (только локальная
//         разработка: требует SELFVOY_DEV_MODE=1, иначе сервер не стартует);
//   log — значение по умолчанию: в лог пишется только факт выдачи кода, сам
//         код не раскрывается ни в логе, ни в API (стенд без SMTP);
//   smtp — настоящая отправка по SMTP (SELFVOY_SMTP_HOST/PORT/USER/PASSWORD/FROM/TLS,
//          см. selfvoy/smtp.h): письмо с кодом подтверждения или сброса, код наружу
//          не отдаётся.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace selfvoy {

struct Config;

// Письмо-уведомление (страницы записи и т.п.): заголовок, абзац, строки (время
// встреч), кнопка-ссылка и примечание. Весь текст экранируется в HTML; в письмо
// не должны попадать данные, которые задаёт анонимный отправитель формы.
struct MailNotice {
    std::string kind;     // для логов: booking.created и т.п.
    std::string to;
    std::string subject;
    std::string title;
    std::string lead;
    std::vector<std::string> lines;
    std::string linkUrl;  // пусто — без кнопки
    std::string linkText;
    std::vector<std::string> footer;  // абзацы после кнопки («Напоминания в Telegram: …»)
    std::string note;
};

class MailProvider {
public:
    virtual ~MailProvider() = default;

    // true — драйвер позволяет вернуть код в API-ответе (режим dev).
    virtual bool exposesCodes() const = 0;
    // true — письма действительно доходят до адресата (smtp; dev — в devOutbox).
    // log только пишет в журнал: ссылку, которую можно получить лишь письмом, с ним менять нельзя.
    virtual bool delivers() const = 0;

    virtual bool sendVerificationCode(const std::string& to, const std::string& code) = 0;
    virtual bool sendPasswordReset(const std::string& to, const std::string& code) = 0;
    virtual bool sendNotice(const MailNotice& notice) = 0;
};

// Письма-уведомления, «отправленные» драйвером dev (SELFVOY_DEV_MODE): последние 50,
// для тестов и локальной отладки. В остальных режимах пусто.
std::vector<MailNotice> devOutbox();

// Фабрика по имени драйвера из Config::mailDriver.
std::unique_ptr<MailProvider> makeMailProvider(const Config& config);

}  // namespace selfvoy
