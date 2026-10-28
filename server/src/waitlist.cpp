// selfvoy/waitlist.cpp — лист ожидания и «пора на повторный приём» (F8).
//
// Лист ожидания — поле страницы waitlist (Pro). Гость оставляет имя, email, желаемые
// дни (местные даты владельца) и часть дня (booking.waitlistJoin). Время освободилось
// (отмена гостем, владельцем или системой, перенос — прежнее время) — сервер под
// блокировкой расписания владельца выбирает самую старую подходящую запись листа и
// держит за ней это время waitlist_hold_minutes: держание — занятость владельца
// (MemoryManager::busy, лимит записей в день), поэтому другие гости его не видят и не
// займут. Гостю — письмо (и бот, если подписан) со ссылкой #/w/<токен>; booking.waitlistClaim
// записывает обычным путём booking.book (та же проверка и оплата — commitBooking).
// Не забрал — планировщик (waitlistTick) отдаёт время следующему в листе, затем оно
// освобождается; запись листа возвращается в конец очереди.
//
// «Пора записаться снова» — поле страницы rebook_after_days (Pro): через N дней после
// последней встречи записи, если у гостя нет более поздних записей на этой странице,
// гостю письмо (+ бот, если подписан на запись) со ссылкой на страницу — ровно один раз
// на запись (rebookTick: атомарный захват, на PostgreSQL FOR UPDATE SKIP LOCKED).
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"
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
constexpr int kWaitlistGetPerMinute = 30;
constexpr int kWaitlistChangePerMinute = 10;
constexpr int kWaitlistListMax = 200;

std::string newToken() {
    std::string token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
    while (!token.empty() && token.back() == '=') token.pop_back();
    return token;
}

bool freeOf(const BookingManager::Busy& busy, std::time_t start, std::time_t end) {
    return std::none_of(busy.begin(), busy.end(),
                        [&](const auto& interval) { return start < interval.second && interval.first < end; });
}

// Часть дня по местному времени владельца (local — YYYY-MM-DDTHH:MM): утро — до 12:00,
// день — 12:00–17:00, вечер — с 17:00. "" — местное время неизвестно.
std::string partOf(const std::string& local) {
    if (!validLocal(local)) return "";
    const int hour = std::atoi(local.substr(11, 2).c_str());
    return hour < 12 ? "morning" : hour < 17 ? "afternoon" : "evening";
}

bool validPart(const std::string& part) {
    return part == "any" || part == "morning" || part == "afternoon" || part == "evening";
}

bool validEntryStatus(const std::string& status) {
    return status == "waiting" || status == "offered" || status == "booked" || status == "left" ||
           status == "expired";
}

// Время подходит записи листа: день в списке (или список пуст) и часть дня.
bool matches(const WaitlistEntryRecord& entry, const std::string& day, const std::string& local) {
    if (!entry.days.empty()) {
        const auto& days = entry.days.items();
        if (std::none_of(days.begin(), days.end(), [&](const Json& item) { return item.asString() == day; })) {
            return false;
        }
    }
    return entry.partOfDay == "any" || entry.partOfDay == partOf(local);
}

// «чт, 22 октября» — местная дата владельца YYYY-MM-DD.
std::string dayLabel(const std::string& day) {
    static const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                          "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
    static const char* const kDays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    std::time_t at = 0;
    if (!validDay(day) || !parseIsoUtc(day + "T00:00:00Z", at)) return day;
    std::tm parts{};
    gmtime_r(&at, &parts);
    return std::string(kDays[parts.tm_wday]) + ", " + std::to_string(parts.tm_mday) + " " + kMonths[parts.tm_mon];
}

std::string partLabel(const std::string& part) {
    if (part == "morning") return "утро (до 12:00)";
    if (part == "afternoon") return "день (12:00–17:00)";
    if (part == "evening") return "вечер (после 17:00)";
    return "любое время";
}

std::string daysText(long long count) {
    const long long tail = count % 100;
    const long long last = count % 10;
    const char* word = (tail >= 11 && tail <= 14) ? "дней" : last == 1 ? "день" : (last >= 2 && last <= 4) ? "дня" : "дней";
    return std::to_string(count) + " " + word;
}

// «15:40 (Europe/Moscow)» — момент at в местном времени владельца по сдвигу встречи
// (local проверен при записи), иначе UTC.
std::string clockText(std::time_t at, const std::string& startIso, const std::string& local,
                      const std::string& timezone) {
    std::time_t start = 0;
    std::time_t asUtc = 0;
    long long offset = 0;
    std::string zone = "UTC";
    if (validLocal(local) && parseIsoUtc(startIso, start) && parseIsoUtc(local + ":00Z", asUtc) &&
        std::llabs(static_cast<long long>(asUtc - start)) <= 14 * 3600) {
        offset = static_cast<long long>(asUtc - start);
        zone = timezone.empty() ? "UTC" : timezone;
    }
    return isoUtc(at + static_cast<std::time_t>(offset)).substr(11, 5) + " (" + zone + ")";
}

}  // namespace

// ------------------------------------------------------------------ общее

std::string BookingManager::waitlistUrl(const std::string& token) const {
    return config_.publicUrl.empty() || token.empty() ? std::string() : config_.publicUrl + "/#/w/" + token;
}

std::string BookingManager::waitlistTokenOf(const WaitlistEntryRecord& entry) const {
    std::string token;
    if (entry.tokenEnc.empty() || !crypto::openField(config_.dataKey(), entry.tokenEnc, kWaitlistTokenAad, token)) {
        return "";
    }
    return tokenHash(token) == entry.tokenHash ? token : std::string();
}

std::string BookingManager::waitlistTelegramUrl(const WaitlistEntryRecord& entry) const {
    if (!telegram_ || !telegram_->enabled() || entry.id <= 0 || entry.tokenHash.empty()) return "";
    if (entry.status != "waiting" && entry.status != "offered") return "";
    return telegram_->linkUrl(telegram_->waitlistPayload(entry.id, entry.tokenHash));
}

Json BookingManager::waitlistGuestJson(const WaitlistEntryRecord& entry) const {
    Json json = Json::object();
    json.set("id", Json(entry.id));
    json.set("status", Json(entry.status));
    json.set("days", entry.days.isArray() ? entry.days : Json::array());
    json.set("part_of_day", Json(entry.partOfDay));
    json.set("expires_at", Json(entry.expiresAt));
    return json;
}

Json BookingManager::waitlistOwnerJson(const WaitlistEntryRecord& entry, const std::string& pageTitle) const {
    Json json = Json::object();
    json.set("id", Json(entry.id));
    json.set("page_id", Json(entry.pageId));
    json.set("page_title", Json(pageTitle));
    json.set("guest_name", Json(entry.guestName));
    json.set("guest_email", Json(maskEmail(entry.guestEmail)));  // замаскирован, как cc_email у гостя
    json.set("days", entry.days.isArray() ? entry.days : Json::array());
    json.set("part_of_day", Json(entry.partOfDay));
    json.set("comment", Json(entry.comment));
    json.set("status", Json(entry.status));
    json.set("booking_id", Json(entry.bookingId));
    json.set("telegram_linked", Json(entry.telegramChatId != 0));
    json.set("created_at", Json(entry.createdAt));
    json.set("expires_at", Json(entry.expiresAt));
    if (entry.status == "offered") {
        Json offer = Json::object();
        offer.set("start", Json(entry.offerStart));
        offer.set("end", Json(entry.offerEnd));
        offer.set("expires_at", Json(entry.offerExpiresAt));
        json.set("offer", offer);
    }
    return json;
}

bool BookingManager::mutateWaitlist(long long entryId, const std::function<bool(WaitlistEntryRecord&)>& change,
                                    std::optional<WaitlistEntryRecord>& out) {
    const auto first = entryId > 0 ? database_.db().findWaitlistEntry(entryId) : std::nullopt;
    if (!first) return false;
    auto lock = schedule_.acquire(first->ownerId);
    if (!lock.ok()) return false;
    auto entry = database_.db().findWaitlistEntry(entryId);  // перечитываем под блокировкой
    if (!entry || !change(*entry)) return false;
    if (!database_.db().updateWaitlistEntry(*entry).ok) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "запись листа ожидания " << entryId << " не сохранена";
        return false;
    }
    out = std::move(entry);
    return true;
}

void BookingManager::notifyWaitlist(const WaitlistEntryRecord& entry, const BookingPageRecord& page,
                                    const std::string& kind, const std::string& subject, const std::string& title,
                                    const std::string& lead, const std::vector<std::string>& lines,
                                    const std::string& linkUrl, const std::string& linkText,
                                    const std::vector<std::string>& footer) {
    GuestNotice notice;
    if (mail_ && !entry.guestEmail.empty()) {
        if (!limited("booking.mail:" + mailboxKey(entry.guestEmail), kMailPerAddressHour, 3600)) {
            notice.emails.push_back(entry.guestEmail);
        } else {
            SELFVOY_LOG(log::Level::Warn, "booking") << "лимит писем на адрес исчерпан, письмо (" << kind << ") не отправлено";
        }
    }
    if (telegram_ && telegram_->enabled() && entry.telegramChatId != 0 &&
        !limited("booking.tg:" + std::to_string(entry.telegramChatId), kMailPerAddressHour, 3600)) {
        notice.chats.push_back(entry.telegramChatId);
    }
    if (notice.emails.empty() && notice.chats.empty()) return;
    if (limited("booking.mail.owner:" + std::to_string(page.ownerId), kMailPerOwnerHour, 3600)) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "лимит писем владельца " << page.ownerId << " исчерпан";
        return;
    }
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
    mail.note = "Письмо пришло, потому что этот адрес указали, вставая в лист ожидания на странице «" + page.title +
                "» (" + ownerName + ") в Selfvoy." + (linkUrl.empty() ? "" : " Ссылка — только для вас, не пересылайте её.");
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

// ------------------------------------------------------------ поля страницы

BookingManager::Result BookingManager::applyWaitlistFields(long long ownerId, const Json& payload, bool creating,
                                                           BookingPageRecord& page) {
    const bool wasWaitlist = !creating && page.waitlist;
    const int wasRebook = creating ? 0 : page.rebookAfterDays;
    bool waitlist = page.waitlist;
    if (payload.contains("waitlist")) {
        if (!payload.get("waitlist").isBool()) {
            return Result::failure(protocol::code::kBadRequest, "waitlist — true или false");
        }
        waitlist = payload.getBool("waitlist");
    }
    long long hold = page.waitlistHoldMinutes;
    if (!intField(payload, "waitlist_hold_minutes", 15, 240, hold)) {
        return Result::failure(protocol::code::kBadRequest, "время держится за человеком из листа — от 15 до 240 минут");
    }
    long long rebook = page.rebookAfterDays;
    if (!intField(payload, "rebook_after_days", 0, 180, rebook) || (rebook != 0 && rebook < 7)) {
        return Result::failure(protocol::code::kBadRequest,
                               "напомнить о повторной записи — 0 (выключено) или от 7 до 180 дней");
    }
    // Pro (как пакеты F5): включить без Pro нельзя, править и выключить уже включённое — можно.
    if (waitlist && !wasWaitlist && !plan::proAllowed(database_.db(), ownerId)) {
        return Result::failure(protocol::code::kProRequired, "лист ожидания доступен на тарифе Pro");
    }
    if (rebook > 0 && wasRebook == 0 && !plan::proAllowed(database_.db(), ownerId)) {
        return Result::failure(protocol::code::kProRequired, "напоминание о повторной записи доступно на тарифе Pro");
    }
    page.waitlist = waitlist;
    page.waitlistHoldMinutes = static_cast<int>(hold);
    page.rebookAfterDays = static_cast<int>(rebook);
    return Result{true, "", "", Json::object()};
}

// ------------------------------------------------------------ освобождение

std::vector<Json> BookingManager::freedOccurrences(const Json& before, const Json& after, std::time_t now) {
    std::vector<Json> freed;
    if (!before.isArray() || !after.isArray()) return freed;
    const auto& was = before.items();
    const auto& is = after.items();
    for (std::size_t i = 0; i < was.size() && i < is.size(); ++i) {
        if (was[i].getString("status") != "active" || is[i].getString("status") != "cancelled") continue;
        std::time_t start = 0;
        if (parseIsoUtc(was[i].getString("start"), start) && start > now) freed.push_back(was[i]);
    }
    return freed;
}

void BookingManager::waitlistRelease(long long pageId, const std::vector<Json>& freed, std::time_t now,
                                     long long excludeEntry) {
    if (freed.empty()) return;
    const auto page = database_.db().findBookingPage(pageId);
    if (!page) return;
    {
        // Освободившееся время — сразу в booking.get этого экземпляра (кэш кандидатов AI).
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.erase(page->id);
    }
    if (!page->waitlist || page->status != "active" || !ownerAccepts(page->ownerId)) return;
    std::vector<Json> slots = freed;
    std::sort(slots.begin(), slots.end(),
              [](const Json& a, const Json& b) { return a.getString("start") < b.getString("start"); });

    std::vector<WaitlistEntryRecord> offered;
    {
        auto lock = schedule_.acquire(page->ownerId);
        if (!lock.ok()) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "лист ожидания страницы " << page->id
                                                     << ": расписание занято, время не предложено";
            return;
        }
        // Предлагается только время, которое гость успеет забрать до начала с запасом
        // страницы: начало ≥ сейчас + держание + notice_hours, в окне days_ahead.
        const std::time_t hold = static_cast<std::time_t>(page->waitlistHoldMinutes) * 60;
        const std::time_t earliest = now + hold + static_cast<std::time_t>(page->noticeHours) * 3600;
        const std::time_t windowEnd = now + static_cast<std::time_t>(page->daysAhead) * 86400;
        const std::time_t duration = static_cast<std::time_t>(page->durationMinutes) * 60;
        auto counts = dayCounts(*page, 0, "");
        Busy busy = memory_.busy(page->ownerId);
        std::vector<WaitlistEntryRecord> queue = database_.db().listWaitingEntries(page->id, isoUtc(now), 500);
        for (const auto& slot : slots) {
            std::time_t start = 0;
            std::time_t end = 0;
            if (!parseIsoUtc(slot.getString("start"), start) || !parseIsoUtc(slot.getString("end"), end)) continue;
            if (end - start != duration || start < earliest || end > windowEnd) continue;
            std::string day = slot.getString("day");
            if (!validDay(day)) day = isoUtc(start).substr(0, 10);
            const std::string local = slot.getString("local");
            if (counts[day] >= page->maxPerDay || !freeOf(busy, start, end)) continue;
            const auto chosen = std::find_if(queue.begin(), queue.end(), [&](const WaitlistEntryRecord& entry) {
                return entry.id != excludeEntry && entry.status == "waiting" && matches(entry, day, local);
            });
            if (chosen == queue.end()) continue;
            WaitlistEntryRecord entry = *chosen;
            entry.status = "offered";
            entry.offerStart = isoUtc(start);
            entry.offerEnd = isoUtc(end);
            entry.offerDay = day;
            entry.offerLocal = validLocal(local) ? local : std::string();
            entry.offerExpiresAt = isoUtc(now + hold);
            entry.offerStatus = "offered";
            if (!database_.db().updateWaitlistEntry(entry).ok) {
                SELFVOY_LOG(log::Level::Warn, "booking") << "предложение листа ожидания " << entry.id << " не сохранено";
                continue;
            }
            busy.emplace_back(start, end);
            ++counts[day];
            queue.erase(chosen);
            offered.push_back(entry);
        }
    }

    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    for (const auto& entry : offered) {
        Json occurrence = Json::object();
        occurrence.set("start", Json(entry.offerStart));
        occurrence.set("local", Json(entry.offerLocal));
        std::time_t until = 0;
        parseIsoUtc(entry.offerExpiresAt, until);
        std::vector<std::string> lines{whenText(occurrence, ownerTimezone)};
        const std::string money = slotspost::moneyLabel(page->price, page->currency);
        if (!money.empty()) {
            lines.push_back("Стоимость: " + money + (page->prepay == "full"      ? " — нужна предоплата"
                                                     : page->prepay == "deposit" ? ", депозит " +
                                                                                       slotspost::moneyLabel(page->deposit,
                                                                                                             page->currency)
                                                                                 : std::string()));
        }
        const std::string link = waitlistUrl(waitlistTokenOf(entry));
        notifyWaitlist(entry, *page, "booking.waitlist_offer", "Selfvoy: освободилось время — " + page->title,
                       "Освободилось время",
                       ownerName + " может принять вас. Время держится за вами до " +
                           clockText(until, entry.offerStart, entry.offerLocal, ownerTimezone) +
                           " — успейте записаться.",
                       lines, link, "Записаться на это время",
                       {"Не подходит — ничего делать не нужно: время уйдёт следующему в листе ожидания, а вы "
                        "останетесь в листе. Покинуть лист можно по той же ссылке."});
    }
}

// ------------------------------------------------------------------ гость

BookingManager::Result BookingManager::waitlistJoin(const Json& payload, const std::string& remoteAddr) {
    const std::string ip = Session::ipOf(remoteAddr);
    if (limited("booking.wl:" + ip, kBookPerMinute) || limited("booking.wl.h:" + ip, kBookPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте позже");
    }
    const std::string slug = payload.getString("slug");
    const auto page = validSlug(slug) ? database_.db().findBookingPageBySlug(slug) : std::nullopt;
    if (!page || page->status == "archived" || !ownerAccepts(page->ownerId)) {
        return Result::failure(protocol::code::kNotFound, "страница не найдена");
    }
    if (!page->waitlist) return Result::failure(protocol::code::kForbidden, "лист ожидания на этой странице выключен");
    if (page->status != "active") return Result::failure(protocol::code::kConflict, "запись временно закрыта");

    WaitlistEntryRecord entry;
    entry.ownerId = page->ownerId;
    entry.pageId = page->id;
    entry.guestName = trim(payload.getString("guest_name"));
    if (entry.guestName.empty() || !validText(entry.guestName, 80, false)) {
        return Result::failure(protocol::code::kBadRequest, "укажите имя — до 80 символов");
    }
    entry.guestEmail = trim(payload.getString("guest_email"));
    if (entry.guestEmail.empty() || !plausibleEmail(entry.guestEmail)) {
        return Result::failure(protocol::code::kBadRequest, "укажите email — на него придёт предложение времени");
    }
    entry.comment = trim(payload.getString("comment"));
    if (!validText(entry.comment, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "комментарий — не больше 500 символов");
    }
    if (payload.contains("part_of_day") && !payload.get("part_of_day").isNull()) {
        entry.partOfDay = payload.getString("part_of_day");
        if (!validPart(entry.partOfDay)) {
            return Result::failure(protocol::code::kBadRequest, "part_of_day — any, morning, afternoon или evening");
        }
    }
    const std::time_t now = std::time(nullptr);
    if (payload.contains("days") && !payload.get("days").isNull()) {
        const Json days = payload.get("days");
        if (!days.isArray() || days.size() > static_cast<std::size_t>(kMaxWaitlistDays)) {
            return Result::failure(protocol::code::kBadRequest, "days — до 14 дат YYYY-MM-DD");
        }
        // Местные даты владельца: от вчера (по UTC — запас на пояс) до конца окна записи.
        const std::string first = isoUtc(now - 86400).substr(0, 10);
        const std::string last =
            isoUtc(now + static_cast<std::time_t>(page->daysAhead + 1) * 86400).substr(0, 10);
        std::set<std::string> unique;
        for (const auto& item : days.items()) {
            const std::string day = item.isString() ? item.asString() : std::string();
            if (!validDay(day)) return Result::failure(protocol::code::kBadRequest, "days — даты в формате YYYY-MM-DD");
            if (day < first || day > last) {
                return Result::failure(protocol::code::kBadRequest,
                                       "дата " + day + " — вне окна записи (" + std::to_string(page->daysAhead) + " дн.)");
            }
            unique.insert(day);
        }
        entry.days = Json::array();
        for (const auto& day : unique) entry.days.push(Json(day));
    }
    entry.expiresAt = isoUtc(now + static_cast<std::time_t>(kWaitlistLifeDays) * 86400);

    // Проверка «одна активная запись на email» и создание — под блокировкой расписания:
    // иначе два одновременных запроса создали бы две записи, а правка — затёрла бы предложение.
    bool created = false;
    std::string token;
    {
        auto lock = schedule_.acquire(page->ownerId);
        if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
        if (auto existing = database_.db().findActiveWaitlistEntry(page->id, lowerAscii(entry.guestEmail))) {
            // Повтор — обновляет пожелания и продлевает срок (место в очереди сохраняется).
            existing->guestName = entry.guestName;
            existing->days = entry.days;
            existing->partOfDay = entry.partOfDay;
            existing->comment = entry.comment;
            existing->expiresAt = entry.expiresAt;
            if (!database_.db().updateWaitlistEntry(*existing).ok) {
                return Result::failure(protocol::code::kInternal, "не удалось сохранить лист ожидания");
            }
            entry = *existing;
        } else {
            if (database_.db().countActiveWaitlistEntries(page->id) >= kMaxWaitlistPerPage) {
                return Result::failure(protocol::code::kForbidden, "лист ожидания этой страницы заполнен");
            }
            long long id = 0;
            for (int attempt = 0; attempt < 3 && id == 0; ++attempt) {
                token = newToken();
                entry.tokenHash = tokenHash(token);
                if (entry.tokenHash.empty()) continue;
                entry.tokenEnc = crypto::sealField(config_.dataKey(), token, kWaitlistTokenAad);
                const DatabaseError saved = database_.db().createWaitlistEntry(entry, id);
                if (!saved.ok) {
                    SELFVOY_LOG(log::Level::Warn, "booking") << "запись листа ожидания не сохранена: " << saved.message;
                    id = 0;
                }
            }
            if (id == 0) return Result::failure(protocol::code::kInternal, "не удалось сохранить лист ожидания");
            created = true;
            if (const auto stored = database_.db().findWaitlistEntry(id)) entry = *stored;
            entry.id = id;
        }
    }

    const std::string telegramUrl = waitlistTelegramUrl(entry);
    if (created) {
        if (notifications_) {
            Json notify = Json::object();
            notify.set("entry_id", Json(entry.id));
            notify.set("page_id", Json(page->id));
            notify.set("guest_name", Json(entry.guestName));
            notify.set("days", entry.days);
            notify.set("part_of_day", Json(entry.partOfDay));
            notifications_->create(page->ownerId, "waitlist.joined", entry.guestName + " встал(а) в лист ожидания",
                                   page->title, notify);
        }
        std::string when;
        for (const auto& day : entry.days.items()) when += (when.empty() ? "" : "; ") + dayLabel(day.asString());
        std::vector<std::string> footer;
        if (!telegramUrl.empty()) footer.push_back("Предложения в Telegram: " + telegramUrl);
        footer.push_back("Запись в листе действует " + daysText(kWaitlistLifeDays) + ". Покинуть лист — по ссылке выше.");
        notifyWaitlist(entry, *page, "booking.waitlist_joined", "Selfvoy: вы в листе ожидания — " + page->title,
                       "Вы в листе ожидания",
                       "Как только освободится подходящее время, мы напишем — оно будет держаться за вами " +
                           std::to_string(page->waitlistHoldMinutes) + " мин.",
                       {"Дни: " + (when.empty() ? std::string("любой") : when),
                        "Время: " + partLabel(entry.partOfDay)},
                       waitlistUrl(token), "Мой лист ожидания", footer);
    }
    Result result;
    result.ok = true;
    result.payload.set("entry", waitlistGuestJson(entry));
    result.payload.set("telegram_url", Json(telegramUrl));
    return result;
}

BookingManager::Result BookingManager::waitlistGet(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.manage:" + Session::ipOf(remoteAddr), kWaitlistGetPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto entry = hash.empty() ? std::nullopt : database_.db().findWaitlistEntryByToken(hash);
    const auto page = entry ? database_.db().findBookingPage(entry->pageId) : std::nullopt;
    if (!entry || !page) return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    const std::time_t now = std::time(nullptr);
    // Статус предложения для гостя: offered (держится сейчас) | claimed | expired | cancelled;
    // waiting — предложения ещё не было.
    std::string status = "waiting";
    std::time_t until = 0;
    if (entry->status == "offered") {
        status = parseIsoUtc(entry->offerExpiresAt, until) && until > now ? "offered" : "expired";
    } else if (entry->status == "booked") {
        status = "claimed";
    } else if (entry->status == "left") {
        status = "cancelled";
    } else if (entry->status == "expired" || entry->offerStatus == "expired") {
        status = "expired";
    }
    const bool shown = status != "waiting" && !entry->offerStart.empty();
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    Json offer = Json::object();
    offer.set("status", Json(status));
    offer.set("entry_status", Json(entry->status));
    offer.set("start", Json(shown ? entry->offerStart : std::string()));
    offer.set("end", Json(shown ? entry->offerEnd : std::string()));
    offer.set("local", Json(shown ? entry->offerLocal : std::string()));
    offer.set("expires_at", Json(shown ? entry->offerExpiresAt : std::string()));
    offer.set("page_title", Json(page->title));
    offer.set("page_slug", Json(page->status == "active" ? page->slug : std::string()));
    offer.set("owner_name", Json(ownerName));
    offer.set("owner_timezone", Json(ownerTimezone));
    offer.set("duration_minutes", Json(static_cast<long long>(page->durationMinutes)));
    offer.set("price", Json(page->price));
    offer.set("currency", Json(page->currency));
    offer.set("prepay", Json(page->prepay));
    offer.set("deposit", Json(page->deposit));
    offer.set("policy_text", Json(page->policyText));
    offer.set("guest_name", Json(entry->guestName));
    offer.set("days", entry->days.isArray() ? entry->days : Json::array());
    offer.set("part_of_day", Json(entry->partOfDay));
    offer.set("waitlist_expires_at", Json(entry->expiresAt));
    offer.set("telegram_url", Json(waitlistTelegramUrl(*entry)));
    offer.set("telegram_linked", Json(entry->telegramChatId != 0));
    offer.set("branding", branding(*page));
    offer.set("ref", Json(referrals_.codeFor(page->ownerId)));
    Result result;
    result.ok = true;
    result.payload.set("offer", offer);
    return result;
}

BookingManager::Result BookingManager::waitlistClaim(const Json& payload, const std::string& remoteAddr) {
    const std::string ip = Session::ipOf(remoteAddr);
    // Лимиты — как у booking.book (те же счётчики).
    if (limited("booking.book:" + ip, kBookPerMinute) || limited("booking.book.h:" + ip, kBookPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много записей, попробуйте позже");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto entry = hash.empty() ? std::nullopt : database_.db().findWaitlistEntryByToken(hash);
    const auto page = entry ? database_.db().findBookingPage(entry->pageId) : std::nullopt;
    if (!entry || !page || page->status == "archived" || !ownerAccepts(page->ownerId)) {
        return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    }
    if (entry->status == "booked") return Result::failure(protocol::code::kConflict, "вы уже записаны на это время");
    std::time_t until = 0;
    if (entry->status != "offered" || !parseIsoUtc(entry->offerExpiresAt, until) || until <= std::time(nullptr)) {
        return Result::failure(protocol::code::kConflict, "время больше не держится за вами — выберите другое");
    }
    if (page->status != "active") return Result::failure(protocol::code::kConflict, "запись временно закрыта");
    Offer offer;
    if (!parseIsoUtc(entry->offerStart, offer.start) || !parseIsoUtc(entry->offerEnd, offer.end)) {
        return Result::failure(protocol::code::kConflict, "время больше не держится за вами — выберите другое");
    }
    if (offer.end - offer.start != static_cast<std::time_t>(page->durationMinutes) * 60) {
        return Result::failure(protocol::code::kConflict, "длительность встречи изменилась — выберите время на странице");
    }
    offer.day = validDay(entry->offerDay) ? entry->offerDay : isoUtc(offer.start).substr(0, 10);
    offer.local = entry->offerLocal;

    // Тот же путь, что booking.book: имя, email и комментарий — из записи листа; цена —
    // снимок цены страницы; предоплата и способы оплаты — как у обычной записи.
    BookingRecord booking;
    booking.pageId = page->id;
    booking.ownerId = page->ownerId;
    booking.price = page->price;
    booking.currency = page->currency.empty() ? std::string("RUB") : page->currency;
    booking.guestName = entry->guestName;
    booking.guestEmail = entry->guestEmail;
    booking.guestComment = entry->comment;
    const std::vector<Offer> plan{offer};
    Result result = commitBooking(*page, std::move(booking), PackageIntent{}, plan, plan, 1, entry->id);
    if (!result.ok) return result;
    result.payload.set("manage_url", result.payload.get("url"));
    result.payload.set("payment", result.payload.get("booking").get("payment"));
    return result;
}

BookingManager::Result BookingManager::waitlistLeave(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kWaitlistChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto found = hash.empty() ? std::nullopt : database_.db().findWaitlistEntryByToken(hash);
    if (!found) return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    std::optional<Json> freed;
    std::optional<WaitlistEntryRecord> entry;
    mutateWaitlist(found->id, [&](WaitlistEntryRecord& record) {
        if (record.status != "waiting" && record.status != "offered") return false;  // уже закрыта
        if (record.status == "offered") {
            Json slot = Json::object();
            slot.set("start", Json(record.offerStart));
            slot.set("end", Json(record.offerEnd));
            slot.set("day", Json(record.offerDay));
            slot.set("local", Json(record.offerLocal));
            freed = slot;
            record.offerStatus = "cancelled";
        }
        record.status = "left";
        return true;
    }, entry);
    if (!entry) entry = database_.db().findWaitlistEntry(found->id);
    if (!entry) return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    if (freed) waitlistRelease(entry->pageId, {*freed}, std::time(nullptr), entry->id);
    Result result;
    result.ok = true;
    result.payload.set("entry", waitlistGuestJson(*entry));
    return result;
}

// ------------------------------------------------------------------ владелец

BookingManager::Result BookingManager::waitlistList(long long ownerId, const Json& payload) {
    long long limit = 100;
    if (!intField(payload, "limit", 1, kWaitlistListMax, limit)) {
        return Result::failure(protocol::code::kBadRequest, "limit — от 1 до 200");
    }
    long long pageId = 0;
    if (!intField(payload, "page_id", 0, 1LL << 53, pageId)) {
        return Result::failure(protocol::code::kBadRequest, "page_id — id страницы записи");
    }
    const std::string status = payload.getString("status");
    if (!status.empty() && !validEntryStatus(status)) {
        return Result::failure(protocol::code::kBadRequest, "status — waiting, offered, booked, left или expired");
    }
    std::map<long long, std::string> titles;
    Json entries = Json::array();
    for (const auto& entry : database_.db().listWaitlistEntries(ownerId, pageId, status, static_cast<int>(limit))) {
        auto title = titles.find(entry.pageId);
        if (title == titles.end()) {
            const auto page = database_.db().findBookingPage(entry.pageId);
            title = titles.emplace(entry.pageId, page ? page->title : std::string()).first;
        }
        entries.push(waitlistOwnerJson(entry, title->second));
    }
    Result result;
    result.ok = true;
    result.payload.set("entries", entries);
    return result;
}

BookingManager::Result BookingManager::waitlistRemove(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("entry_id");
    const auto found = id > 0 ? database_.db().findWaitlistEntry(id) : std::nullopt;
    if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    std::optional<Json> freed;
    std::optional<WaitlistEntryRecord> entry;
    mutateWaitlist(id, [&](WaitlistEntryRecord& record) {
        if (record.status != "waiting" && record.status != "offered") return false;  // уже закрыта — без изменений
        if (record.status == "offered") {
            Json slot = Json::object();
            slot.set("start", Json(record.offerStart));
            slot.set("end", Json(record.offerEnd));
            slot.set("day", Json(record.offerDay));
            slot.set("local", Json(record.offerLocal));
            freed = slot;
            record.offerStatus = "cancelled";
        }
        record.status = "left";
        return true;
    }, entry);
    if (!entry) entry = database_.db().findWaitlistEntry(id);
    if (!entry) return Result::failure(protocol::code::kNotFound, "запись листа ожидания не найдена");
    if (freed) waitlistRelease(entry->pageId, {*freed}, std::time(nullptr), entry->id);
    const auto page = database_.db().findBookingPage(entry->pageId);
    Result result;
    result.ok = true;
    result.payload.set("entry", waitlistOwnerJson(*entry, page ? page->title : std::string()));
    return result;
}

// ------------------------------------------------------------ планировщик

std::size_t BookingManager::waitlistTick(int limit, std::time_t now) {
    const auto due = database_.db().listDueWaitlist(isoUtc(now), limit);
    for (const auto& item : due) {
        std::optional<Json> freed;
        std::optional<WaitlistEntryRecord> entry;
        mutateWaitlist(item.id, [&](WaitlistEntryRecord& record) {
            std::time_t at = 0;
            if (record.status == "offered") {
                if (parseIsoUtc(record.offerExpiresAt, at) && at > now) return false;  // гость ещё успевает
                Json slot = Json::object();
                slot.set("start", Json(record.offerStart));
                slot.set("end", Json(record.offerEnd));
                slot.set("day", Json(record.offerDay));
                slot.set("local", Json(record.offerLocal));
                freed = slot;
                record.offerStatus = "expired";
                // Не забрал — обратно в лист, в конец очереди (или срок записи листа вышел).
                record.status = parseIsoUtc(record.expiresAt, at) && at <= now ? "expired" : "waiting";
                record.queuedAt = isoUtc(now);
                return true;
            }
            if (record.status == "waiting" && parseIsoUtc(record.expiresAt, at) && at <= now) {
                record.status = "expired";
                return true;
            }
            return false;
        }, entry);
        // Время — следующему в листе; не нашлось — просто освобождается.
        if (freed && entry) waitlistRelease(entry->pageId, {*freed}, now, entry->id);
    }
    return due.size();
}

std::size_t BookingManager::rebookTick(int limit, std::time_t now) {
    const auto claimed = database_.db().claimDueRebooks(isoUtc(now), limit);
    for (const auto& booking : claimed) {
        const bool sent = sendRebook(booking, now);
        database_.db().setRebookStatus(booking.id, sent ? "sent" : "skipped");
        if (!sent) SELFVOY_LOG(log::Level::Debug, "booking") << "повторный приём записи " << booking.id << " пропущен";
    }
    return claimed.size();
}

bool BookingManager::sendRebook(const BookingRecord& booking, std::time_t now) {
    if (booking.status != "active") return false;
    const auto page = database_.db().findBookingPage(booking.pageId);
    if (!page || page->rebookAfterDays <= 0 || page->status != "active" || !ownerAccepts(page->ownerId)) return false;
    // Последняя встреча записи (одиночной или последняя в серии).
    const Json* last = nullptr;
    std::time_t lastEnd = 0;
    for (const auto& occurrence : booking.occurrences.items()) {
        std::time_t end = 0;
        if (occurrence.getString("status") != "active" || !parseIsoUtc(occurrence.getString("end"), end)) continue;
        if (!last || end > lastEnd) {
            last = &occurrence;
            lastEnd = end;
        }
    }
    if (!last || lastEnd > now) return false;
    const std::time_t due = lastEnd + static_cast<std::time_t>(page->rebookAfterDays) * 86400;
    if (due > now || now - due > kRebookStaleSec) return false;  // сервер стоял дольше 3 дней — не шлём старое
    // У гостя есть запись этой страницы, которая заканчивается позже (будущая или более
    // поздняя прошедшая), — напоминание пришлёт она (или оно не нужно).
    if (!booking.guestEmail.empty() &&
        database_.db().hasLaterBooking(page->id, lowerAscii(booking.guestEmail), booking.id, isoUtc(lastEnd))) {
        return false;
    }
    GuestNotice notice;
    if (!guestRecipients(booking, *page, "booking.rebook", false, notice)) return false;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    const std::string pageUrl = config_.publicUrl.empty() ? std::string() : config_.publicUrl + "/#/b/" + page->slug;
    const std::string signature = brandingLine(*page);
    MailNotice& mail = notice.mail;
    mail.kind = "booking.rebook";
    mail.subject = "Selfvoy: пора записаться снова — " + page->title;
    mail.title = "Пора записаться снова";
    mail.lead = "С последней встречи прошло " + daysText(page->rebookAfterDays) + ". " + ownerName +
                " будет рад(а) видеть вас снова — выберите удобное время" +
                (pageUrl.empty() ? std::string(" на странице записи.") : ".");
    mail.lines.push_back("Последняя встреча: " + whenText(*last, ownerTimezone));
    if (!pageUrl.empty()) {
        mail.linkUrl = pageUrl;
        mail.linkText = "Записаться снова";
    }
    if (!signature.empty()) mail.footer.push_back(signature);
    mail.note = "Письмо пришло, потому что этот адрес указали при записи на странице «" + page->title + "» (" +
                ownerName + ") в Selfvoy. Напоминание приходит один раз.";
    if (!notice.chats.empty()) {
        std::string text = mail.title + "\n«" + page->title + "» — " + ownerName + "\n\n" + mail.lead;
        for (const auto& line : mail.lines) text += "\n• " + line;
        if (pageUrl.rfind("https://", 0) == 0) {
            Json button = Json::object();
            button.set("text", Json(std::string("Записаться снова")));
            button.set("url", Json(pageUrl));
            Json row = Json::array();
            row.push(button);
            Json keyboard = Json::array();
            keyboard.push(row);
            notice.telegramMarkup = Json::object();
            notice.telegramMarkup.set("inline_keyboard", keyboard);
        } else if (!pageUrl.empty()) {
            text += "\n\nЗаписаться снова: " + pageUrl;
        }
        if (!signature.empty()) text += "\n\n" + signature;
        notice.telegramText = text;
    }
    enqueueNotice(std::move(notice));
    return true;
}

}  // namespace selfvoy
