// selfvoy/bookingmanager.cpp
#include "selfvoy/bookingmanager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/mailprovider.h"
#include "selfvoy/memorymanager.h"
#include "selfvoy/net.h"
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


constexpr std::size_t kTokenBytes = 32;   // 256 бит случайности
constexpr std::size_t kTokenLength = 43;  // base64url без '=' от 32 байт
constexpr std::size_t kSlugLength = 10;
constexpr int kReminderLeadSec = 15 * 60;
constexpr int kWeekSec = 7 * 24 * 3600;
// Кандидатов в день от AI — не меньше максимума max_per_day страницы (16), иначе
// настройка 9–16 записей в день не работала бы; AI отдаёт ближайшие дни целиком.
constexpr int kAiPerDay = 16;
constexpr int kAiMaxSlots = 400;
constexpr int kCacheSeconds = 60;

// day/local от AI-сервиса — не источник истины: они должны соответствовать start
// (сдвиг пояса не больше 14 ч и кратен 15 мин; day — дата из local). Иначе
// day — дата UTC, local пуст: лимит записей в день не обойти подменой даты.
void checkLocal(std::time_t start, std::string& day, std::string& local) {
    std::time_t asUtc = 0;
    if (validLocal(local) && parseIsoUtc(local + ":00Z", asUtc)) {
        const long long offset = static_cast<long long>(asUtc - start);
        if (std::llabs(offset) <= 14 * 3600 && offset % 900 == 0) {
            day = local.substr(0, 10);
            return;
        }
    }
    local.clear();
    std::time_t midnight = 0;
    const bool plausible = validDay(day) && parseIsoUtc(day + "T00:00:00Z", midnight) &&
                           start - midnight >= -14 * 3600 && start - midnight < 38 * 3600;
    if (!plausible) day = isoUtc(start).substr(0, 10);
}

// Дата YYYY-MM-DD плюс days суток.
std::string addDays(const std::string& day, int days) {
    std::time_t base = 0;
    if (!parseIsoUtc(day + "T00:00:00Z", base)) return day;
    return isoUtc(base + static_cast<std::time_t>(days) * 86400).substr(0, 10);
}

bool isFree(const BookingManager::Busy& busy, std::time_t start, std::time_t end) {
    return std::none_of(busy.begin(), busy.end(),
                        [&](const auto& interval) { return start < interval.second && interval.first < end; });
}

Json occurrenceView(const Json& occurrence) {
    Json json = Json::object();
    json.set("start", Json(occurrence.getString("start")));
    json.set("end", Json(occurrence.getString("end")));
    json.set("status", Json(occurrence.getString("status")));
    json.set("local", Json(occurrence.getString("local")));
    json.set("confirmed_at", Json(occurrence.getString("confirmed_at")));
    return json;
}

// Сдвиг местного времени владельца от UTC по полю local встречи (проверено checkLocal
// при записи); false — local неизвестен.
bool localOffset(const Json& occurrence, std::time_t start, long long& offset) {
    const std::string local = occurrence.getString("local");
    std::time_t asUtc = 0;
    if (!validLocal(local) || !parseIsoUtc(local + ":00Z", asUtc)) return false;
    offset = static_cast<long long>(asUtc - start);
    return std::llabs(offset) <= 14 * 3600;
}

// «завтра в 15:00» / «сегодня в 15:00» / «21 октября в 15:00» — относительно now в том же
// поясе (местное время владельца или UTC).
std::string relativeWhen(std::time_t start, std::time_t now, long long offset) {
    static const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                          "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
    const std::time_t localStart = start + static_cast<std::time_t>(offset);
    const std::time_t localNow = now + static_cast<std::time_t>(offset);
    std::tm startParts{};
    std::tm nowParts{};
    gmtime_r(&localStart, &startParts);
    gmtime_r(&localNow, &nowParts);
    char clock[8];
    std::snprintf(clock, sizeof clock, "%02d:%02d", startParts.tm_hour, startParts.tm_min);
    const long long days = static_cast<long long>(localStart / 86400) - static_cast<long long>(localNow / 86400);
    if (days == 0) return std::string("сегодня в ") + clock;
    if (days == 1) return std::string("завтра в ") + clock;
    return std::to_string(startParts.tm_mday) + " " + kMonths[startParts.tm_mon] + " в " + clock;
}

// ------------------------------------------------------------ оплата (F4)

// Ссылка оплаты владельца: только https://, без пробелов и управляющих символов, ≤ 500.
bool validPaymentUrl(const std::string& url) {
    if (url.size() > 500 || url.size() <= 8 || url.rfind("https://", 0) != 0) return false;
    return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
        return ch <= 0x20 || ch == 0x7F || ch == '"' || ch == '<' || ch == '>' || ch == '\\';
    });
}

std::string lateMessage(const BookingPageRecord& page) {
    return "до встречи меньше " + std::to_string(page.cancelHours) + " ч — свяжитесь со специалистом";
}

// Политика отмены для гостя: {cancel_hours, late_cancel, text}.
Json policyJson(const BookingPageRecord& page) {
    Json json = Json::object();
    json.set("cancel_hours", Json(static_cast<long long>(page.cancelHours)));
    json.set("late_cancel", Json(page.lateCancel));
    json.set("text", Json(page.policyText));
    return json;
}

}  // namespace

BookingManager::BookingManager(DatabaseManager& database, MemoryManager& memory, NotificationsManager* notifications,
                               ReferralManager& referrals, ScheduleLock& scheduleLock, const Config& config)
    : database_(database),
      memory_(memory),
      notifications_(notifications),
      referrals_(referrals),
      config_(config),
      mail_(makeMailProvider(config)),
      schedule_(scheduleLock) {}

BookingManager::~BookingManager() {
    {
        std::lock_guard<std::mutex> lock(mailMutex_);
        mailStop_ = true;
    }
    mailCv_.notify_all();
    if (mailThread_.joinable()) mailThread_.join();  // очередь дописывается до конца
}

void BookingManager::mailLoop() {
    std::unique_lock<std::mutex> lock(mailMutex_);
    for (;;) {
        mailCv_.wait(lock, [this] { return mailStop_ || !mailQueue_.empty(); });
        if (mailQueue_.empty()) return;  // mailStop_ и всё отправлено
        GuestNotice notice = std::move(mailQueue_.front());
        mailQueue_.pop_front();
        mailBusy_ = true;
        lock.unlock();
        for (const auto& address : notice.emails) {
            MailNotice mail = notice.mail;
            mail.to = address;
            if (!mail_ || !mail_->sendNotice(mail)) {
                SELFVOY_LOG(log::Level::Warn, "booking") << "письмо гостю (" << mail.kind << ") не отправлено";
            }
        }
        // Telegram — своя очередь бота: здесь только постановка, без ожидания сети.
        if (telegram_) {
            for (const long long chat : notice.chats) telegram_->sendMessage(chat, notice.telegramText, notice.telegramMarkup);
        }
        lock.lock();
        mailBusy_ = false;
        mailCv_.notify_all();
    }
}

void BookingManager::flushMail() {
    std::unique_lock<std::mutex> lock(mailMutex_);
    mailCv_.wait(lock, [this] { return mailQueue_.empty() && !mailBusy_; });
}

bool BookingManager::ownerAccepts(long long ownerId) const {
    const auto owner = database_.db().findUserById(ownerId);
    return owner && owner->active && owner->emailVerified;
}

void BookingManager::deleteOwnTask(long long ownerId, long long taskId) {
    if (taskId <= 0) return;
    const auto task = database_.db().findTask(taskId);
    if (task && task->userId == ownerId) database_.db().deleteTask(taskId);
}

// Ключ лимита писем: один почтовый ящик — один ключ. Регистр, «+метка» и точки
// в Gmail не дают обойти лимит на адрес.
std::string BookingManager::mailboxKey(const std::string& email) {
    std::string address = email;
    std::transform(address.begin(), address.end(), address.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::size_t at = address.rfind('@');
    if (at == std::string::npos) return address;
    std::string local = address.substr(0, at);
    const std::string domain = address.substr(at + 1);
    if (const std::size_t plus = local.find('+'); plus != std::string::npos) local.resize(plus);
    if (domain == "gmail.com" || domain == "googlemail.com") {
        local.erase(std::remove(local.begin(), local.end(), '.'), local.end());
        return local + "@gmail.com";
    }
    return local + "@" + domain;
}

std::string BookingManager::manageUrl(const std::string& token) const {
    return config_.publicUrl.empty() || token.empty() ? std::string() : config_.publicUrl + "/#/m/" + token;
}

bool BookingManager::guestRecipients(const BookingRecord& booking, const BookingPageRecord& page,
                                     const std::string& kind, bool reserved, GuestNotice& notice) {
    // Лимиты на адрес/чат и на владельца: форма записи не должна превращаться в
    // рассылку на чужой email, а страница — в канал рассылки от имени Selfvoy.
    // Email гостя и второго получателя (F5, cc_email) — у каждого адреса свой лимит;
    // reserved относится только к адресу гостя (его лимит уже списан).
    const auto addEmail = [&](const std::string& email, bool alreadyCounted) {
        if (email.empty() || !mail_) return;
        if (alreadyCounted || !limited("booking.mail:" + mailboxKey(email), kMailPerAddressHour, 3600)) {
            notice.emails.push_back(email);
        } else {
            SELFVOY_LOG(log::Level::Warn, "booking") << "лимит писем на адрес исчерпан, письмо (" << kind << ") не отправлено";
        }
    };
    addEmail(booking.guestEmail, reserved);
    if (lowerAscii(booking.ccEmail) != lowerAscii(booking.guestEmail)) addEmail(booking.ccEmail, false);
    if (telegram_ && telegram_->enabled() && booking.id > 0) {
        for (const long long chat : database_.db().bookingTelegramChats(booking.id)) {
            if (limited("booking.tg:" + std::to_string(chat), kMailPerAddressHour, 3600)) {
                SELFVOY_LOG(log::Level::Warn, "booking") << "лимит сообщений в чат исчерпан (" << kind << ")";
                continue;
            }
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

void BookingManager::enqueueNotice(GuestNotice notice) {
    std::lock_guard<std::mutex> lock(mailMutex_);
    if (mailStop_) return;
    mailQueue_.push_back(std::move(notice));
    if (!mailThread_.joinable()) mailThread_ = std::thread([this] { mailLoop(); });
    mailCv_.notify_all();
}

Json BookingManager::branding(const BookingPageRecord& page) {
    // Скрытие действует, только пока у владельца Pro (или тарифы не включены).
    const bool show = !page.hideBranding || !plan::proAllowed(database_.db(), page.ownerId);
    std::string url;
    if (!config_.publicUrl.empty()) {
        const std::string code = referrals_.codeFor(page.ownerId);
        url = config_.publicUrl + "/#/signup" + (code.empty() ? std::string("?src=booking")
                                                               : "?ref=" + code + "&src=booking");
    }
    Json json = Json::object();
    json.set("show", Json(show));
    json.set("text", Json(std::string("Запись через Selfvoy")));
    json.set("url", Json(url));
    return json;
}

std::string BookingManager::brandingLine(const BookingPageRecord& page) {
    const Json info = branding(page);
    if (!info.getBool("show") || info.getString("url").empty()) return "";
    return "Запись через Selfvoy — заведите свой бесплатно: " + info.getString("url");
}

void BookingManager::notifyGuest(const BookingRecord& booking, const BookingPageRecord& page, const std::string& kind,
                                 const std::string& subject, const std::string& title, const std::string& lead,
                                 const std::string& token, bool onlyActive, bool reserved,
                                 const std::vector<std::string>& footer) {
    GuestNotice notice;
    if (!guestRecipients(booking, page, kind, reserved, notice)) return;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    const std::time_t now = std::time(nullptr);
    MailNotice& mail = notice.mail;
    mail.kind = kind;
    mail.subject = subject;
    mail.title = title;
    mail.lead = lead;
    for (const auto& occurrence : booking.occurrences.items()) {
        std::time_t start = 0;
        if (!parseIsoUtc(occurrence.getString("start"), start)) continue;
        const bool active = occurrence.getString("status") == "active";
        if (onlyActive && (!active || start <= now)) continue;
        mail.lines.push_back(whenText(occurrence, ownerTimezone) + (active ? "" : " — отменена"));
    }
    const std::string link = manageUrl(token);
    if (!link.empty()) {
        mail.linkUrl = link;
        mail.linkText = "Перенести или отменить";
    } else if ((kind == "booking.owner_cancelled" || kind == "booking.payment_expired") && page.status == "active" &&
               !config_.publicUrl.empty()) {
        mail.linkUrl = config_.publicUrl + "/#/b/" + page.slug;
        mail.linkText = "Выбрать другое время";
    }
    mail.footer = footer;
    // F3: последняя строка — подпись Selfvoy (если владелец её не скрыл).
    const std::string signature = brandingLine(page);
    if (!signature.empty()) mail.footer.push_back(signature);
    mail.note = "Письмо пришло, потому что этот адрес указали при записи на странице «" + page.title +
                "» (" + ownerName + ") в Selfvoy." + (link.empty() ? "" : " Ссылка — только для вас, не пересылайте её.");

    if (!notice.chats.empty()) {
        // Telegram — простой текст (без parse_mode), ссылка — url-кнопкой, если она https.
        std::string text = title + "\n«" + page.title + "» — " + ownerName + "\n\n" + lead;
        for (const auto& line : mail.lines) text += "\n• " + line;
        if (!mail.linkUrl.empty()) {
            if (mail.linkUrl.rfind("https://", 0) == 0) {
                Json button = Json::object();
                button.set("text", Json(mail.linkText));
                button.set("url", Json(mail.linkUrl));
                Json row = Json::array();
                row.push(button);
                Json keyboard = Json::array();
                keyboard.push(row);
                notice.telegramMarkup = Json::object();
                notice.telegramMarkup.set("inline_keyboard", keyboard);
            } else {
                text += "\n\n" + mail.linkText + ": " + mail.linkUrl;
            }
        }
        // Строки footer (ссылка Telegram и т.п.) в бот не уходят — только подпись.
        if (!signature.empty()) text += "\n\n" + signature;
        notice.telegramText = text;
    }
    enqueueNotice(std::move(notice));
}

std::string BookingManager::sealManageToken(const std::string& token) const {
    return crypto::sealField(config_.dataKey(), token, kManageTokenAad);
}

std::string BookingManager::manageTokenOf(const BookingRecord& booking) const {
    std::string token;
    if (booking.manageTokenEnc.empty() ||
        !crypto::openField(config_.dataKey(), booking.manageTokenEnc, kManageTokenAad, token)) {
        return "";
    }
    // Шифр от прежней ссылки (перевыпуск не записался) — не выдаём недействующий токен.
    return tokenHash(token) == booking.manageHash ? token : std::string();
}

std::string BookingManager::telegramGreeting(const BookingRecord& booking) {
    const auto page = database_.db().findBookingPage(booking.pageId);
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(booking.ownerId, ownerName, ownerTimezone);
    std::string text = "Готово: напоминания о записи «" + (page ? page->title : std::string("Selfvoy")) + "» (" +
                       ownerName + ") будут приходить сюда.";
    const int nearest = findOccurrence(booking, "", std::time(nullptr));
    if (nearest >= 0) {
        text += "\nБлижайшая встреча: " +
                whenText(booking.occurrences.items()[static_cast<std::size_t>(nearest)], ownerTimezone) + ".";
    }
    text += "\nНапомню за сутки и за 2 часа. Отписаться — /stop";
    if (page) {
        const std::string signature = brandingLine(*page);
        if (!signature.empty()) text += "\n\n" + signature;
    }
    return text;
}

BookingManager::Result BookingManager::telegramConfirm(long long bookingId, std::time_t start) {
    if (bookingId <= 0 || start <= 0) return Result::failure(protocol::code::kBadRequest, "Кнопка устарела");
    bool changed = false;
    std::optional<BookingRecord> booking;
    return confirmOccurrence(bookingId, "", start, changed, booking);
}

BookingManager::Result BookingManager::confirmOccurrence(long long bookingId, const std::string& hash,
                                                         std::time_t start, bool& changed,
                                                         std::optional<BookingRecord>& out) {
    changed = false;
    const auto lookup = [&]() {
        return bookingId > 0 ? database_.db().findBooking(bookingId)
                             : (hash.empty() ? std::nullopt : database_.db().findBookingByToken(hash));
    };
    auto booking = lookup();
    if (!booking) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    // confirmed_at — часть occurrences: запись перезаписывается целиком, поэтому под той же
    // блокировкой расписания, что перенос и отмена (иначе одно из изменений потерялось бы).
    auto lock = schedule_.acquire(booking->ownerId);
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    booking = lookup();
    if (!booking) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (booking->status != "active") return Result::failure(protocol::code::kConflict, "запись отменена");
    const std::time_t now = std::time(nullptr);
    const int index = findOccurrence(*booking, start > 0 ? isoUtc(start) : std::string(), now);
    if (index < 0) {
        return start > 0 ? Result::failure(protocol::code::kConflict, "встреча уже прошла или отменена")
                         : Result::failure(protocol::code::kConflict, "предстоящих встреч нет");
    }
    const auto& items = booking->occurrences.items();
    Json occurrence = items[static_cast<std::size_t>(index)];
    if (occurrence.getString("confirmed_at").empty()) {
        occurrence.set("confirmed_at", Json(isoUtc(now)));
        Json updated = Json::array();
        for (std::size_t i = 0; i < items.size(); ++i) updated.push(static_cast<int>(i) == index ? occurrence : items[i]);
        booking->occurrences = updated;
        if (!database_.db().updateBooking(*booking).ok) {
            return Result::failure(protocol::code::kInternal, "не удалось сохранить запись");
        }
        changed = true;
    }
    lock.release();
    if (changed && notifications_) {
        const auto page = database_.db().findBookingPage(booking->pageId);
        Json notify = Json::object();
        notify.set("booking_id", Json(booking->id));
        notify.set("page_id", Json(booking->pageId));
        notify.set("start", Json(occurrence.getString("start")));
        notify.set("guest_name", Json(booking->guestName));
        notifications_->create(booking->ownerId, "booking.confirmed", booking->guestName + " подтвердил(а) встречу",
                               page ? page->title : std::string(), notify);
    }
    out = std::move(booking);
    Result result;
    result.ok = true;
    return result;
}

BookingManager::Result BookingManager::confirm(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    if (hash.empty()) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    std::time_t start = 0;
    const std::string occurrence = payload.getString("occurrence");
    if (!occurrence.empty() && !parseIsoUtc(occurrence, start)) {
        return Result::failure(protocol::code::kBadRequest, "occurrence — время встречи (ISO 8601)");
    }
    bool changed = false;
    std::optional<BookingRecord> booking;
    Result result = confirmOccurrence(0, hash, start, changed, booking);
    if (!result.ok) return result;
    const auto page = booking ? database_.db().findBookingPage(booking->pageId) : std::nullopt;
    if (!booking || !page) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    result.payload.set("booking", guestBookingJson(*booking, *page, false));
    return result;
}

// ------------------------------------------------------------ напоминания (F1)

void BookingManager::scheduleReminders(const BookingRecord& booking, std::time_t now) {
    if (booking.id <= 0 || booking.status != "active") return;
    std::vector<BookingReminderRecord> reminders;
    for (const auto& occurrence : booking.occurrences.items()) {
        std::time_t start = 0;
        if (occurrence.getString("status") != "active" || !parseIsoUtc(occurrence.getString("start"), start)) continue;
        const long long lead = static_cast<long long>(start - now);
        BookingReminderRecord reminder;
        reminder.bookingId = booking.id;
        reminder.ownerId = booking.ownerId;
        reminder.occurrenceStart = isoUtc(start);
        if (lead >= kReminder24hMinLeadSec) {
            reminder.kind = "24h";
            reminder.dueAt = isoUtc(start - kReminder24hSec);
            reminders.push_back(reminder);
        }
        if (lead >= kReminder2hMinLeadSec) {
            reminder.kind = "2h";
            reminder.dueAt = isoUtc(start - kReminder2hSec);
            reminders.push_back(reminder);
        }
    }
    if (reminders.empty()) return;
    const DatabaseError saved = database_.db().addBookingReminders(reminders);
    if (!saved.ok) SELFVOY_LOG(log::Level::Warn, "booking") << "напоминания записи " << booking.id << " не сохранены";
}

void BookingManager::dropInactiveReminders(const BookingRecord& booking) {
    if (booking.id <= 0) return;
    if (booking.status != "active") {
        database_.db().deleteBookingReminders(booking.id, "");
        return;
    }
    for (const auto& occurrence : booking.occurrences.items()) {
        std::time_t start = 0;
        if (occurrence.getString("status") == "active" || !parseIsoUtc(occurrence.getString("start"), start)) continue;
        database_.db().deleteBookingReminders(booking.id, isoUtc(start));
    }
}

std::size_t BookingManager::reminderTick(int limit, std::time_t now) {
    const auto claimed = database_.db().claimDueReminders(isoUtc(now), limit);
    for (const auto& reminder : claimed) {
        if (!sendReminder(reminder, now)) {
            SELFVOY_LOG(log::Level::Debug, "booking") << "напоминание " << reminder.id << " пропущено";
        }
    }
    return claimed.size();
}

bool BookingManager::sendReminder(const BookingReminderRecord& reminder, std::time_t now) {
    const auto booking = database_.db().findBooking(reminder.bookingId);
    if (!booking || booking->status != "active") return false;
    std::time_t start = 0;
    if (!parseIsoUtc(reminder.occurrenceStart, start) || start <= now) return false;  // встреча уже началась
    // Сервер стоял и пропустил «за сутки» — достаточно «за 2 часа».
    if (reminder.kind == "24h" && start - now <= kReminder2hSec) return false;
    const Json* found = nullptr;
    for (const auto& occurrence : booking->occurrences.items()) {
        std::time_t at = 0;
        if (occurrence.getString("status") == "active" && parseIsoUtc(occurrence.getString("start"), at) &&
            at == start) {
            found = &occurrence;
            break;
        }
    }
    if (!found) return false;  // встречу отменили или перенесли
    const auto page = database_.db().findBookingPage(booking->pageId);
    if (!page || !page->guestReminders) return false;
    const auto owner = database_.db().findUserById(booking->ownerId);
    if (!owner || !owner->active) return false;

    GuestNotice notice;
    if (!guestRecipients(*booking, *page, "booking.reminder", false, notice)) return false;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    long long offset = 0;
    std::string zone = ownerTimezone;
    if (!localOffset(*found, start, offset)) {
        offset = 0;
        zone = "UTC";
    }
    const std::string when = relativeWhen(start, now, offset);

    // Ссылки: «Приду» и «Перенести или отменить» — только с токеном (старые записи его не
    // хранят) и адресом веб-клиента.
    const std::string token = manageTokenOf(*booking);
    const std::string manage = manageUrl(token);
    const std::string confirmLink = manage.empty() ? std::string() : manage + "?confirm=" + isoUtc(start);
    const std::string fallback = "Перенести или отменить можно по ссылке из письма о записи";
    const std::string signature = brandingLine(*page);

    MailNotice& mail = notice.mail;
    mail.kind = "booking.reminder";
    mail.subject = "Напоминание: " + when + " — " + page->title;
    mail.title = "Напоминание о встрече";
    mail.lead = ownerName + " ждёт вас " + when + " (" + zone + ").";
    mail.lines.push_back(whenText(*found, ownerTimezone));
    if (!confirmLink.empty()) {
        mail.linkUrl = confirmLink;
        mail.linkText = "Приду";
        mail.footer.push_back("Перенести или отменить: " + manage);
    } else {
        mail.footer.push_back(fallback);
    }
    if (!signature.empty()) mail.footer.push_back(signature);
    mail.note = "Письмо пришло, потому что этот адрес указали при записи на странице «" + page->title + "» (" +
                ownerName + ") в Selfvoy." + (manage.empty() ? "" : " Ссылка — только для вас, не пересылайте её.");

    if (!notice.chats.empty()) {
        std::string text = mail.title + "\n«" + page->title + "» — " + ownerName + "\n\n" + mail.lead;
        for (const auto& line : mail.lines) text += "\n• " + line;
        Json confirmButton = Json::object();
        confirmButton.set("text", Json(std::string("Приду")));
        confirmButton.set("callback_data",
                          Json("cf:" + std::to_string(booking->id) + ":" + std::to_string(static_cast<long long>(start))));
        Json firstRow = Json::array();
        firstRow.push(confirmButton);
        Json keyboard = Json::array();
        keyboard.push(firstRow);
        if (manage.rfind("https://", 0) == 0) {
            Json manageButton = Json::object();
            manageButton.set("text", Json(std::string("Перенести или отменить")));
            manageButton.set("url", Json(manage));
            Json secondRow = Json::array();
            secondRow.push(manageButton);
            keyboard.push(secondRow);
        } else if (!manage.empty()) {
            text += "\n\nПеренести или отменить: " + manage;
        } else {
            text += "\n\n" + fallback;
        }
        if (!signature.empty()) text += "\n\n" + signature;
        notice.telegramText = text;
        notice.telegramMarkup = Json::object();
        notice.telegramMarkup.set("inline_keyboard", keyboard);
    }
    enqueueNotice(std::move(notice));
    return true;
}

std::string BookingManager::tokenHash(const std::string& token) {
    if (token.size() != kTokenLength) return "";
    for (const unsigned char ch : token) {
        if (std::isalnum(ch) == 0 && ch != '-' && ch != '_') return "";
    }
    return crypto::toHex(crypto::sha256(token));
}

bool BookingManager::validSlug(const std::string& slug) {
    if (slug.size() != kSlugLength) return false;
    return std::all_of(slug.begin(), slug.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    });
}

bool BookingManager::underLimit(const std::string& key, int limit, int windowSeconds) {
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitCount(key) < limit;
    std::lock_guard<std::mutex> lock(limitsMutex_);
    const auto it = limits_.find(key);
    return it == limits_.end() ||
           std::chrono::steady_clock::now() - it->second.start > std::chrono::seconds(windowSeconds) ||
           it->second.count < limit;
}

bool BookingManager::limited(const std::string& key, int perWindow, int windowSeconds) {
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitHit(key, windowSeconds) > perWindow;
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(windowSeconds);
    std::lock_guard<std::mutex> lock(limitsMutex_);
    if (limits_.size() > 10000) {
        for (auto it = limits_.begin(); it != limits_.end();) {
            it = now - it->second.start > std::chrono::hours(1) ? limits_.erase(it) : std::next(it);
        }
    }
    auto& entry = limits_[key];
    if (entry.count == 0 || now - entry.start > window) {
        entry.start = now;
        entry.count = 0;
    }
    if (entry.count >= perWindow) return true;
    ++entry.count;
    return false;
}

void BookingManager::ownerInfo(long long ownerId, std::string& name, std::string& timezone) const {
    name = "Пользователь Selfvoy";
    timezone = "UTC";
    if (const auto owner = database_.db().findUserById(ownerId)) {
        if (!owner->displayName.empty()) name = owner->displayName;
        if (!owner->timezone.empty()) timezone = owner->timezone;
    }
}

// ------------------------------------------------------------------ виды

Json BookingManager::pageJson(const BookingPageRecord& page) {
    const std::time_t now = std::time(nullptr);
    long long upcoming = 0;
    for (const auto& booking : database_.db().listUpcomingBookings(page.ownerId, page.id, 5000)) {
        for (const auto& occurrence : booking.occurrences.items()) {
            std::time_t start = 0;
            if (occurrence.getString("status") == "active" && parseIsoUtc(occurrence.getString("start"), start) &&
                start > now) {
                ++upcoming;
            }
        }
    }
    Json json = Json::object();
    json.set("id", Json(page.id));
    json.set("slug", Json(page.slug));
    json.set("url", Json(config_.publicUrl.empty() ? std::string() : config_.publicUrl + "/#/b/" + page.slug));
    json.set("title", Json(page.title));
    json.set("description", Json(page.description));
    json.set("duration_minutes", Json(static_cast<long long>(page.durationMinutes)));
    json.set("days_ahead", Json(static_cast<long long>(page.daysAhead)));
    json.set("max_per_day", Json(static_cast<long long>(page.maxPerDay)));
    json.set("notice_hours", Json(static_cast<long long>(page.noticeHours)));
    json.set("max_series", Json(static_cast<long long>(page.maxSeries)));
    json.set("weekends", Json(page.weekends));
    json.set("status", Json(page.status));
    json.set("guest_reminders", Json(page.guestReminders));
    json.set("hide_branding", Json(page.hideBranding));
    json.set("price", Json(page.price));
    json.set("currency", Json(page.currency));
    json.set("prepay", Json(page.prepay));
    json.set("deposit", Json(page.deposit));
    json.set("payment_url", Json(page.paymentUrl));
    json.set("payment_note", Json(page.paymentNote));
    json.set("pay_within_minutes", Json(static_cast<long long>(page.payWithinMinutes)));
    json.set("cancel_hours", Json(static_cast<long long>(page.cancelHours)));
    json.set("late_cancel", Json(page.lateCancel));
    json.set("policy_text", Json(page.policyText));
    json.set("packages", page.packages.isArray() ? page.packages : Json::array());  // F5: предложения пакетов
    // F8: лист ожидания и «пора на повторный приём».
    json.set("waitlist", Json(page.waitlist));
    json.set("waitlist_hold_minutes", Json(static_cast<long long>(page.waitlistHoldMinutes)));
    json.set("rebook_after_days", Json(static_cast<long long>(page.rebookAfterDays)));
    json.set("ai_admin", Json(page.aiAdmin));                                      // F7
    json.set("telegram_ask_url", Json(telegramAskUrl(page)));
    json.set("upcoming", Json(upcoming));
    json.set("created_at", Json(page.createdAt));
    json.set("updated_at", Json(page.updatedAt));
    return json;
}

Json BookingManager::bookingJson(const BookingRecord& booking, const std::string& pageTitle, int rebookDays) const {
    Json occurrences = Json::array();
    for (const auto& occurrence : booking.occurrences.items()) occurrences.push(occurrenceView(occurrence));
    Json json = Json::object();
    json.set("id", Json(booking.id));
    json.set("page_id", Json(booking.pageId));
    json.set("page_title", Json(pageTitle));
    json.set("guest_name", Json(booking.guestName));
    json.set("guest_email", Json(booking.guestEmail));
    json.set("guest_comment", Json(booking.guestComment));
    json.set("cc_email", Json(booking.ccEmail));        // F5: второй получатель
    json.set("package_id", Json(booking.packageId));    // F5: запись по пакету (0 — нет)
    // Цена одной встречи на момент записи (0 — по пакету или бесплатно); payment.amount —
    // сколько нужно внести онлайн (депозит или полная предоплата).
    json.set("price", Json(booking.price));
    json.set("currency", Json(booking.currency));
    json.set("status", Json(booking.status));
    json.set("cancelled_by", Json(booking.cancelledBy));
    json.set("reschedules", Json(static_cast<long long>(booking.reschedules)));
    json.set("occurrences", occurrences);
    // F1: запланированные и отправленные напоминания гостю (для прозрачности).
    Json reminders = Json::array();
    if (booking.id > 0) {
        for (const auto& reminder : database_.db().listBookingReminders(booking.id)) {
            Json item = Json::object();
            item.set("start", Json(reminder.occurrenceStart));
            item.set("kind", Json(reminder.kind));
            item.set("due_at", Json(reminder.dueAt));
            item.set("sent_at", Json(reminder.sentAt));
            reminders.push(item);
        }
    }
    json.set("reminders", reminders);
    json.set("payment", booking.payment.isObject() ? booking.payment : Json());  // F4: целиком
    // F8: «пора записаться снова» — когда уйдёт ("" — не запланировано: поле страницы
    // выключено, запись отменена или уже обработана) и когда ушло ("" — не отправлялось).
    std::string rebookDue;
    const std::string endsAt = bookingEndsAt(booking.occurrences);
    std::time_t ends = 0;
    if (rebookDays > 0 && booking.status == "active" && booking.rebookSentAt.empty() && parseIsoUtc(endsAt, ends)) {
        rebookDue = isoUtc(ends + static_cast<std::time_t>(rebookDays) * 86400);
    }
    json.set("rebook_due_at", Json(rebookDue));
    json.set("rebook_sent_at", Json(booking.rebookStatus == "sent" ? booking.rebookSentAt : std::string()));
    json.set("created_at", Json(booking.createdAt));
    json.set("updated_at", Json(booking.updatedAt));
    return json;
}

// Вид для гостя по ссылке управления: без email владельца и данных других гостей.
Json BookingManager::guestBookingJson(const BookingRecord& booking, const BookingPageRecord& page, bool withSlots) {
    const std::time_t now = std::time(nullptr);
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    Json occurrences = Json::array();
    bool upcoming = false;
    for (const auto& occurrence : booking.occurrences.items()) {
        occurrences.push(occurrenceView(occurrence));
        std::time_t start = 0;
        if (occurrence.getString("status") == "active" && parseIsoUtc(occurrence.getString("start"), start) &&
            start > now) {
            upcoming = true;
        }
    }
    const int left = std::max(0, kMaxReschedules - booking.reschedules);
    // F4: поздно ли менять ближайшую встречу (до начала меньше cancel_hours).
    bool late = false;
    if (booking.status == "active") {
        const int nearest = findOccurrence(booking, "", now);
        std::time_t nearestStart = 0;
        late = nearest >= 0 &&
               parseIsoUtc(booking.occurrences.items()[static_cast<std::size_t>(nearest)].getString("start"),
                           nearestStart) &&
               lateFor(page, nearestStart, now);
    }
    const bool blocked = late && page.lateCancel == "block";
    // Отменить можно всегда, пока есть предстоящая встреча (и на паузе, и после удаления
    // страницы); перенести — только пока запись на странице открыта и переносы остались.
    // При late_cancel=block позже срока — ни то, ни другое.
    const bool canCancel = booking.status == "active" && upcoming && !blocked;
    const bool canReschedule = canCancel && page.status == "active" && left > 0;
    Json json = Json::object();
    json.set("page_title", Json(page.title));
    json.set("page_slug", Json(page.status == "active" ? page.slug : std::string()));
    json.set("owner_name", Json(ownerName));
    json.set("owner_timezone", Json(ownerTimezone));
    json.set("duration_minutes", Json(static_cast<long long>(page.durationMinutes)));
    json.set("guest_name", Json(booking.guestName));
    json.set("cc_email", Json(maskEmail(booking.ccEmail)));  // F5: замаскирован
    json.set("by_package", Json(booking.packageId > 0));
    json.set("price", Json(booking.price));  // цена встречи на момент записи (0 — по пакету/бесплатно)
    json.set("currency", Json(booking.currency));
    json.set("status", Json(booking.status));
    json.set("cancelled_by", Json(booking.cancelledBy));
    json.set("occurrences", occurrences);
    json.set("can_cancel", Json(canCancel));
    json.set("can_reschedule", Json(canReschedule));
    json.set("can_change", Json(canReschedule));  // устаревшее: = can_reschedule (старые клиенты)
    json.set("reschedules_left", Json(static_cast<long long>(left)));
    Json slots = Json::array();
    bool slotsUnavailable = false;
    if (withSlots && canReschedule) {
        std::vector<Offer> candidates;
        if (offers(page, candidates)) {
            // Слоты для переноса ближайшей встречи (её booking.reschedule двигает по умолчанию):
            // её собственный интервал и место в лимите дня не мешают.
            const int nearest = findOccurrence(booking, "", now);
            const std::string skipStart =
                nearest >= 0 ? booking.occurrences.items()[static_cast<std::size_t>(nearest)].getString("start")
                             : std::string();
            for (const auto& offer : available(page, candidates, booking.id, skipStart)) {
                Json slot = Json::object();
                slot.set("start", Json(isoUtc(offer.start)));
                slot.set("end", Json(isoUtc(offer.end)));
                slots.push(slot);
            }
        } else {
            slotsUnavailable = true;
        }
    }
    json.set("slots", slots);
    json.set("slots_unavailable", Json(slotsUnavailable));
    json.set("ref", Json(referrals_.codeFor(page.ownerId)));
    json.set("branding", branding(page));
    // Telegram (F2): ссылка подписки на напоминания и подключён ли уже чат.
    json.set("telegram_url", Json(telegram_ ? telegram_->guestUrl(booking) : std::string()));
    json.set("telegram_linked", Json(booking.id > 0 && !database_.db().bookingTelegramChats(booking.id).empty()));
    // F4: оплата (без provider_id), поздно ли менять ближайшую встречу, политика отмены.
    json.set("payment", payments::guestView(booking.payment));
    // F10: ссылка на чек «Мой налог», если специалист её указал ("" — нет).
    json.set("receipt_url", Json(booking.payment.isObject() ? booking.payment.getString("receipt_url") : std::string()));
    json.set("late", Json(late));
    json.set("policy", policyJson(page));
    json.set("payment_note", Json(page.paymentNote));
    return json;
}

// ------------------------------------------------------------ свободное время

bool BookingManager::offers(const BookingPageRecord& page, std::vector<Offer>& out) {
    const std::string version = page.updatedAt + "|" + std::to_string(page.durationMinutes) + "|" +
                                std::to_string(page.daysAhead) + "|" + std::to_string(page.noticeHours) + "|" +
                                (page.weekends ? "1" : "0");
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto it = cache_.find(page.id);
        if (it != cache_.end() && it->second.version == version &&
            std::chrono::steady_clock::now() - it->second.at < std::chrono::seconds(kCacheSeconds)) {
            out = it->second.offers;
            return true;
        }
    }

    Json request = Json::object();
    request.set("context", memory_.context(page.ownerId, page.title));
    request.set("duration_minutes", Json(static_cast<long long>(page.durationMinutes)));
    request.set("window_hours", Json(static_cast<long long>(page.daysAhead) * 24));
    request.set("max_slots", Json(static_cast<long long>(kAiMaxSlots)));
    request.set("per_day", Json(static_cast<long long>(kAiPerDay)));
    request.set("lead_minutes", Json(static_cast<long long>(page.noticeHours) * 60));
    request.set("weekends", Json(page.weekends));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;
    std::string error;
    std::string url = config_.aiServiceUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const net::HttpResponse response =
        net::httpRequest("POST", url + "/v1/agent/slots", headers, request.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Warn, "booking")
            << "подбор слотов не удался: " << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
        return false;
    }
    const Json answer = Json::parse(response.body, &error);
    if (!answer.isObject() || !answer.get("slots").isArray()) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "некорректный ответ /v1/agent/slots";
        return false;
    }

    // AI-сервис не источник истины: длительность, окно и пересечения проверяем сами.
    // Запас до начала и занятость — при каждом показе (они меняются со временем).
    const std::time_t now = std::time(nullptr);
    const std::time_t windowEnd = now + static_cast<std::time_t>(page.daysAhead) * 86400;
    const std::time_t duration = static_cast<std::time_t>(page.durationMinutes) * 60;
    std::vector<Offer> accepted;
    const Json offered = answer.get("slots");
    for (const auto& slot : offered.items()) {
        Offer offer;
        if (!parseIsoUtc(slot.getString("start"), offer.start) || !parseIsoUtc(slot.getString("end"), offer.end)) {
            continue;
        }
        if (offer.end - offer.start != duration || offer.start <= now || offer.end > windowEnd) continue;
        const bool overlaps = std::any_of(accepted.begin(), accepted.end(), [&](const Offer& other) {
            return offer.start < other.end && other.start < offer.end;
        });
        if (overlaps) continue;
        offer.day = slot.getString("day");
        offer.local = slot.getString("local");
        checkLocal(offer.start, offer.day, offer.local);
        accepted.push_back(offer);
        if (static_cast<int>(accepted.size()) >= kAiMaxSlots) break;
    }
    std::sort(accepted.begin(), accepted.end(), [](const Offer& a, const Offer& b) { return a.start < b.start; });
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (cache_.size() > 1000) cache_.clear();
        cache_[page.id] = Cached{std::chrono::steady_clock::now(), version, accepted};
    }
    out = std::move(accepted);
    return true;
}

bool BookingManager::series(const BookingPageRecord& page, const Offer& first, int count, std::vector<Offer>& out) {
    out.clear();
    if (count <= 1) {
        out.push_back(first);
        return true;
    }
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    Json request = Json::object();
    request.set("timezone", Json(ownerTimezone));
    request.set("start", Json(isoUtc(first.start)));
    request.set("duration_minutes", Json(static_cast<long long>(page.durationMinutes)));
    request.set("count", Json(static_cast<long long>(count)));
    request.set("interval_days", Json(7LL));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;
    std::string error;
    std::string url = config_.aiServiceUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const net::HttpResponse response =
        net::httpRequest("POST", url + "/v1/agent/series", headers, request.dump(), config_.aiTimeoutMs, error);
    const Json answer = response.ok() ? Json::parse(response.body, &error) : Json();
    if (!answer.isObject() || !answer.get("occurrences").isArray()) {
        SELFVOY_LOG(log::Level::Warn, "booking")
            << "серия не рассчитана: " << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
        return false;
    }
    // AI не источник истины: первая встреча — ровно выбранная, длительность та же,
    // повторы через неделю ± 2 часа (сдвиг перехода на летнее/зимнее время).
    const std::time_t duration = static_cast<std::time_t>(page.durationMinutes) * 60;
    const Json items = answer.get("occurrences");
    for (const auto& item : items.items()) {
        Offer offer;
        if (!parseIsoUtc(item.getString("start"), offer.start) || !parseIsoUtc(item.getString("end"), offer.end) ||
            offer.end - offer.start != duration) {
            out.clear();
            return false;
        }
        if (out.empty() ? offer.start != first.start
                        : std::abs(static_cast<long long>(offer.start - out.back().start) - kWeekSec) > 2 * 3600) {
            out.clear();
            return false;
        }
        offer.day = validDay(item.getString("day")) ? item.getString("day")
                                                    : addDays(first.day, static_cast<int>(out.size() * 7));
        offer.local = item.getString("local");
        if (out.empty() && !validLocal(offer.local)) offer.local = first.local;
        checkLocal(offer.start, offer.day, offer.local);
        out.push_back(offer);
    }
    if (static_cast<int>(out.size()) != count) {
        out.clear();
        return false;
    }
    return true;
}

std::unordered_map<std::string, int> BookingManager::dayCounts(const BookingPageRecord& page, long long skipBooking,
                                                               const std::string& skipStart, long long skipHold) {
    std::unordered_map<std::string, int> counts;
    for (const auto& booking : database_.db().listUpcomingBookings(page.ownerId, page.id, 5000)) {
        for (const auto& occurrence : booking.occurrences.items()) {
            if (occurrence.getString("status") != "active") continue;
            if (booking.id == skipBooking && occurrence.getString("start") == skipStart) continue;
            ++counts[occurrence.getString("day")];
        }
    }
    // F8: время, которое держится за гостем из листа ожидания этой страницы, — место в дне.
    for (const auto& hold : database_.db().listWaitlistHolds(page.ownerId, isoUtc(std::time(nullptr)))) {
        if (hold.pageId == page.id && hold.id != skipHold) ++counts[hold.offerDay];
    }
    return counts;
}

BookingManager::Busy BookingManager::busyFor(long long ownerId, long long skipBooking, const std::string& skipStart,
                                             long long skipHold) {
    Busy busy = memory_.busy(ownerId);
    const auto drop = [&busy](std::time_t start, std::time_t end) {
        const auto it = std::find(busy.begin(), busy.end(), std::make_pair(start, end));
        if (it != busy.end()) busy.erase(it);
    };
    if (skipBooking > 0 && !skipStart.empty()) {
        // Переносимая встреча не мешает сама себе.
        if (const auto booking = database_.db().findBooking(skipBooking)) {
            for (const auto& occurrence : booking->occurrences.items()) {
                std::time_t start = 0;
                std::time_t end = 0;
                if (occurrence.getString("start") != skipStart || occurrence.getString("status") != "active" ||
                    !parseIsoUtc(occurrence.getString("start"), start) ||
                    !parseIsoUtc(occurrence.getString("end"), end)) {
                    continue;
                }
                drop(start, end);
            }
        }
    }
    if (skipHold > 0) {
        // F8: время, которое держится за этим гостем из листа ожидания, ему не мешает.
        if (const auto entry = database_.db().findWaitlistEntry(skipHold)) {
            std::time_t start = 0;
            std::time_t end = 0;
            if (entry->status == "offered" && parseIsoUtc(entry->offerStart, start) && parseIsoUtc(entry->offerEnd, end)) {
                drop(start, end);
            }
        }
    }
    return busy;
}

std::vector<BookingManager::Offer> BookingManager::available(const BookingPageRecord& page,
                                                             const std::vector<Offer>& candidates,
                                                             long long skipBooking, const std::string& skipStart,
                                                             long long skipHold) {
    const std::time_t now = std::time(nullptr);
    const std::time_t earliest = now + static_cast<std::time_t>(page.noticeHours) * 3600;
    const Busy busy = busyFor(page.ownerId, skipBooking, skipStart, skipHold);
    const auto counts = dayCounts(page, skipBooking, skipStart, skipHold);
    std::vector<Offer> result;
    for (const auto& offer : candidates) {
        if (offer.start <= now || offer.start < earliest) continue;
        if (!isFree(busy, offer.start, offer.end)) continue;
        const auto count = counts.find(offer.day);
        if (count != counts.end() && count->second >= page.maxPerDay) continue;
        result.push_back(offer);
    }
    return result;
}

// ------------------------------------------------------------------ владелец

BookingManager::Result BookingManager::pageCreate(long long ownerId, const Json& payload) {
    BookingPageRecord page;
    page.ownerId = ownerId;
    page.title = trim(payload.getString("title"));
    if (page.title.empty() || !validText(page.title, 120, false)) {
        return Result::failure(protocol::code::kBadRequest, "название — от 1 до 120 символов в одну строку");
    }
    page.description = trim(payload.getString("description"));
    if (!validText(page.description, 1000, true)) {
        return Result::failure(protocol::code::kBadRequest, "описание — не больше 1000 символов");
    }
    long long duration = 30, daysAhead = 14, maxPerDay = 4, notice = 2, maxSeries = 1;
    if (!intField(payload, "duration_minutes", 15, 240, duration)) {
        return Result::failure(protocol::code::kBadRequest, "длительность — от 15 до 240 минут");
    }
    if (!intField(payload, "days_ahead", 1, 60, daysAhead)) {
        return Result::failure(protocol::code::kBadRequest, "запись вперёд — от 1 до 60 дней");
    }
    if (!intField(payload, "max_per_day", 1, 16, maxPerDay)) {
        return Result::failure(protocol::code::kBadRequest, "записей в день — от 1 до 16");
    }
    if (!intField(payload, "notice_hours", 0, 168, notice)) {
        return Result::failure(protocol::code::kBadRequest, "запас до встречи — от 0 до 168 часов");
    }
    if (!intField(payload, "max_series", 1, 12, maxSeries)) {
        return Result::failure(protocol::code::kBadRequest, "серия — от 1 до 12 встреч");
    }
    if (payload.contains("weekends") && !payload.get("weekends").isBool()) {
        return Result::failure(protocol::code::kBadRequest, "weekends — true или false");
    }
    if (payload.contains("guest_reminders") && !payload.get("guest_reminders").isBool()) {
        return Result::failure(protocol::code::kBadRequest, "guest_reminders — true или false");
    }
    if (payload.contains("hide_branding") && !payload.get("hide_branding").isBool()) {
        return Result::failure(protocol::code::kBadRequest, "hide_branding — true или false");
    }
    page.guestReminders = payload.getBool("guest_reminders", true);
    page.hideBranding = payload.getBool("hide_branding", false);
    if (page.hideBranding && !plan::proAllowed(database_.db(), ownerId)) {
        return Result::failure(protocol::code::kProRequired, "скрыть подпись Selfvoy можно на тарифе Pro");
    }
    // F4: цена, предоплата и политика отмены.
    const Result paymentFields = applyPaymentFields(ownerId, payload, true, page);
    if (!paymentFields.ok) return paymentFields;
    // F5: предложения пакетов.
    const Result packageOffers = applyPackageOffers(ownerId, payload, true, page);
    if (!packageOffers.ok) return packageOffers;
    // F8: лист ожидания и повторный приём.
    const Result waitlistFields = applyWaitlistFields(ownerId, payload, true, page);
    if (!waitlistFields.ok) return waitlistFields;
    // F7: ИИ-администратор в Telegram.
    const Result aiAdmin = applyAiAdmin(ownerId, payload, true, page);
    if (!aiAdmin.ok) return aiAdmin;
    page.durationMinutes = static_cast<int>(duration);
    page.daysAhead = static_cast<int>(daysAhead);
    page.maxPerDay = static_cast<int>(maxPerDay);
    page.noticeHours = static_cast<int>(notice);
    page.maxSeries = static_cast<int>(maxSeries);
    page.weekends = payload.getBool("weekends", false);

    // Удалённые страницы в лимит 10 не входят — создание ограничено ещё и по времени,
    // чтобы цикл «создать → удалить» не плодил строки без конца.
    if (limited("booking.page:" + std::to_string(ownerId), kPageCreatesPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много новых страниц — попробуйте через час");
    }
    auto lock = schedule_.acquire(ownerId);  // проверка лимита и создание — без гонки двух запросов
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    if (database_.db().listBookingPages(ownerId).size() >= static_cast<std::size_t>(kMaxPages)) {
        return Result::failure(protocol::code::kForbidden, "не больше 10 страниц записи — удалите ненужные");
    }

    long long id = 0;
    for (int attempt = 0; attempt < 5 && id == 0; ++attempt) {
        const std::string bytes = crypto::randomBytes(kSlugLength);
        page.slug.clear();
        for (const unsigned char byte : bytes) page.slug.push_back("abcdefghijklmnopqrstuvwxyz0123456789"[byte % 36]);
        const DatabaseError saved = database_.db().createBookingPage(page, id);
        if (!saved.ok) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "страница записи не сохранена: " << saved.message;
            id = 0;
        }
    }
    if (id == 0) return Result::failure(protocol::code::kInternal, "не удалось сохранить страницу записи");
    const auto stored = database_.db().findBookingPage(id);
    Result result;
    result.ok = true;
    result.payload.set("page", pageJson(stored ? *stored : page));
    return result;
}

BookingManager::Result BookingManager::pageList(long long ownerId) {
    Json pages = Json::array();
    for (const auto& page : database_.db().listBookingPages(ownerId)) pages.push(pageJson(page));
    Result result;
    result.ok = true;
    result.payload.set("pages", pages);
    return result;
}

BookingManager::Result BookingManager::pageUpdate(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("page_id");
    auto page = id > 0 ? database_.db().findBookingPage(id) : std::nullopt;
    if (!page || page->ownerId != ownerId || page->status == "archived") {
        return Result::failure(protocol::code::kNotFound, "страница записи не найдена");
    }
    if (payload.contains("title")) {
        page->title = trim(payload.getString("title"));
        if (page->title.empty() || !validText(page->title, 120, false)) {
            return Result::failure(protocol::code::kBadRequest, "название — от 1 до 120 символов в одну строку");
        }
    }
    if (payload.contains("description")) {
        page->description = trim(payload.getString("description"));
        if (!validText(page->description, 1000, true)) {
            return Result::failure(protocol::code::kBadRequest, "описание — не больше 1000 символов");
        }
    }
    long long duration = page->durationMinutes, daysAhead = page->daysAhead, maxPerDay = page->maxPerDay,
              notice = page->noticeHours, maxSeries = page->maxSeries;
    if (!intField(payload, "duration_minutes", 15, 240, duration)) {
        return Result::failure(protocol::code::kBadRequest, "длительность — от 15 до 240 минут");
    }
    if (!intField(payload, "days_ahead", 1, 60, daysAhead)) {
        return Result::failure(protocol::code::kBadRequest, "запись вперёд — от 1 до 60 дней");
    }
    if (!intField(payload, "max_per_day", 1, 16, maxPerDay)) {
        return Result::failure(protocol::code::kBadRequest, "записей в день — от 1 до 16");
    }
    if (!intField(payload, "notice_hours", 0, 168, notice)) {
        return Result::failure(protocol::code::kBadRequest, "запас до встречи — от 0 до 168 часов");
    }
    if (!intField(payload, "max_series", 1, 12, maxSeries)) {
        return Result::failure(protocol::code::kBadRequest, "серия — от 1 до 12 встреч");
    }
    if (payload.contains("weekends")) {
        if (!payload.get("weekends").isBool()) {
            return Result::failure(protocol::code::kBadRequest, "weekends — true или false");
        }
        page->weekends = payload.getBool("weekends");
    }
    if (payload.contains("guest_reminders")) {
        if (!payload.get("guest_reminders").isBool()) {
            return Result::failure(protocol::code::kBadRequest, "guest_reminders — true или false");
        }
        page->guestReminders = payload.getBool("guest_reminders");
    }
    if (payload.contains("hide_branding")) {
        if (!payload.get("hide_branding").isBool()) {
            return Result::failure(protocol::code::kBadRequest, "hide_branding — true или false");
        }
        const bool hide = payload.getBool("hide_branding");
        if (hide && !page->hideBranding && !plan::proAllowed(database_.db(), ownerId)) {
            return Result::failure(protocol::code::kProRequired, "скрыть подпись Selfvoy можно на тарифе Pro");
        }
        page->hideBranding = hide;
    }
    const Result paymentFields = applyPaymentFields(ownerId, payload, false, *page);
    if (!paymentFields.ok) return paymentFields;
    const Result packageOffers = applyPackageOffers(ownerId, payload, false, *page);
    if (!packageOffers.ok) return packageOffers;
    const int rebookBefore = page->rebookAfterDays;
    const Result waitlistFields = applyWaitlistFields(ownerId, payload, false, *page);
    if (!waitlistFields.ok) return waitlistFields;
    const Result aiAdmin = applyAiAdmin(ownerId, payload, false, *page);
    if (!aiAdmin.ok) return aiAdmin;
    if (payload.contains("status")) {
        const std::string status = payload.getString("status");
        if (status != "active" && status != "paused") {
            return Result::failure(protocol::code::kBadRequest, "status — active или paused");
        }
        page->status = status;
    }
    page->durationMinutes = static_cast<int>(duration);
    page->daysAhead = static_cast<int>(daysAhead);
    page->maxPerDay = static_cast<int>(maxPerDay);
    page->noticeHours = static_cast<int>(notice);
    page->maxSeries = static_cast<int>(maxSeries);
    const DatabaseError saved = database_.db().updateBookingPage(*page);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить страницу записи");
    // F8: выключили «пора на повторный приём» — неотправленные напоминания отменяются.
    if (rebookBefore > 0 && page->rebookAfterDays == 0) {
        database_.db().cancelPendingRebooks(page->id, isoUtc(std::time(nullptr)));
    }
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.erase(page->id);
    }
    const auto stored = database_.db().findBookingPage(page->id);
    Result result;
    result.ok = true;
    result.payload.set("page", pageJson(stored ? *stored : *page));
    return result;
}

BookingManager::Result BookingManager::pageDelete(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("page_id");
    auto page = id > 0 ? database_.db().findBookingPage(id) : std::nullopt;
    if (!page || page->ownerId != ownerId || page->status == "archived") {
        return Result::failure(protocol::code::kNotFound, "страница записи не найдена");
    }
    // Страница уходит в архив: ссылка перестаёт работать, уже сделанные записи
    // остаются в силе (их можно отменить через booking.ownerCancel).
    page->status = "archived";
    if (!database_.db().updateBookingPage(*page).ok) {
        return Result::failure(protocol::code::kInternal, "не удалось удалить страницу записи");
    }
    Result result;
    result.ok = true;
    result.payload.set("deleted", Json(true));
    return result;
}

BookingManager::Result BookingManager::list(long long ownerId, const Json& payload) {
    long long limit = payload.getInt("limit", 50);
    limit = std::max(1LL, std::min(200LL, limit));
    const long long pageId = std::max(0LL, payload.getInt("page_id"));
    std::unordered_map<long long, std::pair<std::string, int>> titles;  // page_id → (название, rebook_after_days)
    Json bookings = Json::array();
    for (const auto& booking : database_.db().listBookings(ownerId, pageId, payload.getBool("active_only", false),
                                                           static_cast<int>(limit))) {
        auto title = titles.find(booking.pageId);
        if (title == titles.end()) {
            const auto page = database_.db().findBookingPage(booking.pageId);
            title = titles.emplace(booking.pageId, page ? std::make_pair(page->title, page->rebookAfterDays)
                                                         : std::make_pair(std::string(), 0))
                        .first;
        }
        bookings.push(bookingJson(booking, title->second.first, title->second.second));
    }
    Result result;
    result.ok = true;
    result.payload.set("bookings", bookings);
    return result;
}

// ------------------------------------------------------------ встречи записи

long long BookingManager::createTask(const BookingRecord& booking, const std::string& pageTitle, std::time_t start) {
    TaskRecord task;
    task.userId = booking.ownerId;
    task.title = "Запись: " + pageTitle + " — " + booking.guestName;
    std::string notes;
    if (!booking.guestEmail.empty()) notes += "Email гостя: " + booking.guestEmail;
    if (!booking.guestComment.empty()) notes += (notes.empty() ? "" : "\n") + booking.guestComment;
    task.notes = notes;
    task.dueAt = isoUtc(start);
    task.remindAt = isoUtc(std::max(std::time(nullptr), start - kReminderLeadSec));
    long long taskId = 0;
    const DatabaseError saved = database_.db().createTask(task, taskId);
    if (!saved.ok) SELFVOY_LOG(log::Level::Warn, "booking") << "задача встречи не создана: " << saved.message;
    return taskId;
}

int BookingManager::findOccurrence(const BookingRecord& booking, const std::string& start, std::time_t now) {
    const auto& items = booking.occurrences.items();
    int nearest = -1;
    std::time_t nearestStart = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::time_t at = 0;
        if (items[i].getString("status") != "active" || !parseIsoUtc(items[i].getString("start"), at) || at <= now) {
            continue;
        }
        if (!start.empty()) {
            std::time_t wanted = 0;
            if (parseIsoUtc(start, wanted) && wanted == at) return static_cast<int>(i);
            continue;
        }
        if (nearest < 0 || at < nearestStart) {
            nearest = static_cast<int>(i);
            nearestStart = at;
        }
    }
    return nearest;
}

int BookingManager::cancelOccurrences(BookingRecord& booking, int index, std::time_t now,
                                      std::vector<long long>& tasks) {
    int cancelled = 0;
    Json updated = Json::array();
    const auto& items = booking.occurrences.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        Json occurrence = items[i];
        std::time_t at = 0;
        const bool future = parseIsoUtc(occurrence.getString("start"), at) && at > now;
        const bool target = index < 0 ? future : static_cast<int>(i) == index;
        if (target && occurrence.getString("status") == "active") {
            occurrence.set("status", Json(std::string("cancelled")));
            const long long taskId = occurrence.getInt("task_id");
            if (taskId > 0) tasks.push_back(taskId);
            occurrence.set("task_id", Json(0LL));
            ++cancelled;
        }
        updated.push(occurrence);
    }
    booking.occurrences = updated;
    const bool anyActive = std::any_of(updated.items().begin(), updated.items().end(), [now](const Json& item) {
        std::time_t at = 0;
        return item.getString("status") == "active" && parseIsoUtc(item.getString("end"), at) && at > now;
    });
    if (!anyActive) booking.status = "cancelled";
    return cancelled;
}

BookingManager::Result BookingManager::ownerCancel(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("booking_id");
    auto lock = schedule_.acquire(ownerId);
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    auto booking = id > 0 ? database_.db().findBooking(id) : std::nullopt;
    if (!booking || booking->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (booking->status != "active") return Result::failure(protocol::code::kConflict, "запись уже отменена");
    const std::time_t now = std::time(nullptr);
    int index = -1;
    if (payload.contains("occurrence")) {
        index = findOccurrence(*booking, payload.getString("occurrence"), now);
        if (index < 0) return Result::failure(protocol::code::kBadRequest, "нет такой предстоящей встречи");
    }
    std::vector<long long> tasks;
    const Json before = booking->occurrences;
    if (cancelOccurrences(*booking, index, now, tasks) == 0) {
        return Result::failure(protocol::code::kConflict, "предстоящих встреч нет");
    }
    if (booking->status == "cancelled") booking->cancelledBy = "owner";
    // F4: владелец отменил запись целиком — ожидаемая оплата не нужна, полученная
    // возвращается всегда (ЮKassa — POST /refunds, иначе — напоминание вернуть вручную).
    bool refundProvider = false;
    bool refundManual = false;
    if (booking->status == "cancelled" && booking->payment.isObject()) {
        if (payments::awaiting(booking->payment)) {
            booking->payment = withStatus(booking->payment, "expired");
        } else if (payments::settled(booking->payment)) {
            refundProvider = booking->payment.getString("method") == "yookassa" &&
                             booking->payment.getString("status") == "paid";
            refundManual = !refundProvider;
        }
    }
    if (!database_.db().updateBooking(*booking).ok) {
        return Result::failure(protocol::code::kInternal, "не удалось сохранить запись");
    }
    // F5: занятия пакета, отменённые владельцем, возвращаются всегда.
    int returned = 0;
    if (booking->packageId > 0) {
        returned = returnableSessions(before, booking->occurrences, nullptr, now, true);
        std::optional<PackagePurchaseRecord> unused;
        if (returned > 0) shiftPackageUsedLocked(booking->packageId, -returned, now, unused);
    }
    lock.release();
    for (const long long task : tasks) deleteOwnTask(booking->ownerId, task);
    dropInactiveReminders(*booking);
    std::vector<std::string> footer;
    if (refundProvider || refundManual) {
        refundAfterCancel(*booking, refundProvider);
        footer.push_back(refundProvider ? "Оплата " + moneyOf(booking->payment) + " будет возвращена тем же способом."
                                        : "Организатор вернёт оплату " + moneyOf(booking->payment) + ".");
        if (auto fresh = database_.db().findBooking(booking->id)) booking = std::move(fresh);
    }
    if (returned > 0) footer.push_back("Занятия возвращены на абонемент: " + std::to_string(returned) + ".");
    const auto freed = freedOccurrences(before, booking->occurrences, now);  // F8: время — листу ожидания
    const auto page = database_.db().findBookingPage(booking->pageId);
    if (page) {
        std::string ownerName;
        std::string ownerTimezone;
        ownerInfo(page->ownerId, ownerName, ownerTimezone);
        notifyGuest(*booking, *page, "booking.owner_cancelled", "Selfvoy: встреча отменена",
                    "Организатор отменил встречу",
                    ownerName + (booking->status == "cancelled" ? " отменил(а) запись. Встречи:"
                                                                : " отменил(а) одну встречу. Расписание:"),
                    "", booking->status != "cancelled", false, footer);
    }
    if (!freed.empty()) waitlistRelease(booking->pageId, freed, now);
    Result result;
    result.ok = true;
    result.payload.set("booking", bookingJson(*booking, page ? page->title : std::string(),
                                              page ? page->rebookAfterDays : 0));
    return result;
}

// ------------------------------------------------------------------ гость

BookingManager::Result BookingManager::get(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.get:" + Session::ipOf(remoteAddr), kGetPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string slug = payload.getString("slug");
    const auto page = validSlug(slug) ? database_.db().findBookingPageBySlug(slug) : std::nullopt;
    if (!page || page->status == "archived" || !ownerAccepts(page->ownerId)) {
        return Result::failure(protocol::code::kNotFound, "страница не найдена");
    }

    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    Json slots = Json::array();
    bool unavailable = false;
    if (page->status == "active") {
        std::vector<Offer> candidates;
        if (offers(*page, candidates)) {
            for (const auto& offer : available(*page, candidates)) {
                Json slot = Json::object();
                slot.set("start", Json(isoUtc(offer.start)));
                slot.set("end", Json(isoUtc(offer.end)));
                slot.set("day", Json(offer.day));
                slots.push(slot);
            }
        } else {
            unavailable = true;
        }
    }
    Json view = Json::object();
    view.set("slug", Json(page->slug));
    view.set("title", Json(page->title));
    view.set("description", Json(page->description));
    view.set("duration_minutes", Json(static_cast<long long>(page->durationMinutes)));
    view.set("max_series", Json(static_cast<long long>(page->maxSeries)));
    view.set("owner_name", Json(ownerName));
    view.set("owner_timezone", Json(ownerTimezone));
    view.set("status", Json(page->status));
    view.set("slots", slots);
    view.set("slots_unavailable", Json(unavailable));
    view.set("ref", Json(referrals_.codeFor(page->ownerId)));
    view.set("branding", branding(*page));
    // F4: цена, предоплата и политика отмены.
    view.set("price", Json(page->price));
    view.set("currency", Json(page->currency));
    view.set("prepay", Json(page->prepay));
    view.set("deposit", Json(page->deposit));
    view.set("payment_note", Json(page->paymentNote));
    view.set("payment_url", Json(page->paymentUrl));
    view.set("pay_within_minutes", Json(static_cast<long long>(page->payWithinMinutes)));
    view.set("policy", policyJson(*page));
    // F5: предложения пакетов (абонементов).
    view.set("packages", page->packages.isArray() ? page->packages : Json::array());
    // F8: «Встать в лист ожидания» и повторный приём.
    view.set("waitlist", Json(page->waitlist));
    view.set("waitlist_hold_minutes", Json(static_cast<long long>(page->waitlistHoldMinutes)));
    view.set("rebook_after_days", Json(static_cast<long long>(page->rebookAfterDays)));
    // F7: «Спросить в Telegram» — ИИ-администратор страницы ("" — выключен).
    view.set("telegram_ask_url", Json(telegramAskUrl(*page)));
    Result result;
    result.ok = true;
    result.payload.set("page", view);
    return result;
}

BookingManager::Result BookingManager::book(const Json& payload, const std::string& remoteAddr,
                                            long long telegramChat) {
    const std::string ip = Session::ipOf(remoteAddr);
    if (limited("booking.book:" + ip, kBookPerMinute) || limited("booking.book.h:" + ip, kBookPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много записей, попробуйте позже");
    }
    const std::string slug = payload.getString("slug");
    const auto page = validSlug(slug) ? database_.db().findBookingPageBySlug(slug) : std::nullopt;
    if (!page || page->status == "archived" || !ownerAccepts(page->ownerId)) {
        return Result::failure(protocol::code::kNotFound, "страница не найдена");
    }
    if (page->status != "active") return Result::failure(protocol::code::kConflict, "запись временно закрыта");
    // Записей на страницу в час: анонимно не забить календарь владельца. Считаются
    // только состоявшиеся записи — чужие неудачные попытки квоту не тратят.
    const std::string pageKey = "booking.page.h:" + std::to_string(page->id);
    if (!underLimit(pageKey, kBookPerPageHour, 3600)) {
        return Result::failure(protocol::code::kForbidden,
                               "на эту страницу сейчас слишком много записей — попробуйте через час");
    }

    long long count = 1;
    if (!intField(payload, "count", 1, page->maxSeries, count)) {
        return Result::failure(protocol::code::kBadRequest,
                               "число встреч — от 1 до " + std::to_string(page->maxSeries));
    }
    // F5: покупка пакета (package_id — id предложения страницы) или запись по пакету
    // (package_token): имя и адреса по умолчанию — из пакета, предоплата не нужна.
    PackageIntent intent;
    const Result packageCheck = packageIntent(*page, payload, count, intent);
    if (!packageCheck.ok) return packageCheck;
    const std::optional<PackagePurchaseRecord> held =
        intent.packageId > 0 ? database_.db().findPackagePurchase(intent.packageId) : std::nullopt;

    BookingRecord booking;
    booking.pageId = page->id;
    booking.ownerId = page->ownerId;
    // Снимок цены одной встречи на момент записи: смена цены страницы позже его не меняет.
    // Запись по пакету (покупка пакета или package_token) оплачена абонементом — 0.
    booking.price = intent.any() ? 0 : page->price;
    booking.currency = page->currency.empty() ? std::string("RUB") : page->currency;
    booking.guestName = trim(payload.getString("guest_name"));
    if (booking.guestName.empty() && held) booking.guestName = held->guestName;
    if (booking.guestName.empty() || !validText(booking.guestName, 80, false)) {
        return Result::failure(protocol::code::kBadRequest, "укажите имя — до 80 символов");
    }
    booking.guestEmail = trim(payload.getString("guest_email"));
    if (booking.guestEmail.empty() && held) booking.guestEmail = held->guestEmail;
    if (!booking.guestEmail.empty() && !plausibleEmail(booking.guestEmail)) {
        return Result::failure(protocol::code::kBadRequest, "некорректный email");
    }
    // F5: второй получатель извещений («копия напоминаний, например родителю»).
    booking.ccEmail = trim(payload.getString("cc_email"));
    if (booking.ccEmail.empty() && held && lowerAscii(held->ccEmail) != lowerAscii(booking.guestEmail)) {
        booking.ccEmail = held->ccEmail;
    }
    if (!booking.ccEmail.empty()) {
        if (!plausibleEmail(booking.ccEmail)) {
            return Result::failure(protocol::code::kBadRequest, "второй адрес (cc_email) — некорректный email");
        }
        if (lowerAscii(booking.ccEmail) == lowerAscii(booking.guestEmail)) {
            return Result::failure(protocol::code::kBadRequest, "второй адрес должен отличаться от email гостя");
        }
    }
    booking.guestComment = trim(payload.getString("comment"));
    if (!validText(booking.guestComment, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "комментарий — не больше 500 символов");
    }
    std::time_t wanted = 0;
    if (!parseIsoUtc(payload.getString("start"), wanted)) {
        return Result::failure(protocol::code::kBadRequest, "start — время одного из свободных слотов");
    }

    std::vector<Offer> candidates;
    if (!offers(*page, candidates)) {
        return Result::failure(protocol::code::kUpstream, "не удалось проверить свободное время — попробуйте позже");
    }
    const auto offered = std::find_if(candidates.begin(), candidates.end(),
                                      [wanted](const Offer& o) { return o.start == wanted; });
    if (offered == candidates.end()) {
        // Один ответ и для «такого слота нет», и для «уже занято»: отличая их,
        // анонимный гость восстановил бы личное расписание владельца на любое время.
        return Result::failure(protocol::code::kConflict, "это время уже занято — выберите другое");
    }
    // Серия: та же встреча каждую неделю в том же местном времени владельца
    // (AI-сервис считает её в поясе владельца) — до блокировки, чтобы сетевой
    // запрос не держал запись других гостей.
    std::vector<Offer> plan;
    if (!series(*page, *offered, static_cast<int>(count), plan)) {
        return Result::failure(protocol::code::kUpstream, "не удалось рассчитать серию встреч — попробуйте позже");
    }
    return commitBooking(*page, std::move(booking), intent, candidates, plan, count, 0, telegramChat);
}

BookingManager::Result BookingManager::commitBooking(const BookingPageRecord& pageRecord, BookingRecord booking,
                                                     const PackageIntent& intent, const std::vector<Offer>& candidates,
                                                     const std::vector<Offer>& plan, long long count,
                                                     long long waitlistEntry, long long telegramChat) {
    const BookingPageRecord* page = &pageRecord;
    if (plan.empty()) return Result::failure(protocol::code::kBadRequest, "start — время одного из свободных слотов");
    const std::time_t wanted = plan.front().start;
    std::optional<PackagePurchaseRecord> held;

    auto lock = schedule_.acquire(page->ownerId);
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    // F8: гость из листа ожидания забирает предложенное время — оно должно всё ещё держаться
    // за ним (перечитываем под блокировкой: планировщик мог отдать его следующему).
    std::optional<WaitlistEntryRecord> claim;
    if (waitlistEntry > 0) {
        claim = database_.db().findWaitlistEntry(waitlistEntry);
        std::time_t until = 0;
        if (!claim || claim->status != "offered" || claim->offerStart != isoUtc(wanted) ||
            !parseIsoUtc(claim->offerExpiresAt, until) || until <= std::time(nullptr)) {
            return Result::failure(protocol::code::kConflict, "время больше не держится за вами — выберите другое");
        }
    }
    const auto free = available(*page, candidates, 0, "", waitlistEntry);
    if (std::none_of(free.begin(), free.end(), [wanted](const Offer& o) { return o.start == wanted; })) {
        return Result::failure(protocol::code::kConflict, "это время уже занято — выберите другое");
    }
    const Offer* chosen = &plan.front();

    // Каждая встреча серии должна быть свободна и не превышать лимит записей в свой день.
    const Busy busy = busyFor(page->ownerId, 0, "", waitlistEntry);
    auto counts = dayCounts(*page, 0, "", waitlistEntry);
    Json occurrences = Json::array();
    for (const auto& item : plan) {
        if (!isFree(busy, item.start, item.end) || counts[item.day] >= page->maxPerDay) {
            return Result::failure(protocol::code::kConflict, "в неделю " + item.day +
                                                                  " это время занято — выберите меньше встреч или "
                                                                  "другое время");
        }
        ++counts[item.day];
        Json occurrence = Json::object();
        occurrence.set("start", Json(isoUtc(item.start)));
        occurrence.set("end", Json(isoUtc(item.end)));
        occurrence.set("day", Json(item.day));
        occurrence.set("local", Json(item.local));
        occurrence.set("status", Json(std::string("active")));
        occurrence.set("task_id", Json(0LL));
        occurrences.push(occurrence);
    }

    // F5: пакет перечитывается под блокировкой — остаток и срок могли измениться; каждая
    // встреча — до конца срока действия пакета.
    if (intent.packageId > 0) {
        held = database_.db().findPackagePurchase(intent.packageId);
        if (!held) return Result::failure(protocol::code::kNotFound, "абонемент не найден");
        const Result usable = packageUsable(*held, page->id, count, std::time(nullptr));
        if (!usable.ok) return usable;
        std::time_t until = 0;
        if (parseIsoUtc(held->validUntil, until) &&
            std::any_of(plan.begin(), plan.end(), [until](const Offer& item) { return item.start >= until; })) {
            return Result::failure(protocol::code::kConflict, "абонемент действует до " + held->validUntil.substr(0, 10) +
                                                                  " — выберите время раньше");
        }
    }

    // F4: предоплата — сумма (full: цена × встречи, deposit: депозит один раз), срок оплаты
    // (не позже начала первой встречи), способ: ЮKassa владельца или ссылка/реквизиты.
    // Запись по пакету (F5) оплачена пакетом.
    if (!intent.any() && page->prepay != "none" && page->price > 0 && payments_) {
        const long long amount = page->prepay == "deposit" ? page->deposit : page->price * count;
        const std::time_t due = std::min(std::time(nullptr) + static_cast<std::time_t>(page->payWithinMinutes) * 60,
                                         plan.front().start);
        Json payment = payments_->begin(page->ownerId, page->prepay, amount, page->currency, due, page->paymentUrl);
        if (payment.getString("method") == "yookassa" || !page->paymentUrl.empty() || !page->paymentNote.empty()) {
            booking.payment = payment;
        } else {
            SELFVOY_LOG(log::Level::Warn, "booking")
                << "страница " << page->id << ": предоплата без способа оплаты (ЮKassa отключена) — запись без оплаты";
        }
    }

    booking.occurrences = occurrences;
    // F5: покупка пакета при записи — строка покупки до записи (запись ссылается на неё).
    PackagePurchaseRecord purchase;
    std::string packageToken;
    if (intent.offerId > 0) {
        const Result created =
            createPurchase(*page, intent.offer, booking, static_cast<int>(count), plan.front().start, purchase, packageToken);
        if (!created.ok) return created;
        booking.packageId = purchase.id;
    } else if (intent.packageId > 0) {
        booking.packageId = intent.packageId;
    }

    std::string token;
    long long id = 0;
    for (int attempt = 0; attempt < 3 && id == 0; ++attempt) {
        token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
        while (!token.empty() && token.back() == '=') token.pop_back();
        booking.manageHash = tokenHash(token);
        if (booking.manageHash.empty()) continue;
        booking.manageTokenEnc = sealManageToken(token);
        const DatabaseError saved = database_.db().createBooking(booking, id);
        if (!saved.ok) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "запись не сохранена: " << saved.message;
            id = 0;
        }
    }
    if (id == 0) {
        if (purchase.id > 0) {
            purchase.status = "cancelled";
            if (purchase.payment.isObject()) purchase.payment = withStatus(purchase.payment, "expired");
            database_.db().updatePackagePurchase(purchase);
        }
        return Result::failure(protocol::code::kInternal, "не удалось сохранить запись");
    }
    booking.id = id;
    // F7: запись из Telegram — чат гостя привязывается сразу (извещение о записи уйдёт туда).
    if (telegramChat != 0 && !database_.db().linkBookingTelegram(id, telegramChat, TelegramBot::kMaxBookingChats)) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "чат Telegram не привязан к записи " << id;
    }
    // F5: запись по пакету списывает занятия (по одному на встречу).
    std::optional<PackagePurchaseRecord> consumed;
    if (intent.packageId > 0) shiftPackageUsedLocked(intent.packageId, static_cast<int>(count), std::time(nullptr), consumed);
    // Засчитываем состоявшуюся запись в лимит страницы (проверка — в book()).
    limited("booking.page.h:" + std::to_string(pageRecord.id), kBookPerPageHour + 1, 3600);

    // Задачи с напоминанием — на каждую встречу.
    Json withTasks = Json::array();
    for (const auto& occurrence : booking.occurrences.items()) {
        Json item = occurrence;
        std::time_t start = 0;
        parseIsoUtc(item.getString("start"), start);
        item.set("task_id", Json(createTask(booking, page->title, start)));
        withTasks.push(item);
    }
    booking.occurrences = withTasks;
    if (!database_.db().updateBooking(booking).ok) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "id задач записи " << id << " не сохранены";
    }
    if (claim) {
        // F8: держание закрыто записью — запись листа ожидания выполнена.
        claim->status = "booked";
        claim->offerStatus = "claimed";
        claim->bookingId = id;
        if (!database_.db().updateWaitlistEntry(*claim).ok) {
            SELFVOY_LOG(log::Level::Warn, "booking") << "запись листа ожидания " << claim->id << " не сохранена";
        }
    }
    lock.release();  // дальше — платёж, уведомление и письмо, расписание владельца не держим

    // F4: платёж в ЮKassa (Idempotence-Key bk-<id>; гость вернётся на ссылку управления).
    if (booking.payment.isObject() && booking.payment.getString("method") == "yookassa") {
        std::string ownerName;
        std::string ownerTimezone;
        ownerInfo(page->ownerId, ownerName, ownerTimezone);
        Json payment = booking.payment;
        Json metadata = Json::object();
        metadata.set("booking_id", Json(std::to_string(id)));
        const std::string description =
            utf8Prefix(page->title + ", " + whenText(booking.occurrences.items().front(), ownerTimezone), 128);
        std::string error;
        if (!payments_->submit(page->ownerId, payment, description, manageUrl(token) + "?paid=1",
                               "bk-" + std::to_string(id), metadata, error)) {
            // Платёж не создан — время не держим: запись отменяется системой, гостю — ошибка.
            std::vector<long long> tasks;
            std::optional<BookingRecord> cancelled;
            mutateBooking(id, [&](BookingRecord& record) {
                if (record.status != "active") return false;
                cancelOccurrences(record, -1, std::time(nullptr), tasks);
                record.status = "cancelled";
                record.cancelledBy = "system";
                if (record.payment.isObject()) record.payment = withStatus(record.payment, "expired");
                return true;
            }, cancelled);
            for (const long long task : tasks) deleteOwnTask(page->ownerId, task);
            return Result::failure(protocol::code::kUpstream, "не удалось создать платёж — попробуйте позже");
        }
        std::optional<BookingRecord> updated;
        mutateBooking(id, [&](BookingRecord& record) {
            if (!record.payment.isObject() || !record.payment.getString("provider_id").empty()) return false;
            Json stored = record.payment;
            stored.set("provider_id", payment.get("provider_id"));
            stored.set("action_url", payment.get("action_url"));
            record.payment = stored;
            return true;
        }, updated);
        booking.payment = updated ? updated->payment : payment;
    }
    // F5: платёж за пакет в ЮKassa (Idempotence-Key pk-<id>; гость вернётся на страницу пакета).
    if (purchase.id > 0 && purchase.payment.isObject() && purchase.payment.getString("method") == "yookassa" &&
        purchase.status == "pending") {
        Json payment = purchase.payment;
        Json metadata = Json::object();
        metadata.set("package_id", Json(std::to_string(purchase.id)));
        const std::string description = utf8Prefix(page->title + ": " + purchase.title, 128);
        std::string error;
        if (!payments_ || !payments_->submit(page->ownerId, payment, description, packageUrl(packageToken) + "?paid=1",
                                             "pk-" + std::to_string(purchase.id), metadata, error)) {
            // Платёж не создан — ни пакет, ни время не держим: гостю — ошибка.
            std::optional<PackagePurchaseRecord> dropped;
            mutatePackage(purchase.id, [](PackagePurchaseRecord& record) {
                if (record.status != "pending") return false;
                record.status = "cancelled";
                if (record.payment.isObject()) record.payment = withStatus(record.payment, "expired");
                return true;
            }, dropped);
            std::vector<long long> tasks;
            std::optional<BookingRecord> cancelled;
            mutateBooking(id, [&](BookingRecord& record) {
                if (record.status != "active") return false;
                cancelOccurrences(record, -1, std::time(nullptr), tasks);
                record.status = "cancelled";
                record.cancelledBy = "system";
                return true;
            }, cancelled);
            for (const long long task : tasks) deleteOwnTask(page->ownerId, task);
            return Result::failure(protocol::code::kUpstream, "не удалось создать платёж — попробуйте позже");
        }
        std::optional<PackagePurchaseRecord> updated;
        mutatePackage(purchase.id, [&](PackagePurchaseRecord& record) {
            if (!record.payment.isObject() || !record.payment.getString("provider_id").empty()) return false;
            Json stored = record.payment;
            stored.set("provider_id", payment.get("provider_id"));
            stored.set("action_url", payment.get("action_url"));
            record.payment = stored;
            return true;
        }, updated);
        if (updated) purchase = *updated;
    }
    scheduleReminders(booking, std::time(nullptr));

    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(id));
        notify.set("page_id", Json(page->id));
        notify.set("start", Json(isoUtc(chosen->start)));
        notify.set("end", Json(isoUtc(chosen->end)));
        notify.set("count", Json(count));
        notify.set("guest_name", Json(booking.guestName));
        if (booking.packageId > 0) notify.set("package_id", Json(booking.packageId));
        if (claim) {
            // F8: запись из листа ожидания — одно уведомление waitlist.booked (вместо booking.created).
            notify.set("entry_id", Json(claim->id));
            notifications_->create(page->ownerId, "waitlist.booked",
                                   booking.guestName + " забрал(а) освободившееся время из листа ожидания",
                                   page->title, notify);
        } else {
            notifications_->create(page->ownerId, "booking.created",
                                   booking.guestName + (count > 1 ? " записался(ась) на серию встреч"
                                                                  : " записался(ась) на встречу"),
                                   page->title, notify);
        }
    }
    {
        std::string ownerName;
        std::string ownerTimezone;
        ownerInfo(page->ownerId, ownerName, ownerTimezone);
        std::vector<std::string> footer = paymentLines(*page, booking.payment, std::time(nullptr));
        if (purchase.id > 0) {
            // F5: письмо о записи — и о покупке пакета: занятия, ссылка пакета, оплата.
            const auto lines = packageLines(purchase, *page, packageToken, std::time(nullptr));
            footer.insert(footer.end(), lines.begin(), lines.end());
        } else if (consumed) {
            footer.push_back("Списано с абонемента «" + consumed->title + "»: осталось " +
                             std::to_string(std::max(0, consumed->sessionsTotal - consumed->sessionsUsed)) + " из " +
                             std::to_string(consumed->sessionsTotal) + ".");
        }
        const std::string telegramUrl = telegram_ ? telegram_->guestUrl(booking) : std::string();
        if (!telegramUrl.empty()) footer.push_back("Напоминания в Telegram: " + telegramUrl);
        notifyGuest(booking, *page, "booking.created", "Selfvoy: вы записаны", "Вы записаны",
                    ownerName + (count > 1 ? " ждёт вас на встречах:" : " ждёт вас:"), token, true, false, footer);
    }

    if (purchase.id > 0) {
        if (notifications_) {
            notifications_->create(page->ownerId, "package.purchased",
                                   booking.guestName + " купил(а) пакет «" + purchase.title + "»",
                                   slotspost::moneyLabel(purchase.price, purchase.currency) + " · " + page->title,
                                   packageNotice(purchase, Json::object()));
        }
    }
    // F5: остаток 1 или 0 после списания — «продлите абонемент» (гостю и владельцу).
    if (consumed && consumed->sessionsTotal - consumed->sessionsUsed <= kPackageLowLeft) packageLow(*consumed);
    if (purchase.id > 0 && purchase.status != "pending" && purchase.sessionsTotal - purchase.sessionsUsed <= kPackageLowLeft) {
        packageLow(purchase);
    }

    const auto stored = database_.db().findBooking(id);
    Result result;
    result.ok = true;
    result.payload.set("booking", guestBookingJson(stored ? *stored : booking, *page, false));
    result.payload.set("token", Json(token));
    result.payload.set("url", Json(config_.publicUrl.empty() ? std::string() : config_.publicUrl + "/#/m/" + token));
    result.payload.set("telegram_url", Json(telegram_ ? telegram_->guestUrl(stored ? *stored : booking) : std::string()));
    if (purchase.id > 0) {
        // Токен ссылки пакета показывается один раз (и уходит письмом гостю).
        const auto fresh = database_.db().findPackagePurchase(purchase.id);
        result.payload.set("package_token", Json(packageToken));
        result.payload.set("package_url", Json(packageUrl(packageToken)));
        result.payload.set("package", packageGuestJson(fresh ? *fresh : purchase, *page));
    } else if (intent.packageId > 0) {
        const auto fresh = database_.db().findPackagePurchase(intent.packageId);
        if (fresh) result.payload.set("package", packageGuestJson(*fresh, *page));
    }
    return result;
}

BookingManager::Result BookingManager::manage(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.manage:" + Session::ipOf(remoteAddr), kGetPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    auto booking = hash.empty() ? std::nullopt : database_.db().findBookingByToken(hash);
    const auto page = booking ? database_.db().findBookingPage(booking->pageId) : std::nullopt;
    if (!booking || !page) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    // F4: оплата через ЮKassa — статус проверяется при каждом открытии ссылки управления
    // (гость вернулся со страницы оплаты с ?paid=1).
    if (booking->payment.isObject() && booking->payment.getString("method") == "yookassa") {
        const bool waiting = payments::awaiting(booking->payment);
        if (syncPayment(*booking, std::time(nullptr), waiting ? 0 : PaymentManager::kPollIntervalSec) !=
            PaymentManager::Remote::Throttled) {
            if (auto fresh = database_.db().findBooking(booking->id)) booking = std::move(fresh);
        }
    }
    Result result;
    result.ok = true;
    result.payload.set("booking", guestBookingJson(*booking, *page, true));
    return result;
}

BookingManager::Result BookingManager::reschedule(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    auto booking = hash.empty() ? std::nullopt : database_.db().findBookingByToken(hash);
    const auto page = booking ? database_.db().findBookingPage(booking->pageId) : std::nullopt;
    if (!booking || !page) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (page->status != "active") return Result::failure(protocol::code::kConflict, "запись временно закрыта");
    std::time_t wanted = 0;
    if (!parseIsoUtc(payload.getString("start"), wanted)) {
        return Result::failure(protocol::code::kBadRequest, "start — время одного из свободных слотов");
    }
    std::vector<Offer> candidates;
    if (!offers(*page, candidates)) {
        return Result::failure(protocol::code::kUpstream, "не удалось проверить свободное время — попробуйте позже");
    }

    auto lock = schedule_.acquire(page->ownerId);
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    booking = database_.db().findBookingByToken(hash);  // перечитываем под блокировкой
    if (!booking || booking->status != "active") return Result::failure(protocol::code::kConflict, "запись отменена");
    if (booking->reschedules >= kMaxReschedules) {
        return Result::failure(protocol::code::kConflict, "перенести можно не больше трёх раз — отмените запись");
    }
    const std::time_t now = std::time(nullptr);
    const int index = findOccurrence(*booking, payload.getString("occurrence"), now);
    if (index < 0) return Result::failure(protocol::code::kBadRequest, "нет такой предстоящей встречи");
    Json occurrence = booking->occurrences.items()[static_cast<std::size_t>(index)];
    const std::string from = occurrence.getString("start");
    {
        // F4: позже срока (cancel_hours) при late_cancel=block гость не переносит.
        std::time_t fromAt = 0;
        if (page->lateCancel == "block" && parseIsoUtc(from, fromAt) && lateFor(*page, fromAt, now)) {
            return Result::failure(protocol::code::kConflict, lateMessage(*page));
        }
    }

    const auto free = available(*page, candidates, booking->id, from);
    const auto chosen = std::find_if(free.begin(), free.end(), [wanted](const Offer& o) { return o.start == wanted; });
    if (chosen == free.end()) {
        const bool offered = std::any_of(candidates.begin(), candidates.end(),
                                         [wanted](const Offer& o) { return o.start == wanted; });
        return offered ? Result::failure(protocol::code::kConflict, "это время уже занято — выберите другое")
                       : Result::failure(protocol::code::kBadRequest, "этого времени нет среди свободных");
    }
    // Новое время не должно задевать другие встречи той же серии.
    const auto& items = booking->occurrences.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (static_cast<int>(i) == index || items[i].getString("status") != "active") continue;
        std::time_t start = 0;
        std::time_t end = 0;
        if (parseIsoUtc(items[i].getString("start"), start) && parseIsoUtc(items[i].getString("end"), end) &&
            chosen->start < end && start < chosen->end) {
            return Result::failure(protocol::code::kConflict, "это время совпадает с другой встречей серии");
        }
    }

    const long long oldTask = occurrence.getInt("task_id");
    const Json previous = occurrence;  // F8: прежнее время освобождается — листу ожидания
    occurrence.set("start", Json(isoUtc(chosen->start)));
    occurrence.set("end", Json(isoUtc(chosen->end)));
    occurrence.set("day", Json(chosen->day));
    occurrence.set("local", Json(chosen->local));
    occurrence.set("task_id", Json(createTask(*booking, page->title, chosen->start)));
    occurrence.set("confirmed_at", Json(std::string()));  // новое время — подтверждение заново
    Json updated = Json::array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        updated.push(static_cast<int>(i) == index ? occurrence : items[i]);
    }
    booking->occurrences = updated;
    booking->reschedules += 1;
    if (!database_.db().updateBooking(*booking).ok) {
        // Запись не изменилась — новая задача не нужна, старая остаётся.
        deleteOwnTask(booking->ownerId, occurrence.getInt("task_id"));
        return Result::failure(protocol::code::kInternal, "не удалось сохранить запись");
    }
    lock.release();
    deleteOwnTask(booking->ownerId, oldTask);
    // Напоминания: прежнего времени — удалить (неотправленные), нового — запланировать.
    {
        std::time_t fromAt = 0;
        if (parseIsoUtc(from, fromAt)) database_.db().deleteBookingReminders(booking->id, isoUtc(fromAt));
        scheduleReminders(*booking, std::time(nullptr));
    }

    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(booking->id));
        notify.set("page_id", Json(page->id));
        notify.set("from", Json(from));
        notify.set("start", Json(isoUtc(chosen->start)));
        notify.set("end", Json(isoUtc(chosen->end)));
        notify.set("guest_name", Json(booking->guestName));
        notifications_->create(page->ownerId, "booking.rescheduled", booking->guestName + " перенес(ла) встречу",
                               page->title, notify);
    }
    notifyGuest(*booking, *page, "booking.rescheduled", "Selfvoy: встреча перенесена", "Встреча перенесена",
                "Новое расписание (перенести или отменить можно по ссылке из первого письма):", "", true);
    waitlistRelease(page->id, {previous}, std::time(nullptr));
    Result result;
    result.ok = true;
    result.payload.set("booking", guestBookingJson(*booking, *page, true));
    return result;
}

BookingManager::Result BookingManager::cancel(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string reason = trim(payload.getString("reason"));
    if (!validText(reason, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "причина — не больше 500 символов");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    auto booking = hash.empty() ? std::nullopt : database_.db().findBookingByToken(hash);
    if (!booking) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    auto lock = schedule_.acquire(booking->ownerId);
    if (!lock.ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    booking = database_.db().findBookingByToken(hash);  // перечитываем под блокировкой
    const auto page = booking ? database_.db().findBookingPage(booking->pageId) : std::nullopt;
    if (!booking || !page) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (booking->status != "active") return Result::failure(protocol::code::kConflict, "запись уже отменена");
    const std::time_t now = std::time(nullptr);
    int index = -1;
    if (payload.contains("occurrence")) {
        index = findOccurrence(*booking, payload.getString("occurrence"), now);
        if (index < 0) return Result::failure(protocol::code::kBadRequest, "нет такой предстоящей встречи");
    }
    // F4: поздняя отмена — по ближайшей из отменяемых встреч (до начала меньше cancel_hours).
    bool late = false;
    {
        const int target = index >= 0 ? index : findOccurrence(*booking, "", now);
        std::time_t targetStart = 0;
        late = target >= 0 &&
               parseIsoUtc(booking->occurrences.items()[static_cast<std::size_t>(target)].getString("start"),
                           targetStart) &&
               lateFor(*page, targetStart, now);
    }
    if (late && page->lateCancel == "block") return Result::failure(protocol::code::kConflict, lateMessage(*page));
    std::vector<long long> tasks;
    const Json before = booking->occurrences;
    const int cancelled = cancelOccurrences(*booking, index, now, tasks);
    if (cancelled == 0) return Result::failure(protocol::code::kConflict, "предстоящих встреч нет");
    if (booking->status == "cancelled") booking->cancelledBy = "guest";
    // Оплата: запись отменена целиком — ожидаемая оплата больше не нужна (expired);
    // полученная возвращается, если отмена не позже срока (ЮKassa — автоматически,
    // иначе — владельцу «верните оплату вручную»); позже срока — остаётся специалисту.
    bool refundProvider = false;
    bool refundManual = false;
    bool kept = false;
    if (booking->status == "cancelled" && booking->payment.isObject()) {
        if (payments::awaiting(booking->payment)) {
            booking->payment = withStatus(booking->payment, "expired");
        } else if (payments::settled(booking->payment)) {
            refundProvider = !late && booking->payment.getString("method") == "yookassa" &&
                             booking->payment.getString("status") == "paid";
            refundManual = !late && !refundProvider;
            kept = late;
        }
    }
    if (!database_.db().updateBooking(*booking).ok) {
        return Result::failure(protocol::code::kInternal, "не удалось сохранить запись");
    }
    // F5: запись по пакету — отменённые не позже cancel_hours встречи возвращают занятие,
    // поздняя отмена его сжигает.
    int returned = 0;
    if (booking->packageId > 0) {
        returned = returnableSessions(before, booking->occurrences, &*page, now, false);
        std::optional<PackagePurchaseRecord> unused;
        if (returned > 0) shiftPackageUsedLocked(booking->packageId, -returned, now, unused);
    }
    lock.release();
    for (const long long task : tasks) deleteOwnTask(booking->ownerId, task);
    dropInactiveReminders(*booking);
    std::vector<std::string> footer;
    if (refundProvider || refundManual) {
        refundAfterCancel(*booking, refundProvider);
        footer.push_back(refundProvider ? "Оплата " + moneyOf(booking->payment) + " будет возвращена тем же способом."
                                        : "Организатор вернёт оплату " + moneyOf(booking->payment) + ".");
        if (auto fresh = database_.db().findBooking(booking->id)) booking = std::move(fresh);
    } else if (kept) {
        footer.push_back("Отмена позже чем за " + std::to_string(page->cancelHours) +
                         " ч до встречи — оплата не возвращается.");
    }
    if (booking->packageId > 0) {
        if (returned > 0) footer.push_back("Занятия возвращены на абонемент: " + std::to_string(returned) + ".");
        if (cancelled > returned) {
            footer.push_back("Отмена позже чем за " + std::to_string(page->cancelHours) +
                             " ч до встречи — занятие списано с абонемента.");
        }
    }
    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(booking->id));
        notify.set("page_id", Json(page->id));
        notify.set("cancelled", Json(static_cast<long long>(cancelled)));
        notify.set("guest_name", Json(booking->guestName));
        notify.set("reason", Json(reason));
        notifications_->create(page->ownerId, "booking.cancelled",
                               booking->guestName + (cancelled > 1 ? " отменил(а) встречи" : " отменил(а) встречу"),
                               page->title, notify);
    }
    notifyGuest(*booking, *page, "booking.cancelled", "Selfvoy: запись отменена",
                booking->status == "cancelled" ? "Запись отменена" : "Встреча отменена",
                booking->status == "cancelled" ? "Организатор получил уведомление. Встречи записи:"
                                               : "Организатор получил уведомление. Остальные встречи в силе:",
                "", booking->status != "cancelled", false, footer);
    // F8: освободившееся время — первому подходящему из листа ожидания.
    const auto freed = freedOccurrences(before, booking->occurrences, now);
    if (!freed.empty()) waitlistRelease(page->id, freed, now);
    Result result;
    result.ok = true;
    result.payload.set("booking", guestBookingJson(*booking, *page, true));
    return result;
}

BookingManager::Result BookingManager::resendLink(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.resend:" + Session::ipOf(remoteAddr), kResendPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string slug = payload.getString("slug");
    std::string email = trim(payload.getString("email"));
    if (!plausibleEmail(email)) return Result::failure(protocol::code::kBadRequest, "некорректный email");
    std::transform(email.begin(), email.end(), email.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    Result result;
    result.ok = true;
    result.payload.set("sent", Json(true));  // одинаково для «нашли» и «не нашли»
    const auto page = validSlug(slug) ? database_.db().findBookingPageBySlug(slug) : std::nullopt;
    if (!page || page->status == "archived") return result;

    // Новая ссылка заменяет старую — менять её можно, только если письмо точно уйдёт:
    // иначе любой, кто знает адрес страницы и email гостя, оставил бы гостя без
    // ссылки (драйвер log писем не отправляет, лимит адреса исчерпан).
    if (!mail_ || !mail_->delivers()) return result;
    const std::time_t now = std::time(nullptr);
    auto lock = schedule_.acquire(page->ownerId);
    if (!lock.ok()) return result;
    for (auto booking : database_.db().listUpcomingBookings(page->ownerId, page->id, 1000)) {
        std::string guest = booking.guestEmail;
        std::transform(guest.begin(), guest.end(), guest.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (guest != email || findOccurrence(booking, "", now) < 0) continue;
        if (limited("booking.mail:" + mailboxKey(email), kMailPerAddressHour, 3600)) break;  // лимит адреса — до замены
        std::string token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
        while (!token.empty() && token.back() == '=') token.pop_back();
        booking.manageHash = tokenHash(token);
        booking.manageTokenEnc = sealManageToken(token);
        if (booking.manageHash.empty() || !database_.db().updateBooking(booking).ok) continue;
        notifyGuest(booking, *page, "booking.link", "Selfvoy: ссылка на вашу запись", "Ваша запись",
                    "Вы просили прислать ссылку заново — прежняя больше не действует. Встречи:", token, true, true);
    }
    return result;
}

// ------------------------------------------------------------ оплата (F4)

// ------------------------------------------------- F7: ИИ-администратор в Telegram

BookingManager::Result BookingManager::applyAiAdmin(long long ownerId, const Json& payload, bool creating,
                                                    BookingPageRecord& page) {
    Result ok;
    ok.ok = true;
    if (!payload.contains("ai_admin")) return ok;
    if (!payload.get("ai_admin").isBool()) {
        return Result::failure(protocol::code::kBadRequest, "ai_admin — true или false");
    }
    const bool enable = payload.getBool("ai_admin");
    // Проверки — только при включении (выключить можно всегда, как hide_branding).
    if (enable && (creating || !page.aiAdmin)) {
        if (!telegram_ || !telegram_->enabled()) {
            return Result::failure(protocol::code::kBadRequest,
                                   "ИИ-администратор отвечает гостям через Telegram-бота, а бот не настроен на "
                                   "сервере (SELFVOY_TELEGRAM_BOT_TOKEN и SELFVOY_TELEGRAM_BOT_USERNAME)");
        }
        if (!plan::proAllowed(database_.db(), ownerId)) {
            return Result::failure(protocol::code::kProRequired, "ИИ-администратор в Telegram доступен на тарифе Pro");
        }
    }
    page.aiAdmin = enable;
    return ok;
}

std::string BookingManager::pageUrl(const BookingPageRecord& page) const {
    return config_.publicUrl.empty() ? std::string() : config_.publicUrl + "/#/b/" + page.slug;
}

std::string BookingManager::telegramAskUrl(const BookingPageRecord& page) {
    if (!page.aiAdmin || page.status != "active" || !telegram_ || !telegram_->enabled() || !validSlug(page.slug)) {
        return "";
    }
    if (!plan::proAllowed(database_.db(), page.ownerId)) return "";
    return telegram_->linkUrl("p_" + page.slug);
}

bool BookingManager::freeSlots(const BookingPageRecord& page, std::size_t limit, std::vector<FreeSlot>& out) {
    static const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                          "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
    static const char* const kDays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    out.clear();
    std::vector<Offer> candidates;
    if (!offers(page, candidates)) return false;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page.ownerId, ownerName, ownerTimezone);
    for (const auto& offer : available(page, candidates)) {
        if (out.size() >= limit) break;
        FreeSlot slot;
        slot.start = offer.start;
        slot.end = offer.end;
        Json occurrence = Json::object();
        occurrence.set("start", Json(isoUtc(offer.start)));
        occurrence.set("local", Json(offer.local));
        slot.when = whenText(occurrence, ownerTimezone);
        // Короткая подпись кнопки: местное время владельца, иначе UTC.
        const std::string local = validLocal(offer.local) ? offer.local : isoUtc(offer.start).substr(0, 16);
        std::time_t date = 0;
        parseIsoUtc(local.substr(0, 10) + "T00:00:00Z", date);
        std::tm parts{};
        gmtime_r(&date, &parts);
        slot.label = std::string(kDays[parts.tm_wday]) + ", " + std::to_string(parts.tm_mday) + " " +
                     kMonths[parts.tm_mon] + ", " + local.substr(11, 5);
        out.push_back(slot);
    }
    return true;
}

BookingManager::Result BookingManager::telegramBook(const BookingPageRecord& page, long long chatId, std::time_t start,
                                                    const std::string& guestName, const std::string& comment) {
    if (chatId == 0 || start <= 0) return Result::failure(protocol::code::kBadRequest, "Кнопка устарела");
    Json payload = Json::object();
    payload.set("slug", Json(page.slug));
    payload.set("start", Json(isoUtc(start)));
    payload.set("guest_name", Json(guestName));
    payload.set("comment", Json(comment));
    // «tg-<chat>» без двоеточия: Session::ipOf вернёт строку как есть — лимит на чат.
    return book(payload, "tg-" + std::to_string(chatId), chatId);
}

bool BookingManager::lateFor(const BookingPageRecord& page, std::time_t start, std::time_t now) {
    return page.cancelHours > 0 && start - now < static_cast<std::time_t>(page.cancelHours) * 3600;
}

BookingManager::Result BookingManager::applyPaymentFields(long long ownerId, const Json& payload, bool creating,
                                                          BookingPageRecord& page) {
    const std::string wasPrepay = creating ? std::string("none") : page.prepay;
    long long price = page.price;
    long long deposit = page.deposit;
    long long within = page.payWithinMinutes;
    long long hours = page.cancelHours;
    if (!intField(payload, "price", 0, payments::kMaxPrice, price)) {
        return Result::failure(protocol::code::kBadRequest,
                               "цена — целое число в минимальных единицах (копейках) от 0 до 100 000 000");
    }
    if (!intField(payload, "deposit", 0, payments::kMaxPrice, deposit)) {
        return Result::failure(protocol::code::kBadRequest, "депозит — целое число в минимальных единицах");
    }
    if (!intField(payload, "pay_within_minutes", 15, 1440, within)) {
        return Result::failure(protocol::code::kBadRequest, "срок оплаты — от 15 до 1440 минут");
    }
    if (!intField(payload, "cancel_hours", 0, 168, hours)) {
        return Result::failure(protocol::code::kBadRequest, "срок бесплатной отмены — от 0 до 168 часов");
    }
    const auto text = [&](const char* key, std::string& out) {
        if (!payload.contains(key)) return true;
        if (!payload.get(key).isString()) return false;
        out = trim(payload.getString(key));
        return true;
    };
    std::string currency = page.currency;
    std::string prepay = page.prepay;
    std::string url = page.paymentUrl;
    std::string note = page.paymentNote;
    std::string late = page.lateCancel;
    std::string policy = page.policyText;
    if (!text("currency", currency) || !payments::validCurrency(currency)) {
        return Result::failure(protocol::code::kBadRequest, "валюта — RUB, USD, EUR, KZT или BYN");
    }
    if (!text("prepay", prepay) || (prepay != "none" && prepay != "full" && prepay != "deposit")) {
        return Result::failure(protocol::code::kBadRequest, "предоплата — none, full или deposit");
    }
    if (!text("payment_url", url) || (!url.empty() && !validPaymentUrl(url))) {
        return Result::failure(protocol::code::kBadRequest, "ссылка оплаты — адрес https:// не длиннее 500 символов");
    }
    if (!text("payment_note", note) || !validText(note, 300, false)) {
        return Result::failure(protocol::code::kBadRequest, "реквизиты оплаты — до 300 символов в одну строку");
    }
    if (!text("late_cancel", late) || (late != "allow" && late != "block")) {
        return Result::failure(protocol::code::kBadRequest, "поздняя отмена — allow или block");
    }
    if (!text("policy_text", policy) || !validText(policy, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "текст политики отмены — не больше 500 символов");
    }
    if (prepay == "deposit" && (deposit < 1 || deposit > price)) {
        return Result::failure(protocol::code::kBadRequest, "депозит — от 1 до цены записи (в минимальных единицах)");
    }
    // Предоплата: нужна цена, способ оплаты и Pro. Способ проверяется при создании и
    // при правке полей оплаты (страницу без них можно поставить на паузу и после
    // отключения ЮKassa).
    const bool touched = creating || payload.contains("prepay") || payload.contains("price") ||
                         payload.contains("deposit") || payload.contains("payment_url") ||
                         payload.contains("payment_note");
    if (prepay != "none") {
        if (price <= 0) return Result::failure(protocol::code::kBadRequest, "для предоплаты укажите цену");
        if (touched && url.empty() && note.empty() && !(payments_ && payments_->connected(ownerId))) {
            return Result::failure(protocol::code::kBadRequest,
                                   "для предоплаты нужен способ оплаты: ссылка, реквизиты или подключённая ЮKassa");
        }
        if (wasPrepay == "none" && !plan::proAllowed(database_.db(), ownerId)) {
            return Result::failure(protocol::code::kProRequired, "предоплата доступна на тарифе Pro");
        }
    }
    page.price = price;
    page.currency = currency;
    page.prepay = prepay;
    page.deposit = deposit;
    page.paymentUrl = url;
    page.paymentNote = note;
    page.payWithinMinutes = static_cast<int>(within);
    page.cancelHours = static_cast<int>(hours);
    page.lateCancel = late;
    page.policyText = policy;
    Result result;
    result.ok = true;
    return result;
}

bool BookingManager::mutateBooking(long long bookingId, const std::function<bool(BookingRecord&)>& change,
                                   std::optional<BookingRecord>& out) {
    const auto first = bookingId > 0 ? database_.db().findBooking(bookingId) : std::nullopt;
    if (!first) return false;
    auto lock = schedule_.acquire(first->ownerId);
    if (!lock.ok()) return false;
    auto booking = database_.db().findBooking(bookingId);  // перечитываем под блокировкой
    if (!booking || !change(*booking)) return false;
    if (!database_.db().updateBooking(*booking).ok) {
        SELFVOY_LOG(log::Level::Warn, "booking") << "запись " << bookingId << " не сохранена";
        return false;
    }
    out = std::move(booking);
    return true;
}

std::vector<std::string> BookingManager::paymentLines(const BookingPageRecord& page, const Json& payment,
                                                      std::time_t now) const {
    std::vector<std::string> lines;
    if (!payments::awaiting(payment)) return lines;
    std::time_t due = 0;
    long long minutes = page.payWithinMinutes;
    if (parseIsoUtc(payment.getString("due_at"), due)) minutes = std::max<long long>(1, (due - now + 59) / 60);
    lines.push_back(std::string(payment.getString("kind") == "deposit" ? "Депозит" : "Предоплата") + ": " +
                    moneyOf(payment) + " — оплатите в течение " + slotspost::durationLabel(static_cast<int>(minutes)) +
                    ", иначе запись отменится.");
    const std::string action = payment.getString("action_url");
    if (!action.empty()) lines.push_back("Оплатить: " + action);
    if (payment.getString("method") == "link") {
        if (!page.paymentNote.empty()) lines.push_back("Как оплатить: " + page.paymentNote);
        lines.push_back("После оплаты нажмите «Я оплатил(а)» на странице записи.");
    }
    return lines;
}

void BookingManager::notifyOwnerPayment(const BookingRecord& booking, const std::string& status,
                                        const std::string& title, const std::string& body) {
    if (!notifications_) return;
    Json notify = Json::object();
    notify.set("booking_id", Json(booking.id));
    notify.set("page_id", Json(booking.pageId));
    notify.set("status", Json(status));
    notify.set("amount", Json(booking.payment.getInt("amount")));
    notify.set("currency", Json(booking.payment.getString("currency", "RUB")));
    notify.set("guest_name", Json(booking.guestName));
    notifications_->create(booking.ownerId, "booking.payment", title, body, notify);
}

void BookingManager::announcePaid(const BookingRecord& booking, bool notifyOwner) {
    taxReceiptForBooking(booking);  // F10: «выдать чек» (самозанятые, RUB)
    const auto page = database_.db().findBookingPage(booking.pageId);
    const std::string pageTitle = page ? page->title : std::string();
    if (notifyOwner) {
        notifyOwnerPayment(booking, "paid", booking.guestName + " оплатил(а) запись",
                           moneyOf(booking.payment) + (pageTitle.empty() ? "" : " · " + pageTitle));
    }
    if (!page || booking.status != "active") return;
    std::string ownerName;
    std::string ownerTimezone;
    ownerInfo(page->ownerId, ownerName, ownerTimezone);
    notifyGuest(booking, *page, "booking.paid", "Selfvoy: оплата получена", "Оплата получена",
                moneyOf(booking.payment) + " — спасибо! " + ownerName + " ждёт вас:", manageTokenOf(booking), true);
}

bool BookingManager::expireUnpaid(long long bookingId, std::time_t now) {
    std::vector<long long> tasks;
    int cancelled = 0;
    Json before;
    std::optional<BookingRecord> booking;
    const bool changed = mutateBooking(bookingId, [&](BookingRecord& record) {
        if (!payments::awaiting(record.payment)) return false;  // оплачено или уже решено
        before = record.occurrences;
        record.payment = withStatus(record.payment, "expired");
        if (record.status == "active") {
            cancelled = cancelOccurrences(record, -1, now, tasks);
            if (record.status == "cancelled") record.cancelledBy = "system";
        }
        return true;
    }, booking);
    if (!changed || !booking) return false;
    for (const long long task : tasks) deleteOwnTask(booking->ownerId, task);
    dropInactiveReminders(*booking);
    if (cancelled == 0) return true;  // запись уже была отменена — только статус оплаты
    const auto page = database_.db().findBookingPage(booking->pageId);
    if (notifications_) {
        Json notify = Json::object();
        notify.set("booking_id", Json(booking->id));
        notify.set("page_id", Json(booking->pageId));
        notify.set("cancelled", Json(static_cast<long long>(cancelled)));
        notify.set("guest_name", Json(booking->guestName));
        notify.set("reason", Json(std::string("оплата не поступила")));
        notify.set("cancelled_by", Json(std::string("system")));
        notifications_->create(booking->ownerId, "booking.cancelled",
                               booking->guestName + ": оплата не поступила — запись отменена",
                               page ? page->title : std::string(), notify);
    }
    if (page) {
        notifyGuest(*booking, *page, "booking.payment_expired", "Selfvoy: оплата не поступила — запись отменена",
                    "Запись отменена",
                    "Оплата " + moneyOf(booking->payment) + " не поступила вовремя, поэтому время освобождено. Встречи:",
                    "", false);
    }
    // F8: освобождённое время — листу ожидания (now — реальное время освобождения).
    const auto freed = freedOccurrences(before, booking->occurrences, std::time(nullptr));
    if (!freed.empty()) waitlistRelease(booking->pageId, freed, std::max(now, std::time(nullptr)));
    SELFVOY_LOG(log::Level::Info, "booking") << "запись " << booking->id << " отменена: оплата не поступила";
    return true;
}

void BookingManager::refundAfterCancel(const BookingRecord& booking, bool provider) {
    const std::string money = moneyOf(booking.payment);
    if (provider && payments_) {
        std::string error;
        if (payments_->refund(booking.ownerId, booking.payment, "rf-" + std::to_string(booking.id), error)) {
            const std::string at = isoUtc(std::time(nullptr));
            std::optional<BookingRecord> updated;
            mutateBooking(booking.id, [&](BookingRecord& record) {
                if (!record.payment.isObject() || record.payment.getString("status") == "refunded") return false;
                Json payment = withStatus(record.payment, "refunded");
                payment.set("refunded_at", Json(at));
                record.payment = payment;
                return true;
            }, updated);
            notifyOwnerPayment(updated ? *updated : booking, "refunded", "Оплата возвращена: " + booking.guestName,
                               money + " — возврат через ЮKassa");
            if (updated) taxRefundForBooking(*updated);  // F10: чек уже выдан — аннулировать
            return;
        }
        notifyOwnerPayment(booking, "refund_due", "Верните оплату вручную: " + booking.guestName,
                           "ЮKassa не приняла возврат " + money + " — верните его в личном кабинете ЮKassa");
        return;
    }
    notifyOwnerPayment(booking, "refund_due", "Верните оплату вручную: " + booking.guestName,
                       money + " — запись отменена, оплату нужно вернуть гостю");
}

PaymentManager::Remote BookingManager::syncPayment(const BookingRecord& booking, std::time_t now, int minInterval) {
    const Json& payment = booking.payment;
    if (!payments_ || !payment.isObject() || payment.getString("method") != "yookassa" ||
        payment.getString("provider_id").empty()) {
        return PaymentManager::Remote::Error;
    }
    const std::string status = payment.getString("status");
    // expired с возвратом уже решён; paid/refunded/waived не опрашиваются.
    if (status != "pending" && !(status == "expired" && payment.getString("refunded_at").empty())) {
        return PaymentManager::Remote::Error;
    }
    std::string error;
    const PaymentManager::Remote remote = payments_->poll(booking.ownerId, payment, now, minInterval, error);
    if (status == "pending") {
        if (remote == PaymentManager::Remote::Succeeded) {
            std::optional<BookingRecord> updated;
            const bool changed = mutateBooking(booking.id, [&](BookingRecord& record) {
                if (!payments::awaiting(record.payment)) return false;
                Json paid = withStatus(record.payment, "paid");
                paid.set("paid_at", Json(isoUtc(now)));
                record.payment = paid;
                return true;
            }, updated);
            if (changed && updated) announcePaid(*updated, true);
        } else if (remote == PaymentManager::Remote::Canceled) {
            expireUnpaid(booking.id, now);
        }
        return remote;
    }
    // Время уже освобождено (оплата истекла или запись отменили), а гость всё же
    // заплатил — деньги возвращаются автоматически.
    if (remote == PaymentManager::Remote::Succeeded) {
        SELFVOY_LOG(log::Level::Info, "booking") << "запись " << booking.id << ": оплата после отмены — возврат";
        refundAfterCancel(booking, true);
    }
    return remote;
}

std::size_t BookingManager::paymentTick(int limit, std::time_t now) {
    const auto awaiting = database_.db().listAwaitingPayments(limit);
    for (const auto& booking : awaiting) {
        const Json& payment = booking.payment;
        std::time_t due = 0;
        const bool hasDue = parseIsoUtc(payment.getString("due_at"), due);
        const bool overdue = !hasDue || now >= due;
        if (payment.getString("method") == "yookassa" && !payment.getString("provider_id").empty() && payments_) {
            // К сроку — последний опрос без ограничения: оплата могла пройти в последнюю минуту.
            const auto remote = syncPayment(booking, now, overdue ? 0 : PaymentManager::kPollIntervalSec);
            if (remote == PaymentManager::Remote::Succeeded || remote == PaymentManager::Remote::Canceled) continue;
            // ЮKassa не ответила — не отменяем оплаченную, возможно, запись: ждём до часа сверх срока.
            if (remote == PaymentManager::Remote::Error && overdue && hasDue && now < due + kPaymentGraceSec) continue;
        }
        if (overdue) expireUnpaid(booking.id, now);
    }
    return awaiting.size();
}

BookingManager::Result BookingManager::paymentClaim(const Json& payload, const std::string& remoteAddr) {
    if (limited("booking.change:" + Session::ipOf(remoteAddr), kChangePerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto found = hash.empty() ? std::nullopt : database_.db().findBookingByToken(hash);
    if (!found) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    Result failure;
    bool changed = false;
    std::optional<BookingRecord> booking;
    mutateBooking(found->id, [&](BookingRecord& record) {
        const Json& payment = record.payment;
        const std::string status = payment.isObject() ? payment.getString("status") : std::string();
        if (!payment.isObject()) {
            failure = Result::failure(protocol::code::kBadRequest, "у записи нет оплаты");
        } else if (payment.getString("method") != "link") {
            failure = Result::failure(protocol::code::kBadRequest, "оплата через ЮKassa подтверждается автоматически");
        } else if (status == "expired" || record.status != "active") {
            failure = Result::failure(protocol::code::kConflict, "запись отменена — оплата больше не нужна");
        } else if (status == "pending") {
            record.payment = withStatus(payment, "claimed");
            changed = true;
            return true;
        }
        return false;  // claimed / paid / waived / refunded — без изменений
    }, booking);
    if (!failure.code.empty()) return failure;
    if (!booking) booking = database_.db().findBooking(found->id);
    const auto page = booking ? database_.db().findBookingPage(booking->pageId) : std::nullopt;
    if (!booking || !page) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (changed) {
        notifyOwnerPayment(*booking, "claimed", booking->guestName + " отметил(а) оплату — проверьте поступление",
                           moneyOf(booking->payment) + " · " + page->title);
    }
    Result result;
    result.ok = true;
    result.payload.set("booking", guestBookingJson(*booking, *page, false));
    return result;
}

BookingManager::Result BookingManager::paymentSet(long long ownerId, const Json& payload) {
    const std::string status = payload.getString("status");
    if (status != "paid" && status != "waived" && status != "refunded") {
        return Result::failure(protocol::code::kBadRequest, "status — paid, waived или refunded");
    }
    const long long id = payload.getInt("booking_id");
    const auto found = id > 0 ? database_.db().findBooking(id) : std::nullopt;
    if (!found || found->ownerId != ownerId) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (!found->payment.isObject()) return Result::failure(protocol::code::kBadRequest, "у записи нет оплаты");
    const std::string at = isoUtc(std::time(nullptr));
    bool paidNow = false;
    bool refundedNow = false;
    std::optional<BookingRecord> booking;
    mutateBooking(id, [&](BookingRecord& record) {
        if (!record.payment.isObject() || record.payment.getString("status") == status) return false;
        paidNow = status == "paid";
        refundedNow = status == "refunded";
        Json payment = withStatus(record.payment, status);
        if (status == "paid") payment.set("paid_at", Json(at));
        if (status == "refunded") payment.set("refunded_at", Json(at));
        record.payment = payment;
        return true;
    }, booking);
    if (!booking) booking = database_.db().findBooking(id);
    if (!booking) return Result::failure(protocol::code::kNotFound, "запись не найдена");
    if (paidNow) announcePaid(*booking, false);  // владелец сам отметил — уведомлять его не нужно
    if (refundedNow) taxRefundForBooking(*booking);  // F10: чек уже выдан — аннулировать
    if (paidNow || refundedNow) {
        if (auto fresh = database_.db().findBooking(id)) booking = std::move(fresh);  // F10: npd, receipt_*
    }
    const auto page = database_.db().findBookingPage(booking->pageId);
    Result result;
    result.ok = true;
    result.payload.set("booking", bookingJson(*booking, page ? page->title : std::string(),
                                              page ? page->rebookAfterDays : 0));
    return result;
}

}  // namespace selfvoy
