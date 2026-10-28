// selfvoy/packages.cpp — пакеты (абонементы) и второй получатель извещений гостя (F5).
//
// Предложения пакетов — поле страницы записи packages (≤ 5, Pro). Гость покупает
// пакет при записи (booking.book с package_id) — оплата как у записи (F4: ЮKassa
// владельца, ссылка/реквизиты; без способа оплаты пакет сразу активен, оплату
// отмечает владелец) — или получает его от владельца (package.issue). Дальше гость
// записывается по ссылке пакета #/p/<токен> (booking.book с package_token): каждая
// активная встреча — одно занятие, отмена не позже cancel_hours возвращает занятие,
// поздняя — нет. Все изменения пакета — под блокировкой расписания владельца
// (как записи), поэтому списание и возврат не теряются при гонках.
#include <algorithm>
#include <map>
#include <set>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc / parseIsoUtc
#include "selfvoy/payments.h"
#include "selfvoy/plan.h"
#include "selfvoy/protocol.h"
#include "selfvoy/referralmanager.h"
#include "selfvoy/schedulelock.h"
#include "selfvoy/session.h"
#include "selfvoy/slotspost.h"
#include "selfvoy/telegrambot.h"
#include "bookingutil.h"
#include "notificationsmanager.h"

namespace selfvoy {

using namespace bookingutil;

namespace {

constexpr std::size_t kTokenBytes = 32;
constexpr int kPackageGetPerMinute = 30;
constexpr int kPackageChangePerMinute = 10;
constexpr int kIssuePerHour = 60;
constexpr int kPaymentGraceSec = 3600;

std::string newToken() {
    std::string token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
    while (!token.empty() && token.back() == '=') token.pop_back();
    return token;
}

int leftOf(const PackagePurchaseRecord& record) { return std::max(0, record.sessionsTotal - record.sessionsUsed); }

// 1 занятие, 2 занятия, 5 занятий.
std::string sessionsText(long long count) {
    const long long tail = count % 100;
    const long long last = count % 10;
    const char* word = (tail >= 11 && tail <= 14) ? "занятий"
                       : last == 1                 ? "занятие"
                       : (last >= 2 && last <= 4)  ? "занятия"
                                                   : "занятий";
    return std::to_string(count) + " " + word;
}

std::string daysText(long long count) {
    const long long tail = count % 100;
    const long long last = count % 10;
    const char* word = (tail >= 11 && tail <= 14) ? "дней" : last == 1 ? "день" : (last >= 2 && last <= 4) ? "дня" : "дней";
    return std::to_string(count) + " " + word;
}

// «16 января 2027» из ISO UTC.
std::string dateText(const std::string& iso) {
    static const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                          "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
    std::time_t at = 0;
    if (!parseIsoUtc(iso, at)) return iso;
    std::tm parts{};
    gmtime_r(&at, &parts);
    return std::to_string(parts.tm_mday) + " " + kMonths[parts.tm_mon] + " " + std::to_string(parts.tm_year + 1900);
}

// Активация: срок действия — с этого момента; занятий не осталось — сразу used.
void activate(PackagePurchaseRecord& record, std::time_t now) {
    record.validUntil = isoUtc(now + static_cast<std::time_t>(record.validDays) * 86400);
    record.status = leftOf(record) > 0 ? "active" : "used";
}

// Статус действующего пакета по остатку и сроку (active ↔ used, истёк — expired);
// pending / expired / cancelled не меняются.
void settle(PackagePurchaseRecord& record, std::time_t now) {
    if (record.status != "active" && record.status != "used") return;
    if (leftOf(record) == 0) {
        record.status = "used";
        return;
    }
    std::time_t until = 0;
    record.status = parseIsoUtc(record.validUntil, until) && until <= now ? "expired" : "active";
}

// Оплата, которую отмечает владелец: пакет без способа оплаты на странице или выданный
// владельцем (package.issue). status — pending | paid.
Json manualPayment(long long amount, const std::string& currency, const std::string& status, std::time_t now) {
    Json payment = Json::object();
    payment.set("status", Json(status));
    payment.set("amount", Json(amount));
    payment.set("currency", Json(currency));
    payment.set("kind", Json(std::string("package")));
    payment.set("method", Json(std::string("link")));
    payment.set("provider_id", Json(std::string()));
    payment.set("action_url", Json(std::string()));
    payment.set("due_at", Json(std::string()));
    payment.set("paid_at", Json(status == "paid" ? isoUtc(now) : std::string()));
    payment.set("refunded_at", Json(std::string()));
    return payment;
}

bool validPackageStatus(const std::string& status) {
    return status == "pending" || status == "active" || status == "used" || status == "expired" ||
           status == "cancelled";
}

}  // namespace

// ------------------------------------------------------------------ общее

std::string BookingManager::maskEmail(const std::string& email) {
    if (email.empty()) return "";
    const std::size_t at = email.find('@');
    if (at == std::string::npos || at == 0) return "***";
    const unsigned char first = static_cast<unsigned char>(email[0]);
    const std::size_t width = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
    return email.substr(0, std::min(width, at)) + "***" + email.substr(at);
}

std::string BookingManager::packageUrl(const std::string& token) const {
    return config_.publicUrl.empty() || token.empty() ? std::string() : config_.publicUrl + "/#/p/" + token;
}

std::string BookingManager::packageTokenOf(const PackagePurchaseRecord& record) const {
    std::string token;
    if (record.tokenEnc.empty() || !crypto::openField(config_.dataKey(), record.tokenEnc, kPackageTokenAad, token)) {
        return "";
    }
    return tokenHash(token) == record.tokenHash ? token : std::string();
}

Json BookingManager::packageNotice(const PackagePurchaseRecord& record, const Json& extra) const {
    Json notify = Json::object();
    notify.set("package_id", Json(record.id));
    notify.set("page_id", Json(record.pageId));
    notify.set("guest_name", Json(record.guestName));
    if (extra.isObject()) {
        for (const auto& [key, value] : extra.members()) notify.set(key, value);
    }
    return notify;
}

Json BookingManager::packageOwnerJson(const PackagePurchaseRecord& record, const std::string& pageTitle) const {
    Json json = Json::object();
    json.set("id", Json(record.id));
    json.set("page_id", Json(record.pageId));
    json.set("page_title", Json(pageTitle));
    json.set("offer_id", Json(record.offerId));
    json.set("title", Json(record.title));
    json.set("sessions_total", Json(static_cast<long long>(record.sessionsTotal)));
    json.set("sessions_used", Json(static_cast<long long>(record.sessionsUsed)));
    json.set("sessions_left", Json(static_cast<long long>(leftOf(record))));
    json.set("valid_days", Json(static_cast<long long>(record.validDays)));
    json.set("valid_until", Json(record.validUntil));
    json.set("price", Json(record.price));
    json.set("currency", Json(record.currency));
    json.set("status", Json(record.status));
    json.set("payment", record.payment.isObject() ? record.payment : Json());
    json.set("guest_name", Json(record.guestName));
    json.set("guest_email", Json(record.guestEmail));
    json.set("cc_email", Json(record.ccEmail));
    json.set("created_at", Json(record.createdAt));
    json.set("updated_at", Json(record.updatedAt));
    return json;
}

Json BookingManager::packageGuestJson(const PackagePurchaseRecord& record, const BookingPageRecord& page) {
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    // Встречи по пакету: [{start, status}] по времени (отменённая запись — все её встречи cancelled).
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& booking : database_.db().listBookingsByPackage(record.id, 500)) {
        for (const auto& occurrence : booking.occurrences.items()) {
            const std::string status = booking.status == "active" ? occurrence.getString("status") : "cancelled";
            items.emplace_back(occurrence.getString("start"), status);
        }
    }
    std::sort(items.begin(), items.end());
    Json bookings = Json::array();
    for (const auto& [start, status] : items) {
        Json item = Json::object();
        item.set("start", Json(start));
        item.set("status", Json(status));
        bookings.push(item);
    }
    Json json = Json::object();
    json.set("title", Json(record.title));
    json.set("sessions_total", Json(static_cast<long long>(record.sessionsTotal)));
    json.set("sessions_left", Json(static_cast<long long>(leftOf(record))));
    json.set("valid_until", Json(record.validUntil));
    json.set("valid_days", Json(static_cast<long long>(record.validDays)));
    json.set("status", Json(record.status));
    json.set("price", Json(record.price));
    json.set("currency", Json(record.currency));
    json.set("payment", payments::guestView(record.payment));
    // F10: ссылка на чек «Мой налог», если специалист её указал ("" — нет).
    json.set("receipt_url", Json(record.payment.isObject() ? record.payment.getString("receipt_url") : std::string()));
    json.set("payment_note", Json(page.paymentNote));
    json.set("guest_name", Json(record.guestName));
    json.set("cc_email", Json(maskEmail(record.ccEmail)));
    json.set("page_slug", Json(page.status == "active" ? page.slug : std::string()));
    json.set("page_title", Json(page.title));
    json.set("owner_name", Json(ownerName));
    json.set("owner_timezone", Json(ownerTimezone));
    json.set("bookings", bookings);
    json.set("branding", branding(page));
    json.set("ref", Json(referrals_.codeFor(page.ownerId)));  // как booking.get / invite.get
    return json;
}

std::vector<std::string> BookingManager::packageLines(const PackagePurchaseRecord& record,
                                                      const BookingPageRecord& page, const std::string& token,
                                                      std::time_t now) const {
    std::vector<std::string> lines;
    const std::string money = slotspost::moneyLabel(record.price, record.currency);
    lines.push_back("Абонемент «" + record.title + "»: " + sessionsText(record.sessionsTotal) +
                    (record.price > 0 ? ", " + money : std::string()) + ".");
    if (record.status == "pending" || record.validUntil.empty()) {
        lines.push_back("Срок действия — " + daysText(record.validDays) + " с момента оплаты.");
    } else {
        lines.push_back("Действует до " + dateText(record.validUntil) + ", осталось " + sessionsText(leftOf(record)) + ".");
    }
    const std::string url = packageUrl(token);
    if (!url.empty()) lines.push_back("Ссылка абонемента (запись по пакету и остаток занятий): " + url);
    const Json& payment = record.payment;
    if (!payments::awaiting(payment)) return lines;
    if (record.status == "pending") {
        std::time_t due = 0;
        long long minutes = page.payWithinMinutes;
        if (parseIsoUtc(payment.getString("due_at"), due)) minutes = std::max<long long>(1, (due - now + 59) / 60);
        lines.push_back("Оплата пакета: " + moneyOf(payment) + " — оплатите в течение " +
                        slotspost::durationLabel(static_cast<int>(minutes)) + ", иначе покупка и запись отменятся.");
        const std::string action = payment.getString("action_url");
        if (!action.empty()) lines.push_back("Оплатить: " + action);
        if (payment.getString("method") == "link") {
            if (!page.paymentNote.empty()) lines.push_back("Как оплатить: " + page.paymentNote);
            lines.push_back("После оплаты нажмите «Я оплатил(а)» на странице абонемента.");
        }
    } else {
        lines.push_back("Оплату пакета (" + moneyOf(payment) + ") примет организатор." +
                        (page.paymentNote.empty() ? std::string() : " Реквизиты: " + page.paymentNote));
    }
    return lines;
}

// ------------------------------------------------------------ предложения

BookingManager::Result BookingManager::applyPackageOffers(long long ownerId, const Json& payload, bool creating,
                                                          BookingPageRecord& page) {
    if (!payload.contains("packages")) return Result{true, "", "", Json::object()};
    const Json list = payload.get("packages");
    if (!list.isArray()) {
        return Result::failure(protocol::code::kBadRequest, "packages — массив предложений пакетов");
    }
    if (list.size() > static_cast<std::size_t>(kMaxPackageOffers)) {
        return Result::failure(protocol::code::kBadRequest, "не больше 5 пакетов на странице");
    }
    const Json previous = creating || !page.packages.isArray() ? Json::array() : page.packages;
    long long nextId = 0;
    std::set<long long> known;
    for (const auto& offer : previous.items()) {
        known.insert(offer.getInt("id"));
        nextId = std::max(nextId, offer.getInt("id"));
    }
    std::set<long long> seen;
    Json offers = Json::array();
    for (const auto& item : list.items()) {
        if (!item.isObject()) return Result::failure(protocol::code::kBadRequest, "пакет — объект {title, sessions, price}");
        long long id = 0;
        if (item.contains("id") && !item.get("id").isNull()) {
            if (!intField(item, "id", 1, 1000000000, id) || known.count(id) == 0) {
                return Result::failure(protocol::code::kBadRequest, "неизвестный id пакета — новый пакет передавайте без id");
            }
            if (!seen.insert(id).second) return Result::failure(protocol::code::kBadRequest, "id пакетов повторяются");
        }
        const std::string title = item.get("title").isString() ? trim(item.getString("title")) : std::string();
        if (title.empty() || !validText(title, 80, false)) {
            return Result::failure(protocol::code::kBadRequest, "название пакета — от 1 до 80 символов в одну строку");
        }
        long long sessions = 0;
        long long price = 0;
        long long validDays = 90;
        if (!item.contains("sessions") || !intField(item, "sessions", 2, 100, sessions)) {
            return Result::failure(protocol::code::kBadRequest, "занятий в пакете — от 2 до 100");
        }
        if (!item.contains("price") || !intField(item, "price", 1, payments::kMaxPrice, price)) {
            return Result::failure(protocol::code::kBadRequest,
                                   "цена пакета — целое число в минимальных единицах (копейках), больше 0");
        }
        if (!intField(item, "valid_days", 7, 365, validDays)) {
            return Result::failure(protocol::code::kBadRequest, "срок действия пакета — от 7 до 365 дней");
        }
        Json offer = Json::object();
        offer.set("id", Json(id));  // новые — ниже, после проверки всех
        offer.set("title", Json(title));
        offer.set("sessions", Json(sessions));
        offer.set("price", Json(price));
        offer.set("valid_days", Json(validDays));
        offers.push(offer);
    }
    // Новые предложения — id больше всех прежних (покупки хранят offer_id).
    for (auto& offer : offers.items()) {
        if (offer.getInt("id") == 0) offer.set("id", Json(++nextId));
    }
    // Пакеты — Pro (как скрытие подписи: включить без Pro нельзя, править уже включённые — можно).
    if (!offers.empty() && previous.empty() && !plan::proAllowed(database_.db(), ownerId)) {
        return Result::failure(protocol::code::kProRequired, "пакеты и абонементы доступны на тарифе Pro");
    }
    page.packages = offers;
    return Result{true, "", "", Json::object()};
}

// ------------------------------------------------------------ запись по пакету

BookingManager::Result BookingManager::packageUsable(const PackagePurchaseRecord& record, long long pageId,
                                                     long long count, std::time_t now) {
    if (record.pageId != pageId) {
        return Result::failure(protocol::code::kBadRequest, "этот абонемент — для другой страницы записи");
    }
    if (record.status == "pending") return Result::failure(protocol::code::kConflict, "абонемент ещё не оплачен");
    if (record.status == "cancelled") return Result::failure(protocol::code::kConflict, "абонемент отменён");
    std::time_t until = 0;
    if (record.status == "expired" || (parseIsoUtc(record.validUntil, until) && until <= now)) {
        return Result::failure(protocol::code::kConflict, "срок действия абонемента истёк");
    }
    const int left = leftOf(record);
    if (left == 0) return Result::failure(protocol::code::kConflict, "занятия по абонементу закончились");
    if (left < count) {
        return Result::failure(protocol::code::kConflict, "на абонементе осталось " + sessionsText(left));
    }
    return Result{true, "", "", Json::object()};
}

BookingManager::Result BookingManager::packageIntent(const BookingPageRecord& page, const Json& payload,
                                                     long long count, PackageIntent& out) {
    const bool buying = payload.contains("package_id") && !payload.get("package_id").isNull();
    const std::string token = payload.getString("package_token");
    if (buying && !token.empty()) {
        return Result::failure(protocol::code::kBadRequest,
                               "укажите либо package_id (купить пакет), либо package_token (запись по пакету)");
    }
    if (buying) {
        long long offerId = 0;
        if (!intField(payload, "package_id", 1, 1000000000, offerId)) {
            return Result::failure(protocol::code::kBadRequest, "package_id — id пакета страницы");
        }
        const Json offers = page.packages;
        for (const auto& offer : offers.items()) {
            if (offer.getInt("id") == offerId) {
                out.offerId = offerId;
                out.offer = offer;
            }
        }
        if (out.offerId == 0) return Result::failure(protocol::code::kBadRequest, "на странице нет такого пакета");
        if (count > out.offer.getInt("sessions")) {
            return Result::failure(protocol::code::kBadRequest,
                                   "в пакете " + sessionsText(out.offer.getInt("sessions")) + " — выберите меньше встреч");
        }
        return Result{true, "", "", Json::object()};
    }
    if (!token.empty()) {
        const std::string hash = tokenHash(token);
        const auto record = hash.empty() ? std::nullopt : database_.db().findPackagePurchaseByToken(hash);
        if (!record) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
        const Result usable = packageUsable(*record, page.id, count, std::time(nullptr));
        if (!usable.ok) return usable;
        out.packageId = record->id;
    }
    return Result{true, "", "", Json::object()};
}

BookingManager::Result BookingManager::createPurchase(const BookingPageRecord& page, const Json& offer,
                                                      const BookingRecord& guest, int used, std::time_t firstStart,
                                                      PackagePurchaseRecord& out, std::string& token) {
    const std::time_t now = std::time(nullptr);
    PackagePurchaseRecord record;
    record.ownerId = page.ownerId;
    record.pageId = page.id;
    record.offerId = offer.getInt("id");
    record.title = offer.getString("title");
    record.sessionsTotal = static_cast<int>(offer.getInt("sessions"));
    record.sessionsUsed = used;
    record.validDays = static_cast<int>(offer.getInt("valid_days", 90));
    record.price = offer.getInt("price");
    record.currency = page.currency;
    record.guestName = guest.guestName;
    record.guestEmail = guest.guestEmail;
    record.ccEmail = guest.ccEmail;
    // Оплата — как у записи (F4): ЮKassa владельца или ссылка/реквизиты страницы, срок —
    // pay_within_minutes (не позже первой встречи). Без способа оплаты пакет сразу
    // активен, оплату отмечает владелец (package.paymentSet).
    const std::time_t due = std::min(now + static_cast<std::time_t>(page.payWithinMinutes) * 60, firstStart);
    Json payment = payments_ ? payments_->begin(page.ownerId, "package", record.price, record.currency, due,
                                                page.paymentUrl)
                             : Json();
    const bool gated = payment.isObject() && (payment.getString("method") == "yookassa" || !page.paymentUrl.empty() ||
                                              !page.paymentNote.empty());
    if (gated) {
        record.status = "pending";
        record.payment = payment;
    } else {
        record.payment = manualPayment(record.price, record.currency, "pending", now);
        activate(record, now);
    }
    long long id = 0;
    for (int attempt = 0; attempt < 3 && id == 0; ++attempt) {
        token = newToken();
        record.tokenHash = tokenHash(token);
        if (record.tokenHash.empty()) continue;
        record.tokenEnc = crypto::sealField(config_.dataKey(), token, kPackageTokenAad);
        const DatabaseError saved = database_.db().createPackagePurchase(record, id);
        if (!saved.ok) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "пакет не сохранён: " << saved.message;
            id = 0;
        }
    }
    if (id == 0) return Result::failure(protocol::code::kInternal, "не удалось сохранить покупку пакета");
    record.id = id;
    out = record;
    return Result{true, "", "", Json::object()};
}

bool BookingManager::mutatePackage(long long packageId, const std::function<bool(PackagePurchaseRecord&)>& change,
                                   std::optional<PackagePurchaseRecord>& out) {
    const auto first = packageId > 0 ? database_.db().findPackagePurchase(packageId) : std::nullopt;
    if (!first) return false;
    auto lock = schedule_.acquire(first->ownerId);
    if (!lock.ok()) return false;
    auto record = database_.db().findPackagePurchase(packageId);  // перечитываем под блокировкой
    if (!record || !change(*record)) return false;
    if (!database_.db().updatePackagePurchase(*record).ok) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "пакет " << packageId << " не сохранён";
        return false;
    }
    out = std::move(record);
    return true;
}

bool BookingManager::shiftPackageUsedLocked(long long packageId, int delta, std::time_t now,
                                            std::optional<PackagePurchaseRecord>& out) {
    auto record = packageId > 0 ? database_.db().findPackagePurchase(packageId) : std::nullopt;
    if (!record || record->status == "cancelled" || delta == 0) return false;
    record->sessionsUsed = std::max(0, record->sessionsUsed + delta);
    settle(*record, now);
    if (!database_.db().updatePackagePurchase(*record).ok) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "занятия пакета " << packageId << " не сохранены";
        return false;
    }
    out = std::move(record);
    return true;
}

int BookingManager::returnableSessions(const Json& before, const Json& after, const BookingPageRecord* page,
                                       std::time_t now, bool all) {
    const auto& was = before.items();
    const auto& now_ = after.items();
    int count = 0;
    for (std::size_t i = 0; i < was.size() && i < now_.size(); ++i) {
        if (was[i].getString("status") != "active" || now_[i].getString("status") != "cancelled") continue;
        std::time_t start = 0;
        if (!parseIsoUtc(was[i].getString("start"), start)) continue;
        if (all || page == nullptr || !lateFor(*page, start, now)) ++count;
    }
    return count;
}

// ------------------------------------------------------------ извещения гостю

bool BookingManager::packageRecipients(const PackagePurchaseRecord& record, const BookingPageRecord& page,
                                       const std::string& kind, GuestNotice& notice) {
    const auto addEmail = [&](const std::string& email) {
        if (email.empty() || !mail_) return;
        if (!limited("booking.mail:" + mailboxKey(email), kMailPerAddressHour, 3600)) {
            notice.emails.push_back(email);
        } else {
            SELFVOY_LOG(log::Level::Warn, "booking") << "лимит писем на адрес исчерпан, письмо (" << kind << ") не отправлено";
        }
    };
    addEmail(record.guestEmail);
    if (lowerAscii(record.ccEmail) != lowerAscii(record.guestEmail)) addEmail(record.ccEmail);
    // Telegram — чаты, подключённые к записям этого пакета.
    if (telegram_ && telegram_->enabled() && record.id > 0) {
        std::set<long long> chats;
        for (const auto& booking : database_.db().listBookingsByPackage(record.id, 100)) {
            for (const long long chat : database_.db().bookingTelegramChats(booking.id)) chats.insert(chat);
        }
        for (const long long chat : chats) {
            if (limited("booking.tg:" + std::to_string(chat), kMailPerAddressHour, 3600)) continue;
            notice.chats.push_back(chat);
        }
    }
    if (notice.emails.empty() && notice.chats.empty()) return false;
    if (limited("booking.mail.owner:" + std::to_string(page.ownerId), kMailPerOwnerHour, 3600)) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "лимит писем владельца " << page.ownerId << " исчерпан";
        return false;
    }
    return true;
}

void BookingManager::notifyPackage(const PackagePurchaseRecord& record, const BookingPageRecord& page,
                                   const std::string& kind, const std::string& subject, const std::string& title,
                                   const std::string& lead, const std::vector<std::string>& lines,
                                   const std::string& linkUrl, const std::string& linkText,
                                   const std::vector<std::string>& footer) {
    GuestNotice notice;
    if (!packageRecipients(record, page, kind, notice)) return;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    MailNotice& mail = notice.mail;
    mail.kind = kind;
    mail.subject = subject;
    mail.title = title;
    mail.lead = lead;
    mail.lines = lines;
    mail.linkUrl = linkUrl;
    mail.linkText = linkText;
    mail.footer = footer;
    const std::string signature = brandingLine(page);
    if (!signature.empty()) mail.footer.push_back(signature);
    const bool personal = linkUrl.find("/#/p/") != std::string::npos;
    mail.note = "Письмо пришло, потому что этот адрес указали при оформлении пакета на странице «" + page.title +
                "» (" + ownerName + ") в Selfvoy." + (personal ? " Ссылка — только для вас, не пересылайте её." : "");
    if (!notice.chats.empty()) {
        std::string text = title + "\n«" + page.title + "» — " + ownerName + "\n\n" + lead;
        for (const auto& line : lines) text += "\n• " + line;
        if (!linkUrl.empty()) {
            if (linkUrl.rfind("https://", 0) == 0) {
                Json button = Json::object();
                button.set("text", Json(linkText));
                button.set("url", Json(linkUrl));
                Json row = Json::array();
                row.push(button);
                Json keyboard = Json::array();
                keyboard.push(row);
                notice.telegramMarkup = Json::object();
                notice.telegramMarkup.set("inline_keyboard", keyboard);
            } else {
                text += "\n\n" + linkText + ": " + linkUrl;
            }
        }
        if (!signature.empty()) text += "\n\n" + signature;
        notice.telegramText = text;
    }
    enqueueNotice(std::move(notice));
}

void BookingManager::packageLow(const PackagePurchaseRecord& record) {
    const auto page = database_.db().findBookingPage(record.pageId);
    if (!page) return;
    const int left = leftOf(record);
    const std::string pageUrl = config_.publicUrl.empty() || page->status != "active"
                                    ? std::string()
                                    : config_.publicUrl + "/#/b/" + page->slug;
    const std::string lead = "Осталось " + sessionsText(left) + " — продлите абонемент" +
                             (pageUrl.empty() ? std::string(" на странице записи.") : ": " + pageUrl);
    notifyPackage(record, *page, "package.low", "Selfvoy: осталось " + sessionsText(left) + " по абонементу",
                  "Абонемент заканчивается", lead,
                  {"«" + record.title + "»: использовано " + std::to_string(record.sessionsUsed) + " из " +
                   std::to_string(record.sessionsTotal)},
                  pageUrl, "Продлить абонемент", {});
    if (notifications_) {
        Json extra = Json::object();
        extra.set("left", Json(static_cast<long long>(left)));
        notifications_->create(record.ownerId, "package.low",
                               record.guestName + ": осталось " + sessionsText(left) + " по абонементу",
                               record.title + " · " + page->title, packageNotice(record, extra));
    }
}

// ------------------------------------------------------------ оплата пакета

void BookingManager::announcePackagePaid(const PackagePurchaseRecord& record, bool notifyOwner) {
    taxReceiptForPackage(record);  // F10: «выдать чек» (самозанятые, RUB)
    const auto page = database_.db().findBookingPage(record.pageId);
    const std::string money = moneyOf(record.payment);
    if (notifyOwner && notifications_) {
        Json extra = Json::object();
        extra.set("status", Json(std::string("paid")));
        extra.set("amount", Json(record.payment.getInt("amount")));
        extra.set("currency", Json(record.payment.getString("currency", "RUB")));
        notifications_->create(record.ownerId, "package.payment",
                               record.guestName + " оплатил(а) пакет «" + record.title + "»",
                               money + (page ? " · " + page->title : std::string()), packageNotice(record, extra));
    }
    if (!page) return;
    const std::string url = packageUrl(packageTokenOf(record));
    notifyPackage(record, *page, "package.paid", "Selfvoy: оплата пакета получена", "Оплата получена",
                  money + " — спасибо! Абонемент «" + record.title + "»: " + sessionsText(leftOf(record)) +
                      (record.validUntil.empty() ? std::string(".") : ", действует до " + dateText(record.validUntil) + "."),
                  {}, url, "Открыть абонемент", {});
}

void BookingManager::cancelBySystem(long long bookingId, std::time_t now, const std::string& reason) {
    std::vector<long long> tasks;
    int cancelled = 0;
    Json before;
    std::optional<BookingRecord> booking;
    const bool changed = mutateBooking(bookingId, [&](BookingRecord& record) {
        if (record.status != "active") return false;
        before = record.occurrences;
        cancelled = cancelOccurrences(record, -1, now, tasks);
        if (record.status == "cancelled") record.cancelledBy = "system";
        return cancelled > 0;
    }, booking);
    if (!changed || !booking) return;
    for (const long long task : tasks) deleteOwnTask(booking->ownerId, task);
    dropInactiveReminders(*booking);
    if (notifications_) {
        const auto page = database_.db().findBookingPage(booking->pageId);
        Json notify = Json::object();
        notify.set("booking_id", Json(booking->id));
        notify.set("page_id", Json(booking->pageId));
        notify.set("cancelled", Json(static_cast<long long>(cancelled)));
        notify.set("guest_name", Json(booking->guestName));
        notify.set("reason", Json(reason));
        notify.set("cancelled_by", Json(std::string("system")));
        notifications_->create(booking->ownerId, "booking.cancelled", booking->guestName + ": " + reason + " — запись отменена",
                               page ? page->title : std::string(), notify);
    }
    // F8: освобождённое время — листу ожидания.
    const auto freed = freedOccurrences(before, booking->occurrences, std::time(nullptr));
    if (!freed.empty()) waitlistRelease(booking->pageId, freed, std::max(now, std::time(nullptr)));
}

bool BookingManager::expirePackagePayment(long long packageId, std::time_t now) {
    std::optional<PackagePurchaseRecord> record;
    const bool changed = mutatePackage(packageId, [&](PackagePurchaseRecord& item) {
        if (item.status != "pending" || !payments::awaiting(item.payment)) return false;
        item.payment = withStatus(item.payment, "expired");
        item.status = "cancelled";
        return true;
    }, record);
    if (!changed || !record) return false;
    // Время, занятое записями неоплаченного пакета, освобождается.
    for (const auto& booking : database_.db().listBookingsByPackage(packageId, 200)) {
        if (booking.status == "active") cancelBySystem(booking.id, now, "оплата пакета не поступила");
    }
    const auto page = database_.db().findBookingPage(record->pageId);
    if (notifications_) {
        Json extra = Json::object();
        extra.set("status", Json(std::string("expired")));
        extra.set("amount", Json(record->payment.getInt("amount")));
        extra.set("currency", Json(record->payment.getString("currency", "RUB")));
        notifications_->create(record->ownerId, "package.payment",
                               record->guestName + ": оплата пакета не поступила — покупка отменена",
                               record->title + (page ? " · " + page->title : std::string()),
                               packageNotice(*record, extra));
    }
    if (page) {
        const std::string pageUrl = config_.publicUrl.empty() || page->status != "active"
                                        ? std::string()
                                        : config_.publicUrl + "/#/b/" + page->slug;
        notifyPackage(*record, *page, "package.payment_expired", "Selfvoy: оплата пакета не поступила",
                      "Покупка отменена",
                      "Оплата " + moneyOf(record->payment) + " за пакет «" + record->title +
                          "» не поступила вовремя, поэтому покупка отменена и время освобождено.",
                      {}, pageUrl, "Выбрать время снова", {});
    }
    SELFVOY_LOG(log::Level::Info, "booking") << "пакет " << packageId << " отменён: оплата не поступила";
    return true;
}

void BookingManager::refundPackage(const PackagePurchaseRecord& record, bool provider) {
    const std::string money = moneyOf(record.payment);
    const auto notifyOwner = [&](const PackagePurchaseRecord& item, const std::string& status, const std::string& title,
                                 const std::string& body) {
        if (!notifications_) return;
        Json extra = Json::object();
        extra.set("status", Json(status));
        extra.set("amount", Json(item.payment.getInt("amount")));
        extra.set("currency", Json(item.payment.getString("currency", "RUB")));
        notifications_->create(item.ownerId, "package.payment", title, body, packageNotice(item, extra));
    };
    if (provider && payments_) {
        std::string error;
        if (payments_->refund(record.ownerId, record.payment, "pr-" + std::to_string(record.id), error)) {
            const std::string at = isoUtc(std::time(nullptr));
            std::optional<PackagePurchaseRecord> updated;
            mutatePackage(record.id, [&](PackagePurchaseRecord& item) {
                if (!item.payment.isObject() || item.payment.getString("status") == "refunded") return false;
                Json payment = withStatus(item.payment, "refunded");
                payment.set("refunded_at", Json(at));
                item.payment = payment;
                return true;
            }, updated);
            notifyOwner(updated ? *updated : record, "refunded", "Оплата пакета возвращена: " + record.guestName,
                        money + " — возврат через ЮKassa");
            if (updated) taxRefundForPackage(*updated);  // F10: чек уже выдан — аннулировать
            return;
        }
        notifyOwner(record, "refund_due", "Верните оплату пакета вручную: " + record.guestName,
                    "ЮKassa не приняла возврат " + money + " — верните его в личном кабинете ЮKassa");
        return;
    }
    notifyOwner(record, "refund_due", "Верните оплату пакета вручную: " + record.guestName,
                money + " — пакет отменён, оплату нужно вернуть гостю");
}

PaymentManager::Remote BookingManager::syncPackagePayment(const PackagePurchaseRecord& record, std::time_t now,
                                                          int minInterval) {
    const Json& payment = record.payment;
    if (!payments_ || !payment.isObject() || payment.getString("method") != "yookassa" ||
        payment.getString("provider_id").empty()) {
        return PaymentManager::Remote::Error;
    }
    const std::string status = payment.getString("status");
    const bool waiting = record.status == "pending" && status == "pending";
    // Оплата истекла (покупка отменена), а гость всё же заплатил — автоматический возврат.
    const bool lateRefund = status == "expired" && payment.getString("refunded_at").empty();
    if (!waiting && !lateRefund) return PaymentManager::Remote::Error;
    std::string error;
    const PaymentManager::Remote remote = payments_->poll(record.ownerId, payment, now, minInterval, error);
    if (waiting) {
        if (remote == PaymentManager::Remote::Succeeded) {
            std::optional<PackagePurchaseRecord> updated;
            const bool changed = mutatePackage(record.id, [&](PackagePurchaseRecord& item) {
                if (item.status != "pending" || !payments::awaiting(item.payment)) return false;
                Json paid = withStatus(item.payment, "paid");
                paid.set("paid_at", Json(isoUtc(now)));
                item.payment = paid;
                activate(item, now);
                return true;
            }, updated);
            if (changed && updated) {
                announcePackagePaid(*updated, true);
                // Занятия, списанные записью при покупке, — теперь по действующему пакету.
                if (leftOf(*updated) <= kPackageLowLeft && updated->sessionsUsed > 0) packageLow(*updated);
            }
        } else if (remote == PaymentManager::Remote::Canceled) {
            expirePackagePayment(record.id, now);
        }
        return remote;
    }
    if (remote == PaymentManager::Remote::Succeeded) {
        SELFVOY_LOG(log::Level::Info, "booking") << "пакет " << record.id << ": оплата после отмены — возврат";
        refundPackage(record, true);
    }
    return remote;
}

std::size_t BookingManager::packageTick(int limit, std::time_t now) {
    const auto pending = database_.db().listPendingPackagePurchases(limit);
    for (const auto& record : pending) {
        const Json& payment = record.payment;
        std::time_t due = 0;
        const bool hasDue = parseIsoUtc(payment.getString("due_at"), due);
        const bool overdue = hasDue && now >= due;
        if (payment.getString("method") == "yookassa" && !payment.getString("provider_id").empty() && payments_) {
            // К сроку — последний опрос без ограничения: оплата могла пройти в последнюю минуту.
            const auto remote = syncPackagePayment(record, now, overdue ? 0 : PaymentManager::kPollIntervalSec);
            if (remote == PaymentManager::Remote::Succeeded || remote == PaymentManager::Remote::Canceled) continue;
            // ЮKassa не ответила — ждём до часа сверх срока (не отменяем, возможно, оплаченный пакет).
            if (remote == PaymentManager::Remote::Error && overdue && now < due + kPaymentGraceSec) continue;
        }
        if (overdue && payments::awaiting(payment)) expirePackagePayment(record.id, now);
    }
    // Срок действия: active → expired по valid_until.
    const auto expired = database_.db().listExpiredPackagePurchases(isoUtc(now), limit);
    for (const auto& record : expired) {
        std::optional<PackagePurchaseRecord> updated;
        mutatePackage(record.id, [&](PackagePurchaseRecord& item) {
            std::time_t until = 0;
            if (item.status != "active" || !parseIsoUtc(item.validUntil, until) || until > now) return false;
            item.status = "expired";
            return true;
        }, updated);
    }
    return pending.size() + expired.size();
}

// ------------------------------------------------------------ гость

BookingManager::Result BookingManager::packageGet(const Json& payload, const std::string& remoteAddr) {
    if (limited("package.get:" + Session::ipOf(remoteAddr), kPackageGetPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    auto record = hash.empty() ? std::nullopt : database_.db().findPackagePurchaseByToken(hash);
    const auto page = record ? database_.db().findBookingPage(record->pageId) : std::nullopt;
    if (!record || !page) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    // ЮKassa: статус платежа — при каждом открытии ссылки пакета (гость вернулся с ?paid=1).
    if (record->payment.isObject() && record->payment.getString("method") == "yookassa") {
        const bool waiting = record->status == "pending" && payments::awaiting(record->payment);
        if (syncPackagePayment(*record, std::time(nullptr), waiting ? 0 : PaymentManager::kPollIntervalSec) !=
            PaymentManager::Remote::Throttled) {
            if (auto fresh = database_.db().findPackagePurchase(record->id)) record = std::move(fresh);
        }
    }
    Result result;
    result.ok = true;
    result.payload.set("package", packageGuestJson(*record, *page));
    return result;
}

BookingManager::Result BookingManager::packagePaymentClaim(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kPackageChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto found = hash.empty() ? std::nullopt : database_.db().findPackagePurchaseByToken(hash);
    if (!found) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    const std::time_t now = std::time(nullptr);
    Result failure;
    bool changed = false;
    bool activated = false;
    std::optional<PackagePurchaseRecord> record;
    mutatePackage(found->id, [&](PackagePurchaseRecord& item) {
        const Json& payment = item.payment;
        const std::string status = payment.isObject() ? payment.getString("status") : std::string();
        if (!payment.isObject()) {
            failure = Result::failure(protocol::code::kBadRequest, "у абонемента нет оплаты");
        } else if (payment.getString("method") != "link") {
            failure = Result::failure(protocol::code::kBadRequest, "оплата через ЮKassa подтверждается автоматически");
        } else if (item.status == "cancelled" || status == "expired") {
            failure = Result::failure(protocol::code::kConflict, "покупка отменена — оплата больше не нужна");
        } else if (status == "pending") {
            // «Я оплатил(а)»: пакет начинает действовать (как claimed у записи — не истекает).
            item.payment = withStatus(payment, "claimed");
            activated = item.status == "pending";
            if (activated) activate(item, now);
            changed = true;
            return true;
        }
        return false;
    }, record);
    if (!failure.code.empty()) return failure;
    if (!record) record = database_.db().findPackagePurchase(found->id);
    const auto page = record ? database_.db().findBookingPage(record->pageId) : std::nullopt;
    if (!record || !page) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    if (changed && notifications_) {
        Json extra = Json::object();
        extra.set("status", Json(std::string("claimed")));
        extra.set("amount", Json(record->payment.getInt("amount")));
        extra.set("currency", Json(record->payment.getString("currency", "RUB")));
        notifications_->create(record->ownerId, "package.payment",
                               record->guestName + " отметил(а) оплату пакета — проверьте поступление",
                               moneyOf(record->payment) + " · " + record->title, packageNotice(*record, extra));
    }
    if (activated && record->sessionsUsed > 0 && leftOf(*record) <= kPackageLowLeft) packageLow(*record);
    Result result;
    result.ok = true;
    result.payload.set("package", packageGuestJson(*record, *page));
    return result;
}

// ------------------------------------------------------------ владелец

BookingManager::Result BookingManager::packageList(long long ownerId, const Json& payload) {
    long long limit = 50;
    if (!intField(payload, "limit", 1, 200, limit)) {
        return Result::failure(protocol::code::kBadRequest, "limit — от 1 до 200");
    }
    long long pageId = 0;
    if (!intField(payload, "page_id", 0, 1LL << 53, pageId)) {
        return Result::failure(protocol::code::kBadRequest, "page_id — id страницы записи");
    }
    const std::string status = payload.getString("status");
    if (!status.empty() && !validPackageStatus(status)) {
        return Result::failure(protocol::code::kBadRequest, "status — pending, active, used, expired или cancelled");
    }
    std::map<long long, std::string> titles;
    Json packages = Json::array();
    for (const auto& record :
         database_.db().listPackagePurchases(ownerId, pageId, status, static_cast<int>(limit))) {
        auto title = titles.find(record.pageId);
        if (title == titles.end()) {
            const auto page = database_.db().findBookingPage(record.pageId);
            title = titles.emplace(record.pageId, page ? page->title : std::string()).first;
        }
        packages.push(packageOwnerJson(record, title->second));
    }
    Result result;
    result.ok = true;
    result.payload.set("packages", packages);
    return result;
}

BookingManager::Result BookingManager::packageIssue(long long ownerId, const Json& payload) {
    const long long pageId = payload.getInt("page_id");
    const auto page = pageId > 0 ? database_.db().findBookingPage(pageId) : std::nullopt;
    if (!page || page->ownerId != ownerId || page->status == "archived") {
        return Result::failure(protocol::code::kNotFound, "страница записи не найдена");
    }
    PackagePurchaseRecord record;
    record.ownerId = ownerId;
    record.pageId = page->id;
    record.currency = page->currency;
    record.guestName = trim(payload.getString("guest_name"));
    if (record.guestName.empty() || !validText(record.guestName, 80, false)) {
        return Result::failure(protocol::code::kBadRequest, "укажите имя гостя — до 80 символов");
    }
    record.guestEmail = trim(payload.getString("guest_email"));
    if (!record.guestEmail.empty() && !plausibleEmail(record.guestEmail)) {
        return Result::failure(protocol::code::kBadRequest, "некорректный email");
    }
    record.ccEmail = trim(payload.getString("cc_email"));
    if (!record.ccEmail.empty()) {
        if (!plausibleEmail(record.ccEmail)) {
            return Result::failure(protocol::code::kBadRequest, "второй адрес (cc_email) — некорректный email");
        }
        if (lowerAscii(record.ccEmail) == lowerAscii(record.guestEmail)) {
            return Result::failure(protocol::code::kBadRequest, "второй адрес должен отличаться от email гостя");
        }
    }
    if (payload.contains("paid") && !payload.get("paid").isBool()) {
        return Result::failure(protocol::code::kBadRequest, "paid — true или false");
    }
    const bool paid = payload.getBool("paid", false);
    if (payload.contains("offer_id")) {
        long long offerId = 0;
        if (!intField(payload, "offer_id", 1, 1000000000, offerId)) {
            return Result::failure(protocol::code::kBadRequest, "offer_id — id пакета страницы");
        }
        const Json offers = page->packages;
        bool found = false;
        for (const auto& offer : offers.items()) {
            if (offer.getInt("id") != offerId) continue;
            found = true;
            record.offerId = offerId;
            record.title = offer.getString("title");
            record.sessionsTotal = static_cast<int>(offer.getInt("sessions"));
            record.price = offer.getInt("price");
            record.validDays = static_cast<int>(offer.getInt("valid_days", 90));
        }
        if (!found) return Result::failure(protocol::code::kBadRequest, "на странице нет такого пакета");
    } else {
        // Пакет вне предложений страницы: название и число занятий — обязательно.
        record.title = payload.get("title").isString() ? trim(payload.getString("title")) : std::string();
        if (record.title.empty() || !validText(record.title, 80, false)) {
            return Result::failure(protocol::code::kBadRequest, "название пакета — от 1 до 80 символов в одну строку");
        }
        long long sessions = 0;
        long long price = 0;
        long long validDays = 90;
        if (!payload.contains("sessions") || !intField(payload, "sessions", 1, 100, sessions)) {
            return Result::failure(protocol::code::kBadRequest, "занятий в пакете — от 1 до 100");
        }
        if (!intField(payload, "price", 0, payments::kMaxPrice, price)) {
            return Result::failure(protocol::code::kBadRequest, "цена пакета — целое число в минимальных единицах");
        }
        if (!intField(payload, "valid_days", 7, 365, validDays)) {
            return Result::failure(protocol::code::kBadRequest, "срок действия пакета — от 7 до 365 дней");
        }
        record.sessionsTotal = static_cast<int>(sessions);
        record.price = price;
        record.validDays = static_cast<int>(validDays);
    }
    if (limited("package.issue:" + std::to_string(ownerId), kIssuePerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много пакетов — попробуйте через час");
    }
    const std::time_t now = std::time(nullptr);
    // Продажа вне сайта: пакет действует сразу; оплата — отметка владельца (paid или ждёт).
    if (record.price > 0) record.payment = manualPayment(record.price, record.currency, paid ? "paid" : "pending", now);
    activate(record, now);
    std::string token;
    long long id = 0;
    for (int attempt = 0; attempt < 3 && id == 0; ++attempt) {
        token = newToken();
        record.tokenHash = tokenHash(token);
        if (record.tokenHash.empty()) continue;
        record.tokenEnc = crypto::sealField(config_.dataKey(), token, kPackageTokenAad);
        const DatabaseError saved = database_.db().createPackagePurchase(record, id);
        if (!saved.ok) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "пакет не сохранён: " << saved.message;
            id = 0;
        }
    }
    if (id == 0) return Result::failure(protocol::code::kInternal, "не удалось сохранить пакет");
    const auto stored = database_.db().findPackagePurchase(id);
    if (stored) record = *stored;
    record.id = id;
    const std::string url = packageUrl(token);
    {
        std::string ownerName;
        std::string ownerTimezone;
        ownerInfo(page->ownerId, ownerName, ownerTimezone);
        std::vector<std::string> footer = packageLines(record, *page, token, now);
        footer.erase(footer.begin());  // первая строка — то же, что в тексте письма
        notifyPackage(record, *page, "package.issued", "Selfvoy: ваш абонемент «" + record.title + "»", "Ваш абонемент",
                      ownerName + " оформил(а) для вас абонемент «" + record.title + "»: " +
                          sessionsText(record.sessionsTotal) + ", действует до " + dateText(record.validUntil) + ".",
                      {}, url, "Записаться по абонементу", footer);
    }
    // F10: продажа вне сайта с отметкой «оплачено» — «выдать чек».
    if (record.payment.isObject() && record.payment.getString("status") == "paid") taxReceiptForPackage(record);
    if (const auto fresh = database_.db().findPackagePurchase(id)) record = *fresh;
    Result result;
    result.ok = true;
    result.payload.set("package", packageOwnerJson(record, page->title));
    result.payload.set("token", Json(token));
    result.payload.set("url", Json(url));
    return result;
}

BookingManager::Result BookingManager::packageAdjust(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("package_id");
    const auto found = id > 0 ? database_.db().findPackagePurchase(id) : std::nullopt;
    if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    long long delta = 0;
    if (!payload.contains("delta") || !intField(payload, "delta", -100, 100, delta) || delta == 0) {
        return Result::failure(protocol::code::kBadRequest, "delta — от −100 до 100, не 0");
    }
    const std::time_t now = std::time(nullptr);
    Result failure;
    std::optional<PackagePurchaseRecord> record;
    mutatePackage(id, [&](PackagePurchaseRecord& item) {
        if (item.status == "cancelled") {
            failure = Result::failure(protocol::code::kConflict, "абонемент отменён");
            return false;
        }
        const long long total = item.sessionsTotal + delta;
        if (total < item.sessionsUsed || total < 1) {
            failure = Result::failure(protocol::code::kBadRequest,
                                      "нельзя убрать больше, чем осталось (" + sessionsText(leftOf(item)) + ")");
            return false;
        }
        if (total > kMaxPackageSessions) {
            failure = Result::failure(protocol::code::kBadRequest, "в абонементе — не больше 1000 занятий");
            return false;
        }
        item.sessionsTotal = static_cast<int>(total);
        settle(item, now);
        return true;
    }, record);
    if (!failure.code.empty()) return failure;
    if (!record) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    const auto page = database_.db().findBookingPage(record->pageId);
    Result result;
    result.ok = true;
    result.payload.set("package", packageOwnerJson(*record, page ? page->title : std::string()));
    return result;
}

BookingManager::Result BookingManager::packageCancel(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("package_id");
    const auto found = id > 0 ? database_.db().findPackagePurchase(id) : std::nullopt;
    if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    if (found->status == "cancelled") return Result::failure(protocol::code::kConflict, "абонемент уже отменён");
    bool refundProvider = false;
    std::optional<PackagePurchaseRecord> record;
    mutatePackage(id, [&](PackagePurchaseRecord& item) {
        if (item.status == "cancelled") return false;
        if (payments::awaiting(item.payment)) {
            item.payment = withStatus(item.payment, "expired");
        } else if (item.payment.isObject() && item.payment.getString("status") == "paid" &&
                   item.payment.getString("method") == "yookassa" && item.sessionsUsed == 0) {
            // Оплачен через ЮKassa и ни одно занятие не использовано — возврат всей суммы.
            refundProvider = true;
        }
        item.status = "cancelled";
        return true;
    }, record);
    if (!record) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    if (refundProvider) {
        refundPackage(*record, true);
        if (auto fresh = database_.db().findPackagePurchase(id)) record = std::move(fresh);
    }
    const auto page = database_.db().findBookingPage(record->pageId);
    if (page) {
        std::string ownerName;
        std::string ownerTimezone;
        ownerInfo(page->ownerId, ownerName, ownerTimezone);
        notifyPackage(*record, *page, "package.cancelled", "Selfvoy: абонемент отменён", "Абонемент отменён",
                      ownerName + " отменил(а) абонемент «" + record->title + "». Записаться по нему больше нельзя.",
                      {}, "", "",
                      refundProvider ? std::vector<std::string>{"Оплата " + moneyOf(record->payment) +
                                                                " будет возвращена тем же способом."}
                                     : std::vector<std::string>{});
    }
    Result result;
    result.ok = true;
    result.payload.set("package", packageOwnerJson(*record, page ? page->title : std::string()));
    return result;
}

BookingManager::Result BookingManager::packagePaymentSet(long long ownerId, const Json& payload) {
    const std::string status = payload.getString("status");
    if (status != "paid" && status != "waived" && status != "refunded") {
        return Result::failure(protocol::code::kBadRequest, "status — paid, waived или refunded");
    }
    const long long id = payload.getInt("package_id");
    const auto found = id > 0 ? database_.db().findPackagePurchase(id) : std::nullopt;
    if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    if (!found->payment.isObject()) return Result::failure(protocol::code::kBadRequest, "у абонемента нет оплаты");
    const std::time_t now = std::time(nullptr);
    bool paidNow = false;
    bool refundedNow = false;
    bool activated = false;
    std::optional<PackagePurchaseRecord> record;
    mutatePackage(id, [&](PackagePurchaseRecord& item) {
        if (!item.payment.isObject() || item.payment.getString("status") == status) return false;
        refundedNow = status == "refunded";
        Json payment = withStatus(item.payment, status);
        if (status == "paid") payment.set("paid_at", Json(isoUtc(now)));
        if (status == "refunded") payment.set("refunded_at", Json(isoUtc(now)));
        item.payment = payment;
        // Отметка «оплачено» / «без оплаты» запускает пакет, ждавший оплату.
        activated = (status == "paid" || status == "waived") && item.status == "pending";
        if (activated) activate(item, now);
        paidNow = status == "paid";
        return true;
    }, record);
    if (!record) record = database_.db().findPackagePurchase(id);
    if (!record) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
    if (paidNow) announcePackagePaid(*record, false);  // владелец сам отметил — уведомлять его не нужно
    if (refundedNow) taxRefundForPackage(*record);  // F10: чек уже выдан — аннулировать
    if (paidNow || refundedNow) {
        if (auto fresh = database_.db().findPackagePurchase(id)) record = std::move(fresh);  // F10: npd, receipt_*
    }
    if (activated && record->sessionsUsed > 0 && leftOf(*record) <= kPackageLowLeft) packageLow(*record);
    const auto page = database_.db().findBookingPage(record->pageId);
    Result result;
    result.ok = true;
    result.payload.set("package", packageOwnerJson(*record, page ? page->title : std::string()));
    return result;
}

}  // namespace selfvoy
