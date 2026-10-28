// selfvoy/bookingmanager.h — страницы записи (booking.*).
//
// Владелец заводит постоянную публичную ссылку <SELFVOY_PUBLIC_URL>/#/b/<slug>:
// название, длительность встречи, на сколько дней вперёд открыта запись, лимит
// записей в день, минимальный запас до начала, можно ли брать еженедельную серию.
// Гость без Selfvoy открывает ссылку (booking.get — публичный метод), видит
// свободное время владельца (AI-сервис, POST /v1/agent/slots — рабочие часы в
// поясе владельца) и записывается (booking.book). Сервер не доверяет AI: сам
// проверяет длительность, окно, запас, занятость, пересечения и лимит в день.
//
// Гость получает одноразово показанный токен ссылки управления #/m/<токен>
// (в БД — только sha256): по ней он переносит встречу (booking.reschedule,
// не больше 3 раз) или отменяет одну встречу серии либо все (booking.cancel).
// Владельцу — задача с напоминанием на каждую встречу и уведомления
// booking.created / booking.rescheduled / booking.cancelled / booking.confirmed.
// Встречи записей занимают время владельца (MemoryManager::busy) — в переговорах,
// приглашениях и на других страницах. Гостю — напоминания за сутки и за 2 часа
// (booking_reminders, F1) и подпись «Запись через Selfvoy» (F3). Пакеты/абонементы
// и второй получатель извещений (F5) — packages.cpp. Лист ожидания и «пора на повторный
// приём» (F8) — waitlist.cpp, чеки «Мой налог» (F10) — tax.cpp.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "selfvoy/json.h"
#include "selfvoy/mailprovider.h"
#include "selfvoy/payments.h"

namespace selfvoy {

class DatabaseManager;
class MemoryManager;
class NotificationsManager;
class ReferralManager;
class ScheduleLock;
class TelegramBot;
struct BookingPageRecord;
struct BookingRecord;
struct BookingReminderRecord;
struct Config;
struct PackagePurchaseRecord;
struct WaitlistEntryRecord;

// Извещение гостю: одно сообщение уходит на все каналы записи — email гостя (и
// второго получателя, F5), привязанные Telegram-чаты (booking_telegram). Тексты
// собирает сервер; имя и комментарий гостя туда не попадают.
struct GuestNotice {
    MailNotice mail;                  // текст письма (mail.to задаётся на каждый адрес)
    std::vector<std::string> emails;  // адреса, лимит на каждый уже списан
    std::vector<long long> chats;     // Telegram-чаты, лимит на каждый уже списан
    std::string telegramText;         // простой текст для Telegram
    Json telegramMarkup;              // reply_markup (inline_keyboard: url- и callback-кнопки) или null
};

class BookingManager {
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

    static constexpr int kMaxPages = 10;
    static constexpr int kMaxReschedules = 3;
    static constexpr int kGetPerMinute = 30;
    static constexpr int kBookPerMinute = 10;
    static constexpr int kBookPerHour = 30;
    static constexpr int kChangePerMinute = 10;
    static constexpr int kMailPerAddressHour = 10;
    // Письма гостям несут текст владельца (название страницы, имя) с домена
    // сервиса: 60 в час хватает живому репетитору, но не рассылке.
    static constexpr int kMailPerOwnerHour = 60;
    // Состоявшихся записей на одну страницу в час.
    static constexpr int kBookPerPageHour = 20;
    // Ключ лимита писем: регистр, «+метка» и точки Gmail не дают обойти лимит на ящик.
    static std::string mailboxKey(const std::string& email);
    static constexpr int kPageCreatesPerHour = 20;
    static constexpr int kResendPerMinute = 5;

    BookingManager(DatabaseManager& database, MemoryManager& memory, NotificationsManager* notifications,
                   ReferralManager& referrals, ScheduleLock& scheduleLock, const Config& config);
    ~BookingManager();

    // Владелец (нужен вход).
    Result pageCreate(long long ownerId, const Json& payload);
    Result pageList(long long ownerId);
    Result pageUpdate(long long ownerId, const Json& payload);
    Result pageDelete(long long ownerId, const Json& payload);
    Result list(long long ownerId, const Json& payload);
    Result ownerCancel(long long ownerId, const Json& payload);

    // Гость (без входа): slug страницы или токен ссылки управления в payload.
    Result get(const Json& payload, const std::string& remoteAddr);
    // telegramChat != 0 — запись из Telegram (F7, telegramBook): чат привязывается к записи
    // до извещения гостю.
    Result book(const Json& payload, const std::string& remoteAddr, long long telegramChat = 0);
    Result manage(const Json& payload, const std::string& remoteAddr);
    Result reschedule(const Json& payload, const std::string& remoteAddr);
    Result cancel(const Json& payload, const std::string& remoteAddr);
    // Потерянная ссылка управления: новые ссылки на активные записи этой страницы
    // уходят письмом на email гостя (старые перестают работать). Ответ всегда
    // одинаковый — по нему нельзя узнать, записан ли кто-то с этим адресом.
    Result resendLink(const Json& payload, const std::string& remoteAddr);
    // «Приду» (F1): {token, occurrence? (start; по умолчанию ближайшая предстоящая
    // активная)} → {booking}. Ставит у встречи confirmed_at; повтор — без изменений и
    // без второго уведомления владельцу (booking.confirmed). Лимиты — как у переноса.
    Result confirm(const Json& payload, const std::string& remoteAddr);

    // Пост «свободные окна» (F6, владелец): {page_id, days? (1–14, 7), format?
    // (telegram|vk|plain)} → {text, slots, url}. Окна — те же, что гость видит в
    // booking.get. Реализация — slotspost.cpp (текст — selfvoy/slotspost.h).
    static constexpr int kSlotsPostPerHour = 30;
    Result slotsPost(long long ownerId, const Json& payload);

    // Токен ссылки управления → ключ поиска в БД (hex sha256); "" — заведомо неверный.
    static std::string tokenHash(const std::string& token);
    // Адрес страницы: 10 символов [a-z0-9].
    static bool validSlug(const std::string& slug);

    using Busy = std::vector<std::pair<std::time_t, std::time_t>>;

    // Дождаться отправки извещений гостям из очереди (тесты, остановка).
    void flushMail();

    // --- Рост (S1a) ---------------------------------------------------------
    static constexpr const char* kManageTokenAad = "selfvoy-manage-token";
    // Telegram-бот (может быть nullptr — бот не настроен). Задаётся до первого запроса.
    void setTelegram(TelegramBot* telegram) { telegram_ = telegram; }
    // Токен ссылки управления из manage_token_enc; "" — нет (старая запись) или не расшифровался.
    std::string manageTokenOf(const BookingRecord& booking) const;
    // Ответ бота гостю после привязки чата: ближайшая встреча (местное время владельца).
    std::string telegramGreeting(const BookingRecord& booking);
    // Кнопка «Приду» из Telegram (callback cf:<booking_id>:<start_unix>): чат уже
    // проверен ботом. Логику booking.confirm подключает стадия F1; пока — «недоступно».
    Result telegramConfirm(long long bookingId, std::time_t start);

    // --- F1: напоминания гостю (S1b) ----------------------------------------
    static constexpr int kReminder24hSec = 24 * 3600;
    static constexpr int kReminder2hSec = 2 * 3600;
    static constexpr int kReminder24hMinLeadSec = 25 * 3600;    // 24h — если до начала ≥ 25 ч
    static constexpr int kReminder2hMinLeadSec = 150 * 60;      // 2h — если до начала ≥ 2 ч 30 мин
    // Запланировать напоминания на активные будущие встречи записи (повтор не дублирует).
    void scheduleReminders(const BookingRecord& booking, std::time_t now);
    // Удалить неотправленные напоминания неактивных встреч (вся запись отменена — все).
    void dropInactiveReminders(const BookingRecord& booking);
    // Планировщик сервера: захватить до limit наступивших напоминаний и отправить гостю
    // (email + Telegram). now — параметр (юнит-тесты подставляют будущее время).
    // Возвращает число захваченных (отправленных и пропущенных).
    std::size_t reminderTick(int limit, std::time_t now);

    // --- F3: подпись «Запись через Selfvoy» ----------------------------------
    // {show, text, url}: show — пока владелец не скрыл подпись при действующем Pro.
    Json branding(const BookingPageRecord& page);

    // --- F4: цена, предоплата, политика отмены (S2a) -------------------------
    // Оплата записи (selfvoy/payments.h). nullptr — оплата не настроена (тесты без сервера).
    void setPayments(PaymentManager* payments) { payments_ = payments; }
    static constexpr int kPaymentGraceSec = 3600;  // ЮKassa не отвечает — не дольше часа сверх срока
    // «Я оплатил(а)» (гость, способ link): {token} → {booking}; pending → claimed,
    // владельцу — booking.payment {booking_id, status: claimed}. claimed не истекает.
    Result paymentClaim(const Json& payload, const std::string& remoteAddr);
    // Владелец: {booking_id, status: paid|waived|refunded} → {booking}.
    Result paymentSet(long long ownerId, const Json& payload);
    // Планировщик: ожидающие оплаты — опрос ЮKassa (не чаще раза в 30 с на платёж),
    // succeeded → paid, canceled или срок due_at прошёл (pending) → запись отменяется
    // (cancelled_by: system), оплата expired. now — параметр (юнит-тесты). Возвращает
    // число просмотренных оплат.
    std::size_t paymentTick(int limit, std::time_t now);
    // Поздняя отмена: до начала встречи меньше cancel_hours часов (0 — никогда не поздно).
    static bool lateFor(const BookingPageRecord& page, std::time_t start, std::time_t now);

    // --- F5: пакеты (абонементы) и второй получатель (S2b), packages.cpp -----
    // Пакет — N занятий на странице записи: гость покупает его при записи
    // (booking.book с package_id — id предложения страницы) или получает от владельца
    // (package.issue), затем записывается по ссылке пакета (booking.book с
    // package_token). Каждая активная встреча записи по пакету — одно занятие;
    // отмена не позже cancel_hours возвращает занятие, поздняя — нет. Ссылка пакета
    // <PUBLIC_URL>/#/p/<токен>: в БД — sha256 и шифр токена (AAD ниже, для писем).
    static constexpr const char* kPackageTokenAad = "selfvoy-package-token";
    static constexpr int kMaxPackageOffers = 5;
    static constexpr int kMaxPackageSessions = 1000;  // sessions_total после корректировок
    static constexpr int kPackageLowLeft = 1;         // остаток ≤ 1 — извещение «продлите»
    // Гость (публичные): {token} → {package}; «Я оплатил(а)» за пакет (способ link).
    Result packageGet(const Json& payload, const std::string& remoteAddr);
    Result packagePaymentClaim(const Json& payload, const std::string& remoteAddr);
    // Владелец.
    Result packageList(long long ownerId, const Json& payload);
    Result packageIssue(long long ownerId, const Json& payload);
    Result packageAdjust(long long ownerId, const Json& payload);
    Result packageCancel(long long ownerId, const Json& payload);
    Result packagePaymentSet(long long ownerId, const Json& payload);
    // Планировщик: оплата пакетов (опрос ЮKassa, истечение неоплаченных — покупка
    // cancelled, её записи отменяются системой) и срок действия (active → expired по
    // valid_until). now — параметр (юнит-тесты). Возвращает число просмотренных пакетов.
    std::size_t packageTick(int limit, std::time_t now);
    // Токен ссылки пакета из token_enc ("" — не расшифровался).
    std::string packageTokenOf(const PackagePurchaseRecord& record) const;
    // «m***@example.com» — адрес второго получателя в виде гостя.
    static std::string maskEmail(const std::string& email);

    // --- F8: лист ожидания и «пора на повторный приём» (waitlist.cpp) --------
    // Гость встаёт в лист ожидания страницы (поле waitlist, Pro): освободилось подходящее
    // время (отмена гостем/владельцем/системой, перенос) — оно держится за самой старой
    // записью листа waitlist_hold_minutes (держание — занятость владельца: MemoryManager::busy),
    // гостю — письмо (и бот) со ссылкой #/w/<токен>; забрал — обычная запись (booking.book:
    // та же проверка и оплата), не забрал — время уходит следующему, затем освобождается.
    static constexpr const char* kWaitlistTokenAad = "selfvoy-waitlist-token";
    static constexpr int kWaitlistLifeDays = 30;     // запись листа живёт 30 дней
    static constexpr int kMaxWaitlistDays = 14;      // желаемых дней — не больше 14
    static constexpr int kMaxWaitlistPerPage = 200;  // активных записей листа на страницу
    static constexpr int kRebookStaleSec = 3 * 86400;  // «пора записаться снова» опоздало больше чем на 3 дня — не шлём
    // Гость (публичные).
    Result waitlistJoin(const Json& payload, const std::string& remoteAddr);
    Result waitlistGet(const Json& payload, const std::string& remoteAddr);
    Result waitlistClaim(const Json& payload, const std::string& remoteAddr);
    Result waitlistLeave(const Json& payload, const std::string& remoteAddr);
    // Владелец.
    Result waitlistList(long long ownerId, const Json& payload);
    Result waitlistRemove(long long ownerId, const Json& payload);
    // Планировщик: истёкшие предложения (время — следующему в листе) и записи листа старше
    // 30 дней. now — параметр (юнит-тесты). Возвращает число просмотренных записей листа.
    std::size_t waitlistTick(int limit, std::time_t now);
    // Планировщик: «пора записаться снова» — атомарный захват наступивших (ровно один раз на
    // запись), письмо (+ бот, если подписан на запись). Возвращает число захваченных.
    std::size_t rebookTick(int limit, std::time_t now);
    // Токен ссылки листа ожидания из token_enc ("" — не расшифровался).
    std::string waitlistTokenOf(const WaitlistEntryRecord& entry) const;

    // --- F10: чек «Мой налог» (tax.cpp) ---------------------------------------
    // tax.pending {limit?} → {items}; tax.receiptDone {booking_id | package_id, receipt_url?}.
    static constexpr int kTaxPendingMax = 200;
    Result taxPending(long long ownerId, const Json& payload);
    Result taxReceiptDone(long long ownerId, const Json& payload);
    // --- F7: ИИ-администратор в Telegram (диалог ведёт TelegramAdmin) -------
    // Свободное время для бота: те же окна, что гость видит в booking.get, ближайшие
    // первыми, не больше limit. label — «чт, 15 октября, 10:30», when — «чт, 15 октября
    // 2026, 10:30 (Europe/Moscow)» (местное время владельца). false — AI-сервис недоступен.
    struct FreeSlot {
        std::time_t start = 0;
        std::time_t end = 0;
        std::string label;
        std::string when;
    };
    bool freeSlots(const BookingPageRecord& page, std::size_t limit, std::vector<FreeSlot>& out);
    // Запись из Telegram — обычный путь booking.book (те же проверки времени, лимиты дня,
    // оплата, задачи, уведомления): имя — из Telegram, email нет, комментарий — источник;
    // лимиты частоты записи — на чат («tg-<chat>» вместо IP). Чат привязывается к записи
    // (извещения и напоминания гостю — в этот чат).
    Result telegramBook(const BookingPageRecord& page, long long chatId, std::time_t start,
                        const std::string& guestName, const std::string& comment);
    // https://t.me/<бот>?start=p_<slug> — пока ai_admin включён, бот настроен, у владельца
    // Pro и страница открыта; иначе "".
    std::string telegramAskUrl(const BookingPageRecord& page);
    // Ссылка страницы записи <PUBLIC_URL>/#/b/<slug> ("" — нет PUBLIC_URL).
    std::string pageUrl(const BookingPageRecord& page) const;

private:
    // F7: поле страницы ai_admin (pageCreate/pageUpdate): bool, нужен бот, Pro.
    Result applyAiAdmin(long long ownerId, const Json& payload, bool creating, BookingPageRecord& page);
    // Пакет записи (booking.book): покупка (offerId > 0, offer — предложение страницы)
    // или запись по пакету (packageId > 0, найден по package_token).
    struct PackageIntent {
        long long offerId = 0;
        Json offer;
        long long packageId = 0;
        bool any() const { return offerId > 0 || packageId > 0; }
    };
    struct Offer {
        std::time_t start = 0;
        std::time_t end = 0;
        std::string day;    // местная дата владельца YYYY-MM-DD
        std::string local;  // местное время владельца YYYY-MM-DDTHH:MM ("" — неизвестно)
    };
    struct Cached {
        std::chrono::steady_clock::time_point at;
        std::string version;
        std::vector<Offer> offers;
    };

    Json pageJson(const BookingPageRecord& page);
    // rebookDays — rebook_after_days страницы записи (F8: rebook_due_at в виде владельца).
    Json bookingJson(const BookingRecord& booking, const std::string& pageTitle, int rebookDays = 0) const;
    Json guestBookingJson(const BookingRecord& booking, const BookingPageRecord& page, bool withSlots);
    void ownerInfo(long long ownerId, std::string& name, std::string& timezone) const;

    // Еженедельная серия от first в местном времени владельца (AI POST /v1/agent/series):
    // повтор сохраняет местное время при переходе на летнее/зимнее. false — сервис недоступен
    // или ответ не прошёл проверку.
    bool series(const BookingPageRecord& page, const Offer& first, int count, std::vector<Offer>& out);
    // Кандидаты от AI-сервиса (кэш на минуту); false — сервис недоступен.
    bool offers(const BookingPageRecord& page, std::vector<Offer>& out);
    // Свободное время страницы сейчас: запас, занятость, лимит в день. skipBooking/skipStart —
    // встреча, которую переносят: её интервал и место в лимите дня не учитываются; skipHold —
    // запись листа ожидания, чьё держание не мешает (гость забирает предложенное время, F8).
    std::vector<Offer> available(const BookingPageRecord& page, const std::vector<Offer>& candidates,
                                 long long skipBooking = 0, const std::string& skipStart = "",
                                 long long skipHold = 0);
    // Занятость владельца (MemoryManager::busy: расписание, записи, держания листа ожидания)
    // без переносимой встречи и без держания skipHold.
    Busy busyFor(long long ownerId, long long skipBooking, const std::string& skipStart, long long skipHold);
    // booking.book после разбора запроса (кандидаты и серия уже получены): проверка под
    // блокировкой расписания, оплата, сохранение, задачи, уведомления, письмо. waitlistEntry —
    // гость забирает предложение листа ожидания (F8): держание проверяется и закрывается
    // под той же блокировкой, владельцу — waitlist.booked вместо booking.created. telegramChat —
    // запись из Telegram (F7): чат гостя привязывается к записи сразу после сохранения.
    Result commitBooking(const BookingPageRecord& page, BookingRecord booking, const PackageIntent& intent,
                         const std::vector<Offer>& candidates, const std::vector<Offer>& plan, long long count,
                         long long waitlistEntry, long long telegramChat = 0);
    // Активные встречи страницы по местным дням (для лимита max_per_day) и держания листа
    // ожидания этой страницы (F8), кроме держания skipHold.
    std::unordered_map<std::string, int> dayCounts(const BookingPageRecord& page, long long skipBooking,
                                                   const std::string& skipStart, long long skipHold = 0);

    long long createTask(const BookingRecord& booking, const std::string& pageTitle, std::time_t start);
    // Удалить задачу встречи — только если она принадлежит владельцу записи.
    void deleteOwnTask(long long ownerId, long long taskId);
    // Владелец страницы может принимать гостей: аккаунт активен и email подтверждён.
    bool ownerAccepts(long long ownerId) const;
    // Отмена встреч: index < 0 — все будущие активные. Возвращает число отменённых;
    // id их задач — в tasks (удалять после сохранения записи).
    int cancelOccurrences(BookingRecord& booking, int index, std::time_t now, std::vector<long long>& tasks);
    static int findOccurrence(const BookingRecord& booking, const std::string& start, std::time_t now);

    // Лимит по ключу ещё не исчерпан (событие не засчитывается).
    bool underLimit(const std::string& key, int limit, int windowSeconds);
    bool limited(const std::string& key, int perWindow, int windowSeconds = 60);

    // Извещение гостю о записи — на все каналы (email, Telegram-чаты записи; лимиты на
    // адрес/чат и на владельца). Тема — только текст сервера; в теле — название страницы
    // и имя владельца (экранируются), имя гостя не попадает никуда, чтобы форму нельзя
    // было использовать для рассылки произвольного текста. Уходит из фоновой очереди:
    // запрос гостя не ждёт SMTP и Telegram. reserved — лимит адреса гостя уже списан.
    // footer — дополнительные строки письма (в Telegram не уходят).
    void notifyGuest(const BookingRecord& booking, const BookingPageRecord& page, const std::string& kind,
                     const std::string& subject, const std::string& title, const std::string& lead,
                     const std::string& token, bool onlyActive, bool reserved = false,
                     const std::vector<std::string>& footer = {});
    // Получатели извещения (email и Telegram-чаты записи) с лимитами на адрес/чат и на
    // владельца. false — некому отправлять (или лимит владельца исчерпан).
    bool guestRecipients(const BookingRecord& booking, const BookingPageRecord& page, const std::string& kind,
                         bool reserved, GuestNotice& notice);
    // Строка подписи для писем и бота гостю ("" — подпись скрыта или нет PUBLIC_URL).
    std::string brandingLine(const BookingPageRecord& page);
    // Поставить извещение в фоновую очередь отправки.
    void enqueueNotice(GuestNotice notice);
    // Отправить одно наступившее напоминание (false — пропущено).
    bool sendReminder(const BookingReminderRecord& reminder, std::time_t now);
    // Общая логика «Приду»: запись по id (bookingId > 0) или по хэшу токена, встреча — start
    // (0 — ближайшая предстоящая активная); чтение и сохранение — под блокировкой расписания.
    // changed — confirmed_at поставлен этим вызовом (тогда же — уведомление владельцу).
    Result confirmOccurrence(long long bookingId, const std::string& hash, std::time_t start, bool& changed,
                             std::optional<BookingRecord>& out);
    // F4: проверка и запись полей цены/предоплаты/политики из payload в page (создание —
    // creating). Ошибка — failure с понятным текстом; ok — поля применены.
    Result applyPaymentFields(long long ownerId, const Json& payload, bool creating, BookingPageRecord& page);
    // Изменение записи под блокировкой расписания владельца: перечитать, change() (false —
    // ничего не менять), сохранить. false — записи нет, блокировка не взята, нечего менять
    // или сохранение не удалось. out — запись после изменения.
    bool mutateBooking(long long bookingId, const std::function<bool(BookingRecord&)>& change,
                       std::optional<BookingRecord>& out);
    // Опросить ЮKassa по оплате записи и применить ответ: pending — succeeded → paid,
    // canceled → отмена записи; expired (запись уже отменена), но гость всё же оплатил —
    // автоматический возврат. minInterval — не чаще (0 — сразу).
    PaymentManager::Remote syncPayment(const BookingRecord& booking, std::time_t now, int minInterval);
    // Оплата получена (ЮKassa или владелец): уведомление владельцу и извещение гостю.
    void announcePaid(const BookingRecord& booking, bool notifyOwner);
    // Оплата не поступила к сроку (или ЮKassa отменила платёж): отмена записи системой.
    bool expireUnpaid(long long bookingId, std::time_t now);
    // Возврат после отмены записи: ЮKassa — POST /refunds (rf-<id>), иначе (или ошибка) —
    // владельцу «верните оплату вручную». provider — вернуть через ЮKassa.
    void refundAfterCancel(const BookingRecord& booking, bool provider);
    // Уведомление владельцу booking.payment {booking_id, page_id, status, amount, currency, guest_name}.
    void notifyOwnerPayment(const BookingRecord& booking, const std::string& status, const std::string& title,
                            const std::string& body);
    // Строки письма о записи с предоплатой: сумма, срок, ссылка или реквизиты.
    std::vector<std::string> paymentLines(const BookingPageRecord& page, const Json& payment, std::time_t now) const;
    // --- F5 (packages.cpp) ---
    // Поле страницы packages (pageCreate/pageUpdate): проверка, сохранение id, Pro.
    Result applyPackageOffers(long long ownerId, const Json& payload, bool creating, BookingPageRecord& page);
    // booking.book: разбор package_id / package_token до блокировки расписания.
    Result packageIntent(const BookingPageRecord& page, const Json& payload, long long count, PackageIntent& out);
    // Пакет пригоден для записи count встреч на странице pageId сейчас.
    static Result packageUsable(const PackagePurchaseRecord& record, long long pageId, long long count,
                                std::time_t now);
    // Новая покупка пакета при записи (под блокировкой расписания владельца). token — сырой.
    Result createPurchase(const BookingPageRecord& page, const Json& offer, const BookingRecord& guest, int used,
                          std::time_t firstStart, PackagePurchaseRecord& out, std::string& token);
    // Изменение пакета под блокировкой расписания владельца (как mutateBooking).
    bool mutatePackage(long long packageId, const std::function<bool(PackagePurchaseRecord&)>& change,
                       std::optional<PackagePurchaseRecord>& out);
    // Изменить число использованных занятий (delta > 0 — списание, < 0 — возврат) —
    // вызывающий держит блокировку расписания владельца. Статус used/active — по остатку.
    bool shiftPackageUsedLocked(long long packageId, int delta, std::time_t now,
                                std::optional<PackagePurchaseRecord>& out);
    // Возвращаемые занятия отменённых встреч: встречи, ставшие cancelled (before → after),
    // не позже срока cancel_hours страницы (all — все, отмена владельцем; page может быть nullptr).
    static int returnableSessions(const Json& before, const Json& after, const BookingPageRecord* page,
                                  std::time_t now, bool all);
    Json packageOwnerJson(const PackagePurchaseRecord& record, const std::string& pageTitle) const;
    Json packageGuestJson(const PackagePurchaseRecord& record, const BookingPageRecord& page);
    std::string packageUrl(const std::string& token) const;
    // Строки письма о пакете: занятия, ссылка, оплата.
    std::vector<std::string> packageLines(const PackagePurchaseRecord& record, const BookingPageRecord& page,
                                          const std::string& token, std::time_t now) const;
    // Получатели извещения о пакете: email гостя и второго получателя, Telegram-чаты
    // записей пакета (лимиты — как у извещений записи).
    bool packageRecipients(const PackagePurchaseRecord& record, const BookingPageRecord& page,
                           const std::string& kind, GuestNotice& notice);
    void notifyPackage(const PackagePurchaseRecord& record, const BookingPageRecord& page, const std::string& kind,
                       const std::string& subject, const std::string& title, const std::string& lead,
                       const std::vector<std::string>& lines, const std::string& linkUrl, const std::string& linkText,
                       const std::vector<std::string>& footer);
    // Остаток ≤ 1 после списания: извещение гостю (+cc) и package.low владельцу.
    void packageLow(const PackagePurchaseRecord& record);
    // payload уведомления владельцу о пакете ({package_id, page_id, guest_name} + extra);
    // виды — package.purchased / package.payment / package.low (литералы у вызывающих).
    Json packageNotice(const PackagePurchaseRecord& record, const Json& extra) const;
    PaymentManager::Remote syncPackagePayment(const PackagePurchaseRecord& record, std::time_t now, int minInterval);
    void announcePackagePaid(const PackagePurchaseRecord& record, bool notifyOwner);
    // Оплата пакета не поступила (или ЮKassa отменила платёж): покупка cancelled,
    // активные записи пакета отменяются системой.
    bool expirePackagePayment(long long packageId, std::time_t now);
    void refundPackage(const PackagePurchaseRecord& record, bool provider);
    // Отмена записи системой (оплата пакета не поступила) — задачи, напоминания, извещения.
    void cancelBySystem(long long bookingId, std::time_t now, const std::string& reason);
    // --- F8 (waitlist.cpp) ---
    // Поля страницы waitlist / waitlist_hold_minutes / rebook_after_days: проверка, Pro.
    Result applyWaitlistFields(long long ownerId, const Json& payload, bool creating, BookingPageRecord& page);
    // Встречи, ставшие cancelled (before → after) и ещё не начавшиеся: освободившееся время.
    static std::vector<Json> freedOccurrences(const Json& before, const Json& after, std::time_t now);
    // Освободилось время страницы: каждому интервалу — самая старая подходящая запись листа
    // (кроме excludeEntry), держание и извещение гостю. Блокировку расписания берёт сама.
    void waitlistRelease(long long pageId, const std::vector<Json>& freed, std::time_t now, long long excludeEntry = 0);
    // Изменение записи листа под блокировкой расписания владельца (как mutateBooking).
    bool mutateWaitlist(long long entryId, const std::function<bool(WaitlistEntryRecord&)>& change,
                        std::optional<WaitlistEntryRecord>& out);
    Json waitlistGuestJson(const WaitlistEntryRecord& entry) const;
    Json waitlistOwnerJson(const WaitlistEntryRecord& entry, const std::string& pageTitle) const;
    std::string waitlistUrl(const std::string& token) const;
    std::string waitlistTelegramUrl(const WaitlistEntryRecord& entry) const;
    // Извещение записи листа: email гостя (лимит на адрес и на владельца) и чат бота, если подписан.
    void notifyWaitlist(const WaitlistEntryRecord& entry, const BookingPageRecord& page, const std::string& kind,
                        const std::string& subject, const std::string& title, const std::string& lead,
                        const std::vector<std::string>& lines, const std::string& linkUrl,
                        const std::string& linkText, const std::vector<std::string>& footer);
    // Одно наступившее «пора записаться снова» (false — пропущено).
    bool sendRebook(const BookingRecord& booking, std::time_t now);
    // --- F10 (tax.cpp) ---
    // Оплата записи / пакета стала paid: при npd_receipts и RUB — задача и tax.receipt.
    void taxReceiptForBooking(const BookingRecord& booking);
    void taxReceiptForPackage(const PackagePurchaseRecord& record);
    // Оплата стала refunded, а чек уже отмечен: tax.refund и задача «Аннулировать чек».
    void taxRefundForBooking(const BookingRecord& booking);
    void taxRefundForPackage(const PackagePurchaseRecord& record);
    // Конец текущего местного дня владельца (23:59) в UTC: пояс — AI-сервис (POST
    // /v1/agent/series), иначе сдвиг из local встречи (hint), иначе UTC.
    std::time_t ownerDayEnd(long long ownerId, std::time_t now, const Json& hint);
    void mailLoop();
    std::string sealManageToken(const std::string& token) const;
    std::string manageUrl(const std::string& token) const;

    DatabaseManager& database_;
    MemoryManager& memory_;
    NotificationsManager* notifications_;
    ReferralManager& referrals_;
    const Config& config_;
    std::unique_ptr<MailProvider> mail_;

    // Запись, перенос и отмена: проверка «свободно» и сохранение — под блокировкой
    // расписания владельца (общей с invite.respond и с другими экземплярами сервера).
    ScheduleLock& schedule_;

    std::mutex cacheMutex_;
    std::unordered_map<long long, Cached> cache_;

    struct Window {
        std::chrono::steady_clock::time_point start;
        int count = 0;
    };
    std::mutex limitsMutex_;
    std::unordered_map<std::string, Window> limits_;

    std::mutex mailMutex_;
    std::condition_variable mailCv_;
    std::deque<GuestNotice> mailQueue_;
    bool mailBusy_ = false;
    bool mailStop_ = false;
    std::thread mailThread_;  // запускается при первом извещении
    TelegramBot* telegram_ = nullptr;
    PaymentManager* payments_ = nullptr;
};

}  // namespace selfvoy
