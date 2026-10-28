// selfvoy/mailprovider.cpp — драйверы отправки писем.
#include "selfvoy/mailprovider.h"

#include <algorithm>
#include <deque>
#include <mutex>

#include "selfvoy/config.h"
#include "selfvoy/log.h"
#include "selfvoy/smtp.h"

namespace selfvoy {

namespace {

std::mutex outboxMutex;
std::deque<MailNotice> outbox;

// dev (SELFVOY_DEV_MODE=1): код пишется в лог и возвращается в ответе API.
// log: в лог попадает только факт выдачи кода — сам код не раскрывается
// (стенд без почты: код получить нельзя, нужен SMTP).
class LogMailProvider : public MailProvider {
public:
    explicit LogMailProvider(bool expose) : expose_(expose) {}

    bool exposesCodes() const override { return expose_; }
    bool delivers() const override { return expose_; }  // dev — во «входящие» devOutbox, log — никуда

    bool sendVerificationCode(const std::string& to, const std::string& code) override {
        if (expose_) {
            SELFVOY_LOG(log::Level::Info, "mail") << "[dev] КОД ПОДТВЕРЖДЕНИЯ для " << to << ": " << code;
        } else {
            SELFVOY_LOG(log::Level::Info, "mail") << "выдан код подтверждения email для " << to
                                               << " (драйвер log: письмо не отправляется)";
        }
        return true;
    }

    bool sendPasswordReset(const std::string& to, const std::string& code) override {
        if (expose_) {
            SELFVOY_LOG(log::Level::Info, "mail") << "[dev] КОД СБРОСА ПАРОЛЯ для " << to << ": " << code;
        } else {
            SELFVOY_LOG(log::Level::Info, "mail") << "выдан код сброса пароля для " << to
                                               << " (драйвер log: письмо не отправляется)";
        }
        return true;
    }

    bool sendNotice(const MailNotice& notice) override {
        if (expose_) {
            SELFVOY_LOG(log::Level::Info, "mail") << "[dev] письмо (" << notice.kind << ") для " << notice.to << ": "
                                               << notice.subject << (notice.linkUrl.empty() ? "" : " — ")
                                               << notice.linkUrl;
            std::lock_guard<std::mutex> lock(outboxMutex);
            outbox.push_back(notice);
            if (outbox.size() > 50) outbox.pop_front();
        } else {
            SELFVOY_LOG(log::Level::Info, "mail") << "письмо (" << notice.kind << ") для " << notice.to
                                               << " (драйвер log: письмо не отправляется)";
        }
        return true;
    }

private:
    bool expose_;
};

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            case '\n': out += "<br>"; break;
            case '\r': break;
            default: out.push_back(ch);
        }
    }
    return out;
}

// Только http(s)-ссылки попадают в href (адрес собирает сервер из SELFVOY_PUBLIC_URL).
bool safeLink(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

// Строка таблицы деталей встречи (в стиле Google Calendar / Stripe).
std::string renderDetailRow(const std::string& line) {
    auto colon = line.find(':');
    if (colon != std::string::npos && colon + 1 < line.size()) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        return "<tr><td style=\"padding:10px 14px;color:#8a96a3;font-size:13px;font-weight:500;border-bottom:1px solid #212936;width:32%;vertical-align:top;\">" +
               htmlEscape(key) + "</td><td style=\"padding:10px 14px;color:#e1e7ee;font-size:13px;font-weight:600;border-bottom:1px solid #212936;vertical-align:top;\">" +
               htmlEscape(val) + "</td></tr>";
    }
    return "<tr><td colspan=\"2\" style=\"padding:10px 14px;color:#e1e7ee;font-size:13px;font-weight:500;border-bottom:1px solid #212936;\">" +
           htmlEscape(line) + "</td></tr>";
}

// HTML-уведомление в строгом инженерном стиле (Google Workspace / Steam / Stripe).
std::string htmlNotice(const MailNotice& notice) {
    std::string sectionName = "Календарь и встречи";
    if (notice.kind.find("invite") != std::string::npos) {
        sectionName = "Приглашения";
    }

    std::string tableContent;
    if (!notice.lines.empty()) {
        std::string rows;
        for (const auto& line : notice.lines) {
            rows += renderDetailRow(line);
        }
        tableContent = "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"margin:20px 0;background-color:#0c1017;border:1px solid #283445;border-radius:6px;overflow:hidden;\">" +
                       rows + "</table>";
    }

    std::string button;
    if (!notice.linkUrl.empty() && safeLink(notice.linkUrl)) {
        button = "<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"margin:22px 0 12px 0;\">"
                 "<tr><td style=\"border-radius:4px;background-color:#1a73e8;\">"
                 "<a href=\"" + htmlEscape(notice.linkUrl) + "\" target=\"_blank\" style=\"display:inline-block;padding:12px 24px;"
                 "font-size:14px;font-weight:600;color:#ffffff;text-decoration:none;letter-spacing:-0.1px;\">" +
                 htmlEscape(notice.linkText) + "</a></td></tr></table>";
    }

    std::string footerText;
    for (const auto& line : notice.footer) {
        footerText += "<p style=\"color:#8a96a3;font-size:13px;line-height:1.5;margin:12px 0 0 0;\">" + htmlEscape(line) + "</p>";
    }

    std::string noteBlock;
    if (!notice.note.empty()) {
        noteBlock = "<div style=\"background-color:#121822;border-left:3px solid #1a73e8;padding:12px 14px;border-radius:0 4px 4px 0;font-size:13px;color:#8a96a3;line-height:1.5;margin-top:20px;\">" +
                    htmlEscape(notice.note) + "</div>";
    }

    return "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"></head>"
           "<body style=\"margin:0;padding:32px 16px;background-color:#0f131a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased;color:#c3cbd4;\">"
           "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr><td align=\"center\">"
           "<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"max-width:540px;width:100%;background-color:#171f2b;border:1px solid #283445;border-radius:6px;overflow:hidden;\">"
           "<tr><td style=\"padding:24px 28px;\">"
           "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"padding-bottom:16px;border-bottom:1px solid #283445;\"><tr>"
           "<td><span style=\"font-size:18px;font-weight:700;color:#ffffff;letter-spacing:-0.3px;\">Selfvoy</span></td>"
           "<td align=\"right\"><span style=\"font-size:12px;color:#8a96a3;\">" + htmlEscape(sectionName) + "</span></td>"
           "</tr></table>"
           "<h1 style=\"font-size:19px;font-weight:600;color:#ffffff;line-height:1.4;margin:22px 0 10px 0;\">" + htmlEscape(notice.title) + "</h1>"
           "<p style=\"font-size:14px;line-height:1.6;color:#c3cbd4;margin:0 0 16px 0;\">" + htmlEscape(notice.lead) + "</p>" +
           tableContent + button + footerText + noteBlock +
           "<div style=\"margin-top:28px;padding-top:16px;border-top:1px solid #283445;font-size:12px;color:#626d7a;line-height:1.6;\">"
           "Это сервисное уведомление отправлено автоматически на ваш адрес.<br>&copy; 2026 Selfvoy. Все права защищены.</div>"
           "</td></tr></table></td></tr></table></body></html>";
}

// «24 часа», «1 час», «30 минут» — срок действия кода в тексте письма.
std::string ttlText(long long seconds) {
    auto word = [](long long n, const char* one, const char* few, const char* many) {
        const long long m10 = n % 10, m100 = n % 100;
        if (m10 == 1 && m100 != 11) return one;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
        return many;
    };
    if (seconds >= 3600 && seconds % 3600 == 0) {
        const long long h = seconds / 3600;
        return std::to_string(h) + " " + word(h, "час", "часа", "часов");
    }
    const long long m = std::max(1LL, seconds / 60);
    return std::to_string(m) + " " + word(m, "минуту", "минуты", "минут");
}

// HTML-письмо с кодом безопасности (в стиле Steam Guard / Google Security).
std::string htmlLetter(const std::string& title, const std::string& lead, const std::string& code,
                       const std::string& note) {
    const std::string noteText = note.empty()
        ? "Никогда не сообщайте этот код третьим лицам. Сотрудники Selfvoy никогда не запрашивают ваши пароли или коды доступа."
        : htmlEscape(note);

    return "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"></head>"
           "<body style=\"margin:0;padding:32px 16px;background-color:#0f131a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased;color:#c3cbd4;\">"
           "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr><td align=\"center\">"
           "<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"max-width:540px;width:100%;background-color:#171f2b;border:1px solid #283445;border-radius:6px;overflow:hidden;\">"
           "<tr><td style=\"padding:24px 28px;\">"
           "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"padding-bottom:16px;border-bottom:1px solid #283445;\"><tr>"
           "<td><span style=\"font-size:18px;font-weight:700;color:#ffffff;letter-spacing:-0.3px;\">Selfvoy</span></td>"
           "<td align=\"right\"><span style=\"font-size:12px;color:#8a96a3;\">Служба безопасности</span></td>"
           "</tr></table>"
           "<h1 style=\"font-size:19px;font-weight:600;color:#ffffff;line-height:1.4;margin:22px 0 10px 0;\">" + htmlEscape(title) + "</h1>"
           "<p style=\"font-size:14px;line-height:1.6;color:#c3cbd4;margin:0 0 16px 0;\">" + htmlEscape(lead) + "</p>"
           "<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" width=\"100%\" style=\"margin:22px 0;\"><tr>"
           "<td align=\"center\" style=\"background-color:#0c1017;border:1px solid #283445;border-radius:6px;padding:22px 16px;\">"
           "<div style=\"font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:1.5px;color:#8a96a3;margin-bottom:8px;\">Код подтверждения</div>"
           "<div style=\"font-family:'Consolas','Courier New',Courier,monospace;font-size:36px;font-weight:700;letter-spacing:8px;color:#66c0f4;margin-left:8px;\">" + htmlEscape(code) + "</div>"
           "</td></tr></table>"
           "<div style=\"background-color:#121822;border-left:3px solid #66c0f4;padding:12px 14px;border-radius:0 4px 4px 0;font-size:13px;color:#8a96a3;line-height:1.5;margin-top:20px;\">"
           "<strong>Срок действия:</strong> 5 минут.<br>" +
           noteText + "</div>"
           "<div style=\"margin-top:28px;padding-top:16px;border-top:1px solid #283445;font-size:12px;color:#626d7a;line-height:1.6;\">"
           "Это письмо отправлено автоматически в связи с операцией входа или регистрации.<br>&copy; 2026 Selfvoy. Все права защищены.</div>"
           "</td></tr></table></td></tr></table></body></html>";
}

// Настоящая отправка по SMTP.
class SmtpMailProvider : public MailProvider {
public:
    explicit SmtpMailProvider(const Config& config) : verifyTtl_(config.verifyTtlSeconds), resetTtl_(config.resetTtlSeconds) {
        settings_.host = config.smtpHost;
        settings_.port = config.smtpPort;
        settings_.user = config.smtpUser;
        settings_.password = config.smtpPassword;
        settings_.from = config.smtpFrom;
        settings_.tls = config.smtpTls;
        settings_.caFile = config.smtpCaFile;
        settings_.timeoutMs = config.smtpTimeoutMs;
    }

    bool exposesCodes() const override { return false; }
    bool delivers() const override { return true; }

    bool sendVerificationCode(const std::string& to, const std::string& code) override {
        smtp::Message message;
        message.to = to;
        message.subject = "Код подтверждения Selfvoy: " + code;
        const std::string ttl = ttlText(verifyTtl_);
        message.text = "Здравствуйте!\r\n\r\nВаш код подтверждения email в Selfvoy: " + code +
                       "\r\n\r\nКод действует " + ttl +
                       ". Если вы не регистрировались в Selfvoy, просто проигнорируйте это письмо.\r\n";
        message.html = htmlLetter("Подтвердите email", "Введите этот код в приложении Selfvoy:", code,
                                  "Код действует " + ttl + ". Если вы не регистрировались в Selfvoy, просто проигнорируйте письмо.");
        return deliver(message, "подтверждение");
    }

    bool sendPasswordReset(const std::string& to, const std::string& code) override {
        smtp::Message message;
        message.to = to;
        message.subject = "Сброс пароля Selfvoy";
        const std::string ttl = ttlText(resetTtl_);
        message.text = "Здравствуйте!\r\n\r\nКто-то запросил сброс пароля для вашего аккаунта Selfvoy.\r\nКод сброса: " +
                       code + "\r\n\r\nКод действует " + ttl +
                       ". Если это были не вы — ничего не делайте, пароль останется прежним.\r\n";
        message.html = htmlLetter("Сброс пароля", "Введите этот код в приложении Selfvoy, чтобы задать новый пароль:", code,
                                  "Код действует " + ttl + ". Если это были не вы — ничего не делайте, пароль останется прежним.");
        return deliver(message, "сброс пароля");
    }

    bool sendNotice(const MailNotice& notice) override {
        smtp::Message message;
        message.to = notice.to;
        message.subject = notice.subject;
        std::string text = notice.title + "\r\n\r\n" + notice.lead + "\r\n\r\n";
        for (const auto& line : notice.lines) text += "  " + line + "\r\n";
        if (!notice.linkUrl.empty() && safeLink(notice.linkUrl)) {
            text += "\r\n" + notice.linkText + ": " + notice.linkUrl + "\r\n";
        }
        for (const auto& line : notice.footer) text += "\r\n" + line + "\r\n";
        if (!notice.note.empty()) text += "\r\n" + notice.note + "\r\n";
        message.text = text;
        message.html = htmlNotice(notice);
        return deliver(message, notice.kind.c_str());
    }

private:
    bool deliver(const smtp::Message& message, const char* kind) {
        std::string error;
        if (smtp::send(settings_, message, error)) {
            SELFVOY_LOG(log::Level::Info, "mail") << "письмо (" << kind << ") отправлено " << message.to;
            return true;
        }
        SELFVOY_LOG(log::Level::Error, "mail") << "письмо (" << kind << ") не отправлено " << message.to << ": " << error;
        return false;
    }

    smtp::Settings settings_;
    long long verifyTtl_;
    long long resetTtl_;
};

}  // namespace

std::vector<MailNotice> devOutbox() {
    std::lock_guard<std::mutex> lock(outboxMutex);
    return {outbox.begin(), outbox.end()};
}

std::unique_ptr<MailProvider> makeMailProvider(const Config& config) {
    if (config.mailDriver == "smtp") return std::make_unique<SmtpMailProvider>(config);
    // dev раскрывает коды — только в режиме разработки (Config::validate не
    // даст запустить сервер с dev без SELFVOY_DEV_MODE; здесь — вторая линия).
    if (config.mailDriver == "dev" && config.devMode) return std::make_unique<LogMailProvider>(true);
    // "log" и неизвестные значения — безопасный режим без раскрытия кодов.
    return std::make_unique<LogMailProvider>(false);
}

}  // namespace selfvoy
