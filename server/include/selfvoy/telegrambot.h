// selfvoy/telegrambot.h — Telegram-бот Selfvoy (F2).
//
// Владелец получает в Telegram новые записи, переносы и отмены (и другие
// уведомления NotificationsManager), гость — извещения о своей записи.
// Привязка — без таблиц ссылок: deep link https://t.me/<бот>?start=<payload>,
// payload подписан HMAC-SHA256 от секрета сервера (метка домена
// "selfvoy-telegram-link"), подпись — первые 16 байт в base64url (22 символа):
//   o_<user_id>_<expires_unix>_<sig>  — владелец, sig над "o:<user_id>:<expires>", 15 минут;
//   g_<booking_id>_<sig>              — гость, sig над "g:<booking_id>:<manage_hash>"
//                                       (перевыпуск ссылки управления делает её недействительной);
//   w_<entry_id>_<sig>                — гость листа ожидания (F8), sig над "w:<entry_id>:<token_hash>":
//                                       предложения освободившегося времени приходят в этот чат.
//   p_<slug>                          — гость спрашивает ИИ-администратора страницы (F7,
//                                       без подписи: ссылка публичная, как сама страница).
// Команды: /start <payload>, /stop (отвязать чат везде), иное — справка; текст в чате,
// привязанном к странице (p_), и кнопки as:/ab:/an — TelegramAdmin.
// Кнопка «Приду» (callback cf:<booking_id>:<start_unix>, F1) — только из чата,
// привязанного к записи; подтверждение выполняет BookingManager::telegramConfirm.
//
// Потоки: опрос getUpdates (long polling 25 с) — только на ведущем экземпляре
// (PostgreSQL advisory lock, не получил — повтор через 30 с; SELFVOY_TELEGRAM_POLL=0
// выключает опрос); отправка — своя фоновая очередь: обработчики запросов
// никогда не ждут Telegram. 403 от Telegram (бот заблокирован) — чат отвязывается.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "selfvoy/json.h"
#include "selfvoy/net.h"

namespace selfvoy {

class BookingManager;
class DatabaseManager;
class TelegramAdmin;
struct BookingRecord;
struct Config;

class TelegramBot {
public:
    // Вызов метода Bot API (подменяется в тестах). cancel — прервать ожидание.
    class Transport {
    public:
        virtual ~Transport() = default;
        virtual net::HttpResponse call(const std::string& method, const Json& body, int timeoutMs,
                                       const std::atomic<bool>* cancel, std::string& error) = 0;
    };

    static constexpr int kPollTimeoutSec = 25;
    static constexpr int kLeaderRetrySec = 30;
    static constexpr int kErrorRetrySec = 5;
    static constexpr int kOwnerLinkTtlSec = 15 * 60;
    static constexpr int kMaxBookingChats = 3;  // гость + второй получатель + запас
    static constexpr int kSendTimeoutMs = 10000;
    static constexpr std::size_t kMaxQueue = 2000;
    // Ключ advisory lock ведущего опросчика («SVOYTLG»; не пространство 0x4155 расписаний).
    static constexpr long long kLeaderLockKey = 0x53564F59544C47LL;
    static constexpr const char* kOffsetKey = "telegram.offset";

    TelegramBot(DatabaseManager& database, const Config& config, std::unique_ptr<Transport> transport = nullptr);
    ~TelegramBot();
    TelegramBot(const TelegramBot&) = delete;
    TelegramBot& operator=(const TelegramBot&) = delete;

    // Бот настроен: заданы SELFVOY_TELEGRAM_BOT_TOKEN и SELFVOY_TELEGRAM_BOT_USERNAME.
    bool enabled() const;
    std::string username() const;
    void setBookings(BookingManager* bookings) { bookings_ = bookings; }
    // F7: ИИ-администратор страниц (nullptr — выключен). Задаётся до start().
    void setAdmin(TelegramAdmin* admin) { admin_ = admin; }
    // Подмена транспорта (тесты) — до первой отправки.
    void setTransport(std::unique_ptr<Transport> transport);

    // Поток опроса (если бот включён и SELFVOY_TELEGRAM_POLL != 0). Остановка — stop().
    void start();
    // Останавливает опрос, дописывает очередь отправки (не дольше ~5 с), отпускает лидерство.
    void stop();

    // --- подписанные payload deep link (≤ 64 символов [A-Za-z0-9_-]) ---
    std::string sign(const std::string& data) const;
    std::string ownerPayload(long long userId, std::time_t expires) const;
    std::string guestPayload(long long bookingId, const std::string& manageHash) const;
    std::string waitlistPayload(long long entryId, const std::string& tokenHash) const;
    // Разбор w_<id>_<sig> без проверки подписи (её сверяют с token_hash записи листа).
    static bool parseWaitlist(const std::string& payload, long long& entryId, std::string& signature);
    bool verifyWaitlist(const std::string& payload, long long entryId, const std::string& tokenHash) const;
    // true — подпись верна и срок не истёк; userId — из payload.
    bool verifyOwner(const std::string& payload, std::time_t now, long long& userId) const;
    // Разбор g_<id>_<sig> без проверки подписи (её сверяют с manage_hash записи).
    static bool parseGuest(const std::string& payload, long long& bookingId, std::string& signature);
    bool verifyGuest(const std::string& payload, long long bookingId, const std::string& manageHash) const;
    std::string linkUrl(const std::string& payload) const;  // https://t.me/<бот>?start=<payload>
    // Ссылка гостя на запись; "" — бот выключен или запись не активна.
    std::string guestUrl(const BookingRecord& booking) const;

    // --- протокол владельца (telegram.*) ---
    Json status(long long userId);
    // false — бот не настроен (error — текст для bad_request).
    bool link(long long userId, Json& out, std::string& error);
    Json unlink(long long userId);

    // --- апдейты Bot API (публично: тесты подают их без сети) ---
    void handleUpdate(const Json& update);
    // Один getUpdates + обработка; false — ошибка запроса или ответа.
    bool pollOnce(int timeoutSec);

    // --- отправка (фоновая очередь; вызывающий не ждёт сети) ---
    // Простой текст без parse_mode; markup — reply_markup (inline_keyboard) или null.
    void sendMessage(long long chatId, const std::string& text, const Json& markup = Json());
    // chatId — для отвязки чата при 403.
    void enqueue(const std::string& method, Json body, long long chatId = 0);
    // Дождаться опустошения очереди (тесты).
    void flush();

private:
    struct Outgoing {
        std::string method;
        Json body;
        long long chatId = 0;
    };

    void pollLoop();
    void sendLoop();
    void deliver(const Outgoing& item);
    // Ждать seconds или до stop(); true — stop().
    bool waitStop(int seconds);

    void handleMessage(const Json& message);
    void handleCallback(const Json& callback);
    void linkOwner(long long chatId, const std::string& username, const std::string& payload);
    void linkGuest(long long chatId, const std::string& payload);
    void linkWaitlist(long long chatId, const std::string& payload);
    void reply(long long chatId, const std::string& text) { sendMessage(chatId, text); }

    DatabaseManager& database_;
    const Config& config_;
    BookingManager* bookings_ = nullptr;
    TelegramAdmin* admin_ = nullptr;
    std::shared_ptr<Transport> transport_;
    std::string signKey_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Outgoing> queue_;
    bool sending_ = false;
    bool stopped_ = false;  // после stop() очередь не принимает сообщений
    std::chrono::steady_clock::time_point stopDeadline_{};
    std::atomic<bool> stopping_{false};
    std::thread pollThread_;
    std::thread sendThread_;  // запускается при первой отправке
    bool leader_ = false;     // только поток опроса
};

}  // namespace selfvoy
