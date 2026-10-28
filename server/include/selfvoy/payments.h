// selfvoy/payments.h — оплата записи и подключение ЮKassa владельцем (F4).
//
// Владелец подключает свой магазин ЮKassa (payments.connect: shop_id + секретный
// ключ, проверка GET /me) — деньги гостей идут ему напрямую, Selfvoy их не
// касается. Секрет в БД — AES-256-GCM (Config::dataKey, AAD "selfvoy-payment-secret"),
// в логи не попадает. Без ЮKassa оплата идёт «по ссылке»: payment_url или
// реквизиты (payment_note) владельца, гость отмечает «Я оплатил(а)».
//
// Оплата — JSON, общий для записей (bookings.payment) и пакетов (F5):
//   {status: pending|claimed|paid|refunded|waived|expired, amount (мин. единицы),
//    currency, kind: full|deposit|package, method: link|yookassa, provider_id,
//    action_url, due_at, paid_at, refunded_at}; F10 (чек «Мой налог»): npd (владельцу
//    напомнили выдать чек), receipt_task_id, receipt_url, receipt_at, receipt_refund_task_id
// PaymentManager — общий механизм «создать платёж / проверить статус / вернуть»:
// begin() собирает JSON, submit() создаёт платёж у ЮKassa (Idempotence-Key от
// вызывающего: bk-<booking_id>, для пакетов — свой префикс), poll() спрашивает
// статус (не чаще minInterval на платёж), refund() — возврат всей суммы.
// Переходы статусов и блокировки — у владельца сущности (BookingManager).
//
// Чеки 54-ФЗ (объект receipt) сервер не отправляет: самозанятые формируют чек в
// «Мой налог», остальные — настройками магазина в ЮKassa.
#pragma once

#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "selfvoy/json.h"
#include "selfvoy/net.h"

namespace selfvoy {

class DatabaseManager;
struct Config;

namespace payments {

constexpr const char* kSecretAad = "selfvoy-payment-secret";
constexpr long long kMaxPrice = 100000000;  // мин. единицы (1 000 000,00)

// «2500.00» — сумма в минимальных единицах для API ЮKassa.
std::string amountText(long long minor);
// RUB | USD | EUR | KZT | BYN.
bool validCurrency(const std::string& currency);
// Статусы оплаты (pending … expired).
bool validStatus(const std::string& status);
// Оплата ещё ожидается (держит слот до due_at): pending.
inline bool awaiting(const Json& payment) { return payment.isObject() && payment.getString("status") == "pending"; }
// Гость заплатил или сказал, что заплатил: paid | claimed.
inline bool settled(const Json& payment) {
    const std::string status = payment.isObject() ? payment.getString("status") : std::string();
    return status == "paid" || status == "claimed";
}
// Вид для гостя: без provider_id и служебных полей чека (npd, receipt_task_id,
// receipt_refund_task_id).
Json guestView(const Json& payment);

}  // namespace payments

class PaymentManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    // HTTP-доступ к API ЮKassa (подменяется в тестах). url — полный адрес.
    class Transport {
    public:
        virtual ~Transport() = default;
        virtual net::HttpResponse request(const std::string& method, const std::string& url,
                                          const std::map<std::string, std::string>& headers,
                                          const std::string& body, int timeoutMs, std::string& error) = 0;
    };

    // Статус платежа у провайдера.
    enum class Remote { Throttled, Error, Pending, Succeeded, Canceled };

    static constexpr int kTimeoutMs = 10000;
    static constexpr int kPollIntervalSec = 30;  // планировщик: не чаще раза в 30 с на платёж
    static constexpr int kConnectPerHour = 10;

    PaymentManager(DatabaseManager& database, const Config& config, std::unique_ptr<Transport> transport = nullptr);

    void setTransport(std::unique_ptr<Transport> transport);

    // payments.status / payments.connect / payments.disconnect (владелец).
    Result status(long long userId);
    Result connect(long long userId, const Json& payload);
    Result disconnect(long long userId);

    // ЮKassa подключена и пригодна для оплаты (есть SELFVOY_PUBLIC_URL — адрес возврата гостя).
    bool connected(long long ownerId);

    // Новая оплата: method yookassa, если у владельца подключена ЮKassa и withProvider,
    // иначе link (action_url = linkUrl). status pending, due_at = dueAt.
    Json begin(long long ownerId, const std::string& kind, long long amount, const std::string& currency,
               std::time_t dueAt, const std::string& linkUrl, bool withProvider = true);
    // Для method yookassa: POST /payments (capture, redirect на returnUrl) → provider_id,
    // action_url. false — ошибка провайдера (payment не меняется), error — по-русски.
    bool submit(long long ownerId, Json& payment, const std::string& description, const std::string& returnUrl,
                const std::string& idempotenceKey, const Json& metadata, std::string& error);
    // GET /payments/<id> не чаще minIntervalSec на платёж (по now; 0 — без ограничения).
    Remote poll(long long ownerId, const Json& payment, std::time_t now, int minIntervalSec, std::string& error);
    // POST /refunds на всю сумму. true — возврат принят (succeeded или pending).
    bool refund(long long ownerId, const Json& payment, const std::string& idempotenceKey, std::string& error);

private:
    struct Account {
        std::string shopId;
        std::string secret;
    };
    bool account(long long ownerId, Account& out);
    net::HttpResponse call(const Account& account, const std::string& method, const std::string& path,
                           const std::string& body, const std::string& idempotenceKey, std::string& error);
    bool limited(const std::string& key, int perHour);

    DatabaseManager& database_;
    const Config& config_;
    std::mutex mutex_;
    std::shared_ptr<Transport> transport_;
    std::unordered_map<std::string, std::time_t> polled_;  // provider_id → последний опрос
    std::unordered_map<std::string, std::pair<std::time_t, int>> limits_;
};

}  // namespace selfvoy
