// selfvoy/tax.cpp — чек «Мой налог» для самозанятых (F10).
//
// Настройка владельца prefs.npd_receipts (Pro): «я самозанятый, напоминать выдать чек».
// Оплата записи или пакета стала paid (ЮKassa, отметка владельца, пакет «оплачен» при
// выдаче), валюта RUB — владельцу уведомление tax.receipt и задача «Выдать чек: <услуга>,
// <сумма>» со сроком «сегодня 23:59» по его поясу; в payment записи/пакета — npd: true и
// receipt_task_id (повторная отметка paid второй задачи не создаёт). tax.receiptDone
// сохраняет receipt_url / receipt_at в payment и закрывает задачу; гость видит ссылку
// «Чек». Возврат (refunded) оплаты с отмеченным чеком — tax.refund и задача
// «Аннулировать чек». Selfvoy сам чек не формирует: API «Мой налог» для партнёров
// требует отдельного договора, поэтому сервер готовит текст для копирования.
#include <algorithm>
#include <cstdlib>
#include <map>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"
#include "selfvoy/schedulelock.h"
#include "bookingutil.h"
#include "notificationsmanager.h"

namespace selfvoy {

using namespace bookingutil;

namespace {

constexpr const char* kNalogUrl = "https://lknpd.nalog.ru/";
constexpr int kTaxRemindBeforeSec = 3 * 3600;  // напоминание задачи — за 3 часа до конца дня

std::string receiptText(const std::string& service, const std::string& money) {
    return "Наименование: " + service + "; Сумма: " + money + "; Покупатель: физическое лицо";
}

std::string packageService(const PackagePurchaseRecord& record, const std::optional<BookingPageRecord>& page) {
    return (page ? page->title + ": " : std::string()) + "абонемент «" + record.title + "»";
}

// Первая активная встреча — подсказка пояса владельца (local), если AI-сервис не ответил.
Json firstOccurrence(const Json& occurrences) {
    for (const auto& occurrence : occurrences.items()) {
        if (occurrence.getString("status") == "active") return occurrence;
    }
    return occurrences.size() > 0 ? occurrences.at(0) : Json();
}

// Сдвиг местного времени по полю local встречи (проверено при записи); false — неизвестен.
bool offsetOf(const Json& occurrence, long long& offset) {
    std::time_t start = 0;
    std::time_t asUtc = 0;
    const std::string local = occurrence.getString("local");
    if (!validLocal(local) || !parseIsoUtc(occurrence.getString("start"), start) ||
        !parseIsoUtc(local + ":00Z", asUtc)) {
        return false;
    }
    offset = static_cast<long long>(asUtc - start);
    return std::llabs(offset) <= 14 * 3600 && offset % 900 == 0;
}

}  // namespace

std::time_t BookingManager::ownerDayEnd(long long ownerId, std::time_t now, const Json& hint) {
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(ownerId, ownerName, ownerTimezone);
    long long offset = 0;
    bool known = false;
    // Пояс знает AI-сервис (zoneinfo): местное время текущей минуты через /v1/agent/series.
    const std::time_t minute = now - now % 60;
    Json request = Json::object();
    request.set("timezone", Json(ownerTimezone));
    request.set("start", Json(isoUtc(minute)));
    request.set("duration_minutes", Json(15LL));
    request.set("count", Json(1LL));
    request.set("interval_days", Json(7LL));
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;
    std::string url = config_.aiServiceUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", url + "/v1/agent/series", headers, request.dump(), config_.aiTimeoutMs, error);
    if (response.ok()) {
        const Json answer = Json::parse(response.body);
        const Json first = answer.get("occurrences").at(0);
        std::time_t start = 0;
        known = parseIsoUtc(first.getString("start"), start) && start == minute && offsetOf(first, offset);
    }
    if (!known) known = hint.isObject() && offsetOf(hint, offset);
    if (!known) offset = 0;  // пояс неизвестен — конец дня по UTC
    const long long local = static_cast<long long>(now) + offset;
    const long long dayStart = local - (((local % 86400) + 86400) % 86400);
    return static_cast<std::time_t>(dayStart + 86400 - 60 - offset);
}

// ------------------------------------------------------------ оплата получена

void BookingManager::taxReceiptForBooking(const BookingRecord& booking) {
    const Json& payment = booking.payment;
    if (!payment.isObject() || payment.getString("status") != "paid" || payment.getString("currency", "RUB") != "RUB" ||
        payment.getBool("npd") || payment.getInt("amount") <= 0) {
        return;
    }
    if (!database_.db().getPreferences(booking.ownerId).getBool("npd_receipts")) return;
    const auto page = database_.db().findBookingPage(booking.pageId);
    const std::string service = page ? page->title : std::string("Запись");
    const std::string money = moneyOf(payment);
    const std::string text = receiptText(service, money);
    const std::time_t now = std::time(nullptr);
    const std::time_t due = ownerDayEnd(booking.ownerId, now, firstOccurrence(booking.occurrences));
    TaskRecord task;
    task.userId = booking.ownerId;
    task.title = "Выдать чек: " + service + ", " + money;
    task.notes = text + "\nКлиент: " + booking.guestName + "\n" + kNalogUrl;
    task.dueAt = isoUtc(due);
    task.remindAt = isoUtc(std::max(now, due - kTaxRemindBeforeSec));
    long long taskId = 0;
    if (!database_.db().createTask(task, taskId).ok) taskId = 0;
    std::optional<BookingRecord> updated;
    const bool marked = mutateBooking(booking.id, [&](BookingRecord& record) {
        if (!record.payment.isObject() || record.payment.getBool("npd") ||
            record.payment.getString("status") != "paid") {
            return false;  // уже напомнили (другой путь отметки) или оплата изменилась
        }
        Json stored = record.payment;
        stored.set("npd", Json(true));
        stored.set("receipt_task_id", Json(taskId));
        record.payment = stored;
        return true;
    }, updated);
    if (!marked) {
        deleteOwnTask(booking.ownerId, taskId);
        return;
    }
    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(booking.id));
        notify.set("page_id", Json(booking.pageId));
        notify.set("service", Json(service));
        notify.set("amount", Json(payment.getInt("amount")));
        notify.set("currency", Json(std::string("RUB")));
        notify.set("client", Json(booking.guestName));
        notify.set("paid_at", Json(payment.getString("paid_at")));
        notify.set("text", Json(text));
        notify.set("url", Json(std::string(kNalogUrl)));
        notify.set("task_id", Json(taskId));
        notifications_->create(booking.ownerId, "tax.receipt", "Выдайте чек «Мой налог»: " + booking.guestName,
                               text, notify);
    }
}

void BookingManager::taxReceiptForPackage(const PackagePurchaseRecord& record) {
    const Json& payment = record.payment;
    if (!payment.isObject() || payment.getString("status") != "paid" || payment.getString("currency", "RUB") != "RUB" ||
        payment.getBool("npd") || payment.getInt("amount") <= 0) {
        return;
    }
    if (!database_.db().getPreferences(record.ownerId).getBool("npd_receipts")) return;
    const auto page = database_.db().findBookingPage(record.pageId);
    const std::string service = packageService(record, page);
    const std::string money = moneyOf(payment);
    const std::string text = receiptText(service, money);
    const std::time_t now = std::time(nullptr);
    Json hint;
    for (const auto& booking : database_.db().listBookingsByPackage(record.id, 1)) hint = firstOccurrence(booking.occurrences);
    const std::time_t due = ownerDayEnd(record.ownerId, now, hint);
    TaskRecord task;
    task.userId = record.ownerId;
    task.title = "Выдать чек: " + service + ", " + money;
    task.notes = text + "\nКлиент: " + record.guestName + "\n" + kNalogUrl;
    task.dueAt = isoUtc(due);
    task.remindAt = isoUtc(std::max(now, due - kTaxRemindBeforeSec));
    long long taskId = 0;
    if (!database_.db().createTask(task, taskId).ok) taskId = 0;
    std::optional<PackagePurchaseRecord> updated;
    const bool marked = mutatePackage(record.id, [&](PackagePurchaseRecord& item) {
        if (!item.payment.isObject() || item.payment.getBool("npd") || item.payment.getString("status") != "paid") {
            return false;
        }
        Json stored = item.payment;
        stored.set("npd", Json(true));
        stored.set("receipt_task_id", Json(taskId));
        item.payment = stored;
        return true;
    }, updated);
    if (!marked) {
        deleteOwnTask(record.ownerId, taskId);
        return;
    }
    if (notifications_) {
        Json notify = Json::object();
        notify.set("package_id", Json(record.id));
        notify.set("page_id", Json(record.pageId));
        notify.set("service", Json(service));
        notify.set("amount", Json(payment.getInt("amount")));
        notify.set("currency", Json(std::string("RUB")));
        notify.set("client", Json(record.guestName));
        notify.set("paid_at", Json(payment.getString("paid_at")));
        notify.set("text", Json(text));
        notify.set("url", Json(std::string(kNalogUrl)));
        notify.set("task_id", Json(taskId));
        notifications_->create(record.ownerId, "tax.receipt", "Выдайте чек «Мой налог»: " + record.guestName, text,
                               notify);
    }
}

// ------------------------------------------------------------------ возврат

void BookingManager::taxRefundForBooking(const BookingRecord& booking) {
    const Json& payment = booking.payment;
    if (!payment.isObject() || payment.getString("status") != "refunded" || payment.getString("receipt_at").empty() ||
        payment.getInt("receipt_refund_task_id") > 0) {
        return;
    }
    const auto page = database_.db().findBookingPage(booking.pageId);
    const std::string service = page ? page->title : std::string("Запись");
    const std::string money = moneyOf(payment);
    const std::time_t now = std::time(nullptr);
    TaskRecord task;
    task.userId = booking.ownerId;
    task.title = "Аннулировать чек: " + service + ", " + money;
    task.notes = "Оплата возвращена гостю (" + booking.guestName + ") — аннулируйте чек в «Мой налог»." +
                 (payment.getString("receipt_url").empty() ? std::string() : "\nЧек: " + payment.getString("receipt_url")) +
                 "\n" + kNalogUrl;
    task.dueAt = isoUtc(ownerDayEnd(booking.ownerId, now, firstOccurrence(booking.occurrences)));
    task.remindAt = isoUtc(now);
    long long taskId = 0;
    if (!database_.db().createTask(task, taskId).ok || taskId <= 0) return;
    std::optional<BookingRecord> updated;
    const bool marked = mutateBooking(booking.id, [&](BookingRecord& record) {
        if (!record.payment.isObject() || record.payment.getInt("receipt_refund_task_id") > 0) return false;
        Json stored = record.payment;
        stored.set("receipt_refund_task_id", Json(taskId));
        record.payment = stored;
        return true;
    }, updated);
    if (!marked) {
        deleteOwnTask(booking.ownerId, taskId);
        return;
    }
    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(booking.id));
        notify.set("page_id", Json(booking.pageId));
        notify.set("service", Json(service));
        notify.set("amount", Json(payment.getInt("amount")));
        notify.set("currency", Json(payment.getString("currency", "RUB")));
        notify.set("client", Json(booking.guestName));
        notify.set("receipt_url", Json(payment.getString("receipt_url")));
        notify.set("refunded_at", Json(payment.getString("refunded_at")));
        notify.set("url", Json(std::string(kNalogUrl)));
        notify.set("task_id", Json(taskId));
        notifications_->create(booking.ownerId, "tax.refund", "Аннулируйте чек в «Мой налог»",
                               service + ", " + money + " — оплата возвращена (" + booking.guestName + ")", notify);
    }
}

void BookingManager::taxRefundForPackage(const PackagePurchaseRecord& record) {
    const Json& payment = record.payment;
    if (!payment.isObject() || payment.getString("status") != "refunded" || payment.getString("receipt_at").empty() ||
        payment.getInt("receipt_refund_task_id") > 0) {
        return;
    }
    const auto page = database_.db().findBookingPage(record.pageId);
    const std::string service = packageService(record, page);
    const std::string money = moneyOf(payment);
    const std::time_t now = std::time(nullptr);
    TaskRecord task;
    task.userId = record.ownerId;
    task.title = "Аннулировать чек: " + service + ", " + money;
    task.notes = "Оплата возвращена гостю (" + record.guestName + ") — аннулируйте чек в «Мой налог»." +
                 (payment.getString("receipt_url").empty() ? std::string() : "\nЧек: " + payment.getString("receipt_url")) +
                 "\n" + kNalogUrl;
    task.dueAt = isoUtc(ownerDayEnd(record.ownerId, now, Json()));
    task.remindAt = isoUtc(now);
    long long taskId = 0;
    if (!database_.db().createTask(task, taskId).ok || taskId <= 0) return;
    std::optional<PackagePurchaseRecord> updated;
    const bool marked = mutatePackage(record.id, [&](PackagePurchaseRecord& item) {
        if (!item.payment.isObject() || item.payment.getInt("receipt_refund_task_id") > 0) return false;
        Json stored = item.payment;
        stored.set("receipt_refund_task_id", Json(taskId));
        item.payment = stored;
        return true;
    }, updated);
    if (!marked) {
        deleteOwnTask(record.ownerId, taskId);
        return;
    }
    if (notifications_) {
        Json notify = Json::object();
        notify.set("package_id", Json(record.id));
        notify.set("page_id", Json(record.pageId));
        notify.set("service", Json(service));
        notify.set("amount", Json(payment.getInt("amount")));
        notify.set("currency", Json(payment.getString("currency", "RUB")));
        notify.set("client", Json(record.guestName));
        notify.set("receipt_url", Json(payment.getString("receipt_url")));
        notify.set("refunded_at", Json(payment.getString("refunded_at")));
        notify.set("url", Json(std::string(kNalogUrl)));
        notify.set("task_id", Json(taskId));
        notifications_->create(record.ownerId, "tax.refund", "Аннулируйте чек в «Мой налог»",
                               service + ", " + money + " — оплата возвращена (" + record.guestName + ")", notify);
    }
}

// ------------------------------------------------------------------ владелец

BookingManager::Result BookingManager::taxPending(long long ownerId, const Json& payload) {
    long long limit = 50;
    if (!intField(payload, "limit", 1, kTaxPendingMax, limit)) {
        return Result::failure(protocol::code::kBadRequest, "limit — от 1 до 200");
    }
    struct Item {
        std::string paidAt;
        long long id = 0;
        Json json;
    };
    std::vector<Item> items;
    std::map<long long, std::optional<BookingPageRecord>> pages;
    const auto pageOf = [&](long long pageId) -> const std::optional<BookingPageRecord>& {
        auto it = pages.find(pageId);
        if (it == pages.end()) it = pages.emplace(pageId, database_.db().findBookingPage(pageId)).first;
        return it->second;
    };
    const auto item = [](const std::string& kind, long long id, const std::string& service, const Json& payment,
                         const std::string& client) {
        Json json = Json::object();
        json.set("kind", Json(kind));
        json.set("id", Json(id));
        json.set("service", Json(service));
        json.set("amount", Json(payment.getInt("amount")));
        json.set("currency", Json(payment.getString("currency", "RUB")));
        json.set("client", Json(client));
        json.set("paid_at", Json(payment.getString("paid_at")));
        json.set("text", Json(receiptText(service, moneyOf(payment))));
        json.set("url", Json(std::string(kNalogUrl)));
        return json;
    };
    for (const auto& booking : database_.db().listReceiptPendingBookings(ownerId, static_cast<int>(limit))) {
        const auto& page = pageOf(booking.pageId);
        items.push_back({booking.payment.getString("paid_at"), booking.id,
                         item("booking", booking.id, page ? page->title : std::string("Запись"), booking.payment,
                              booking.guestName)});
    }
    for (const auto& record : database_.db().listReceiptPendingPackages(ownerId, static_cast<int>(limit))) {
        items.push_back({record.payment.getString("paid_at"), record.id,
                         item("package", record.id, packageService(record, pageOf(record.pageId)), record.payment,
                              record.guestName)});
    }
    // Новые сверху (ISO UTC одного формата сравнивается как строка).
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.paidAt > b.paidAt; });
    if (items.size() > static_cast<std::size_t>(limit)) items.resize(static_cast<std::size_t>(limit));
    Json list = Json::array();
    for (const auto& entry : items) list.push(entry.json);
    Result result;
    result.ok = true;
    result.payload.set("items", list);
    return result;
}

BookingManager::Result BookingManager::taxReceiptDone(long long ownerId, const Json& payload) {
    const bool byBooking = payload.contains("booking_id") && !payload.get("booking_id").isNull();
    const bool byPackage = payload.contains("package_id") && !payload.get("package_id").isNull();
    if (byBooking == byPackage) {
        return Result::failure(protocol::code::kBadRequest, "укажите booking_id или package_id");
    }
    std::string receiptUrl;
    if (payload.contains("receipt_url") && !payload.get("receipt_url").isNull()) {
        if (!payload.get("receipt_url").isString()) {
            return Result::failure(protocol::code::kBadRequest, "receipt_url — ссылка https:// не длиннее 500 символов");
        }
        receiptUrl = trim(payload.getString("receipt_url"));
        if (!receiptUrl.empty() && !validHttpsUrl(receiptUrl)) {
            return Result::failure(protocol::code::kBadRequest, "receipt_url — ссылка https:// не длиннее 500 символов");
        }
    }
    const std::string at = isoUtc(std::time(nullptr));
    Result failure;
    long long taskId = 0;
    const auto mark = [&](Json& payment) {
        if (!payment.isObject()) {
            failure = Result::failure(protocol::code::kBadRequest, "оплаты нет — чек не нужен");
            return false;
        }
        if (payment.getString("status") != "paid") {
            failure = Result::failure(protocol::code::kConflict, "чек — только для полученной оплаты (paid)");
            return false;
        }
        taskId = payment.getInt("receipt_task_id");
        Json stored = payment;
        stored.set("receipt_url", Json(receiptUrl));
        stored.set("receipt_at", Json(at));
        payment = stored;
        return true;
    };
    Result result;
    result.ok = true;
    if (byBooking) {
        const long long id = payload.getInt("booking_id");
        const auto found = id > 0 ? database_.db().findBooking(id) : std::nullopt;
        if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "запись не найдена");
        std::optional<BookingRecord> booking;
        const bool changed = mutateBooking(id, [&](BookingRecord& record) { return mark(record.payment); }, booking);
        if (!failure.code.empty()) return failure;
        if (!changed || !booking) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
        const auto page = database_.db().findBookingPage(booking->pageId);
        result.payload.set("booking", bookingJson(*booking, page ? page->title : std::string(),
                                                  page ? page->rebookAfterDays : 0));
    } else {
        const long long id = payload.getInt("package_id");
        const auto found = id > 0 ? database_.db().findPackagePurchase(id) : std::nullopt;
        if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
        std::optional<PackagePurchaseRecord> record;
        const bool changed = mutatePackage(id, [&](PackagePurchaseRecord& item) { return mark(item.payment); }, record);
        if (!failure.code.empty()) return failure;
        if (!changed || !record) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
        const auto page = database_.db().findBookingPage(record->pageId);
        result.payload.set("package", packageOwnerJson(*record, page ? page->title : std::string()));
    }
    // Задача «Выдать чек» закрыта.
    if (taskId > 0) {
        const auto task = database_.db().findTask(taskId);
        if (task && task->userId == ownerId && task->status == "pending") database_.db().setTaskStatus(taskId, "done");
    }
    return result;
}

}  // namespace selfvoy
