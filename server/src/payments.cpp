// selfvoy/payments.cpp — оплата записи и ЮKassa владельца (F4).
#include "selfvoy/payments.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc
#include "selfvoy/plan.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace payments {

std::string amountText(long long minor) {
    if (minor < 0) minor = 0;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%lld.%02lld", minor / 100, minor % 100);
    return buffer;
}

bool validCurrency(const std::string& currency) {
    return currency == "RUB" || currency == "USD" || currency == "EUR" || currency == "KZT" || currency == "BYN";
}

bool validStatus(const std::string& status) {
    return status == "pending" || status == "claimed" || status == "paid" || status == "refunded" ||
           status == "waived" || status == "expired";
}

Json guestView(const Json& payment) {
    if (!payment.isObject()) return Json();
    Json view = payment;
    view.erase("provider_id");
    // F10: служебные поля напоминания о чеке «Мой налог» — только владельцу
    // (receipt_url / receipt_at гость видит).
    view.erase("npd");
    view.erase("receipt_task_id");
    view.erase("receipt_refund_task_id");
    return view;
}

}  // namespace payments

namespace {

// Ответ API по умолчанию — через net::httpRequest (https при сборке с OpenSSL).
class HttpTransport : public PaymentManager::Transport {
public:
    net::HttpResponse request(const std::string& method, const std::string& url,
                              const std::map<std::string, std::string>& headers, const std::string& body,
                              int timeoutMs, std::string& error) override {
        return net::httpRequest(method, url, headers, body, timeoutMs, error);
    }
};

// Описание ошибки ЮKassa ({type: error, code, description}) для лога: без секретов
// (их в ответе нет), не длиннее 200 байт.
std::string describeError(const net::HttpResponse& response, const std::string& transportError) {
    if (!transportError.empty()) return transportError;
    std::string text = "HTTP " + std::to_string(response.status);
    const Json body = Json::parse(response.body);
    if (body.isObject()) {
        const std::string code = body.getString("code");
        const std::string description = body.getString("description");
        if (!code.empty()) text += " " + code;
        if (!description.empty()) text += ": " + description;
    }
    if (text.size() > 200) text.resize(200);
    return text;
}

bool digitsOnly(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

}  // namespace

PaymentManager::PaymentManager(DatabaseManager& database, const Config& config, std::unique_ptr<Transport> transport)
    : database_(database), config_(config) {
    if (transport) {
        transport_ = std::move(transport);
    } else {
        transport_ = std::make_shared<HttpTransport>();
    }
}

void PaymentManager::setTransport(std::unique_ptr<Transport> transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = std::move(transport);
}

bool PaymentManager::limited(const std::string& key, int perHour) {
    const std::time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = limits_[key];
    if (entry.second == 0 || now - entry.first >= 3600) entry = {now, 0};
    if (entry.second >= perHour) return true;
    ++entry.second;
    return false;
}

bool PaymentManager::account(long long ownerId, Account& out) {
    const auto record = database_.db().findPaymentAccount(ownerId);
    if (!record || record->provider != "yookassa") return false;
    std::string secret;
    if (!crypto::openField(config_.dataKey(), record->secretEnc, payments::kSecretAad, secret)) {
        SELFVOY_LOG(log::Level::Warn, "payments") << "секрет ЮKassa владельца " << ownerId << " не расшифровался";
        return false;
    }
    out.shopId = record->shopId;
    out.secret = std::move(secret);
    return true;
}

net::HttpResponse PaymentManager::call(const Account& account, const std::string& method, const std::string& path,
                                       const std::string& body, const std::string& idempotenceKey,
                                       std::string& error) {
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Basic " + crypto::base64Encode(account.shopId + ":" + account.secret);
    headers["Accept"] = "application/json";
    if (method == "POST") {
        headers["Content-Type"] = "application/json";
        headers["Idempotence-Key"] = idempotenceKey;
    }
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    std::string url = config_.yookassaApiUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    error.clear();
    net::HttpResponse response;
    if (transport) response = transport->request(method, url + path, headers, body, kTimeoutMs, error);
    else error = "нет транспорта";
    if (!error.empty() || !response.ok()) {
        // Только метод, путь и ответ API — ни ключа, ни заголовка Authorization.
        SELFVOY_LOG(log::Level::Warn, "payments")
            << "ЮKassa " << method << " " << path << ": " << describeError(response, error);
        if (error.empty()) error = "HTTP " + std::to_string(response.status);
    }
    return response;
}

// ------------------------------------------------------------ владелец

PaymentManager::Result PaymentManager::status(long long userId) {
    const auto record = database_.db().findPaymentAccount(userId);
    Result result;
    result.ok = true;
    result.payload.set("connected", Json(record.has_value()));
    result.payload.set("provider", Json(record ? record->provider : std::string()));
    result.payload.set("shop_id", Json(record ? record->shopId : std::string()));
    result.payload.set("connected_at", Json(record ? record->connectedAt : std::string()));
    return result;
}

PaymentManager::Result PaymentManager::connect(long long userId, const Json& payload) {
    const std::string provider = payload.getString("provider", "yookassa");
    if (provider != "yookassa") return Result::failure(protocol::code::kBadRequest, "provider — только yookassa");
    std::string shopId = payload.getString("shop_id");
    if (payload.get("shop_id").isNumber()) shopId = std::to_string(payload.getInt("shop_id"));
    if (shopId.size() > 20 || !digitsOnly(shopId)) {
        return Result::failure(protocol::code::kBadRequest, "shop_id — идентификатор магазина ЮKassa (цифры)");
    }
    const std::string secret = payload.getString("secret_key");
    const bool secretOk = secret.size() >= 8 && secret.size() <= 200 &&
                          std::all_of(secret.begin(), secret.end(), [](unsigned char ch) {
                              return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
                          });
    if (!secretOk) {
        return Result::failure(protocol::code::kBadRequest,
                               "secret_key — секретный ключ магазина ЮKassa (live_… или test_…)");
    }
    if (!plan::proAllowed(database_.db(), userId)) {
        return Result::failure(protocol::code::kProRequired, "онлайн-оплата через ЮKassa доступна на тарифе Pro");
    }
    if (config_.publicUrl.empty()) {
        return Result::failure(protocol::code::kBadRequest,
                               "на сервере не задан SELFVOY_PUBLIC_URL — гостю некуда вернуться после оплаты");
    }
    if (limited("connect:" + std::to_string(userId), kConnectPerHour)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток — попробуйте через час");
    }
    // Проверка ключей: GET /me с Basic shop_id:secret.
    const Account candidate{shopId, secret};
    std::string error;
    const net::HttpResponse response = call(candidate, "GET", "/me", "", "", error);
    if (response.status == 401 || response.status == 403) {
        return Result::failure(protocol::code::kBadRequest,
                               "ЮKassa не приняла ключи — проверьте shop_id и секретный ключ");
    }
    if (!response.ok()) {
        return Result::failure(protocol::code::kUpstream, "ЮKassa не отвечает — попробуйте позже");
    }
    const Json me = Json::parse(response.body);
    if (!me.isObject()) return Result::failure(protocol::code::kUpstream, "ЮKassa вернула непонятный ответ");
    std::string accountId = me.getString("account_id");
    if (me.get("account_id").isNumber()) accountId = std::to_string(me.getInt("account_id"));
    if (!accountId.empty() && accountId != shopId) {
        return Result::failure(protocol::code::kBadRequest, "секретный ключ выпущен для другого магазина");
    }
    if (me.getString("status") == "disabled") {
        return Result::failure(protocol::code::kBadRequest, "магазин ЮKassa отключён — проверьте его в личном кабинете");
    }

    PaymentAccountRecord record;
    record.userId = userId;
    record.provider = provider;
    record.shopId = shopId;
    record.secretEnc = crypto::sealField(config_.dataKey(), secret, payments::kSecretAad);
    if (record.secretEnc.empty()) return Result::failure(protocol::code::kInternal, "не удалось сохранить ключ");
    const DatabaseError saved = database_.db().setPaymentAccount(record);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить подключение");
    SELFVOY_LOG(log::Level::Info, "payments") << "владелец " << userId << " подключил ЮKassa (магазин " << shopId << ")";
    return status(userId);
}

PaymentManager::Result PaymentManager::disconnect(long long userId) {
    if (!database_.db().deletePaymentAccount(userId).ok) {
        return Result::failure(protocol::code::kInternal, "не удалось отключить ЮKassa");
    }
    Result result;
    result.ok = true;
    result.payload.set("connected", Json(false));
    return result;
}

bool PaymentManager::connected(long long ownerId) {
    if (config_.publicUrl.empty()) return false;
    const auto record = database_.db().findPaymentAccount(ownerId);
    return record && record->provider == "yookassa";
}

// ------------------------------------------------------------ общий механизм

Json PaymentManager::begin(long long ownerId, const std::string& kind, long long amount, const std::string& currency,
                           std::time_t dueAt, const std::string& linkUrl, bool withProvider) {
    const bool yookassa = withProvider && connected(ownerId);
    Json payment = Json::object();
    payment.set("status", Json(std::string("pending")));
    payment.set("amount", Json(amount));
    payment.set("currency", Json(currency));
    payment.set("kind", Json(kind));
    payment.set("method", Json(std::string(yookassa ? "yookassa" : "link")));
    payment.set("provider_id", Json(std::string()));
    payment.set("action_url", Json(yookassa ? std::string() : linkUrl));
    payment.set("due_at", Json(isoUtc(dueAt)));
    payment.set("paid_at", Json(std::string()));
    payment.set("refunded_at", Json(std::string()));
    return payment;
}

bool PaymentManager::submit(long long ownerId, Json& payment, const std::string& description,
                            const std::string& returnUrl, const std::string& idempotenceKey, const Json& metadata,
                            std::string& error) {
    if (payment.getString("method") != "yookassa") return true;
    Account creds;
    if (!account(ownerId, creds)) {
        error = "ЮKassa не подключена";
        return false;
    }
    Json amount = Json::object();
    amount.set("value", Json(payments::amountText(payment.getInt("amount"))));
    amount.set("currency", Json(payment.getString("currency", "RUB")));
    Json confirmation = Json::object();
    confirmation.set("type", Json(std::string("redirect")));
    confirmation.set("return_url", Json(returnUrl));
    Json body = Json::object();
    body.set("amount", amount);
    body.set("capture", Json(true));
    body.set("confirmation", confirmation);
    body.set("description", Json(description));
    body.set("metadata", metadata.isObject() ? metadata : Json::object());
    std::string transportError;
    const net::HttpResponse response = call(creds, "POST", "/payments", body.dump(), idempotenceKey, transportError);
    if (!response.ok()) {
        error = "ЮKassa не создала платёж";
        return false;
    }
    const Json answer = Json::parse(response.body);
    const std::string id = answer.isObject() ? answer.getString("id") : std::string();
    const std::string url = answer.isObject() ? answer.get("confirmation").getString("confirmation_url") : std::string();
    if (id.empty() || id.size() > 100 || (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)) {
        SELFVOY_LOG(log::Level::Warn, "payments") << "ЮKassa: в ответе POST /payments нет id или confirmation_url";
        error = "ЮKassa вернула непонятный ответ";
        return false;
    }
    payment.set("provider_id", Json(id));
    payment.set("action_url", Json(url));
    return true;  // статус (pending → paid) решает poll
}

PaymentManager::Remote PaymentManager::poll(long long ownerId, const Json& payment, std::time_t now,
                                            int minIntervalSec, std::string& error) {
    const std::string id = payment.getString("provider_id");
    if (payment.getString("method") != "yookassa" || id.empty()) {
        error = "платёж не через ЮKassa";
        return Remote::Error;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = polled_.find(id);
        if (minIntervalSec > 0 && it != polled_.end() && now - it->second < minIntervalSec && now >= it->second) {
            return Remote::Throttled;
        }
        if (polled_.size() > 10000) polled_.clear();
        polled_[id] = now;
    }
    Account creds;
    if (!account(ownerId, creds)) {
        error = "ЮKassa не подключена";
        return Remote::Error;
    }
    // id — от ЮKassa (UUID); в путь — только безопасные символы.
    if (!std::all_of(id.begin(), id.end(), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_'; })) {
        error = "некорректный id платежа";
        return Remote::Error;
    }
    const net::HttpResponse response = call(creds, "GET", "/payments/" + id, "", "", error);
    if (!response.ok()) return Remote::Error;
    const Json answer = Json::parse(response.body);
    const std::string status = answer.isObject() ? answer.getString("status") : std::string();
    if (status == "succeeded") return Remote::Succeeded;
    if (status == "canceled") return Remote::Canceled;
    if (status == "pending" || status == "waiting_for_capture") return Remote::Pending;
    error = "неизвестный статус платежа";
    return Remote::Error;
}

bool PaymentManager::refund(long long ownerId, const Json& payment, const std::string& idempotenceKey,
                            std::string& error) {
    const std::string id = payment.getString("provider_id");
    if (payment.getString("method") != "yookassa" || id.empty()) {
        error = "платёж не через ЮKassa";
        return false;
    }
    Account creds;
    if (!account(ownerId, creds)) {
        error = "ЮKassa не подключена";
        return false;
    }
    Json amount = Json::object();
    amount.set("value", Json(payments::amountText(payment.getInt("amount"))));
    amount.set("currency", Json(payment.getString("currency", "RUB")));
    Json body = Json::object();
    body.set("payment_id", Json(id));
    body.set("amount", amount);
    const net::HttpResponse response = call(creds, "POST", "/refunds", body.dump(), idempotenceKey, error);
    if (!response.ok()) return false;
    const Json answer = Json::parse(response.body);
    const std::string status = answer.isObject() ? answer.getString("status") : std::string();
    if (status == "succeeded" || status == "pending") return true;
    error = "ЮKassa отклонила возврат";
    SELFVOY_LOG(log::Level::Warn, "payments") << "ЮKassa: возврат по платежу " << id << " — статус «" << status << "»";
    return false;
}

}  // namespace selfvoy
