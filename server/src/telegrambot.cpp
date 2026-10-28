// selfvoy/telegrambot.cpp — Telegram-бот: привязка по подписанным ссылкам,
// long polling на ведущем экземпляре, фоновая очередь отправки.
#include "selfvoy/telegrambot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc
#include "selfvoy/telegramadmin.h"

namespace selfvoy {

namespace {

constexpr std::size_t kSignatureLength = 22;  // base64url от 16 байт без '='
constexpr std::size_t kMaxPayload = 64;       // ограничение Telegram для start
constexpr std::size_t kMaxText = 4000;        // sendMessage: до 4096 символов

const char* const kHelp =
    "Это бот Selfvoy. Владельцу страницы записи он присылает новые записи, переносы и отмены, "
    "гостю — напоминания о встрече.\n"
    "Подключить — по ссылке из приложения Selfvoy (Настройки → Telegram) или со страницы вашей записи.\n"
    "Отключить — /stop";
const char* const kInvalidLink =
    "Ссылка недействительна или устарела. Получите новую в Selfvoy: в настройках (для уведомлений) "
    "или на странице вашей записи (для напоминаний).";

// Только цифры, без знака и ведущих нулей сверх одного; ≤ 18 знаков (влезает в long long).
bool parseNumber(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) return false;
    if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) return false;
    if (text.size() > 1 && text[0] == '0') return false;
    out = std::atoll(text.c_str());
    return true;
}

bool validSignature(const std::string& sig) {
    return sig.size() == kSignatureLength && std::all_of(sig.begin(), sig.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
           });
}

// Число до следующего '_' начиная с pos; pos сдвигается за '_'.
bool takeNumber(const std::string& text, std::size_t& pos, long long& out) {
    const std::size_t end = text.find('_', pos);
    if (end == std::string::npos) return false;
    if (!parseNumber(text.substr(pos, end - pos), out)) return false;
    pos = end + 1;
    return true;
}

// UTF-8 без разрыва многобайтового символа: не длиннее limit байт.
std::string clip(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end) + "…";
}

// «/start@selfvoy_bot payload» → command="/start", argument="payload".
void splitCommand(const std::string& text, std::string& command, std::string& argument) {
    const std::size_t space = text.find_first_of(" \t\n");
    command = text.substr(0, space);
    const std::size_t at = command.find('@');
    if (at != std::string::npos) command = command.substr(0, at);
    for (auto& ch : command) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    argument.clear();
    if (space != std::string::npos) {
        const std::size_t begin = text.find_first_not_of(" \t\n", space);
        if (begin != std::string::npos) {
            const std::size_t end = text.find_last_not_of(" \t\n");
            argument = text.substr(begin, end - begin + 1);
        }
    }
}

class HttpTransport : public TelegramBot::Transport {
public:
    HttpTransport(std::string apiUrl, std::string token) : apiUrl_(std::move(apiUrl)), token_(std::move(token)) {}

    net::HttpResponse call(const std::string& method, const Json& body, int timeoutMs,
                           const std::atomic<bool>* cancel, std::string& error) override {
        // Токен — часть пути запроса: в логи URL не пишется.
        return net::httpRequest("POST", apiUrl_ + "/bot" + token_ + "/" + method,
                                {{"Content-Type", "application/json"}}, body.dump(), timeoutMs, error, cancel);
    }

private:
    std::string apiUrl_;
    std::string token_;
};

}  // namespace

TelegramBot::TelegramBot(DatabaseManager& database, const Config& config, std::unique_ptr<Transport> transport)
    : database_(database), config_(config) {
    if (transport) {
        transport_ = std::move(transport);
    } else {
        transport_ = std::make_shared<HttpTransport>(config_.telegramApiUrl, config_.telegramBotToken);
    }
    signKey_ = crypto::hmacSha256(config_.jwtSecret, "selfvoy-telegram-link");
}

TelegramBot::~TelegramBot() { stop(); }

bool TelegramBot::enabled() const { return config_.telegramEnabled(); }

std::string TelegramBot::username() const { return enabled() ? config_.telegramBotUsername : std::string(); }

void TelegramBot::setTransport(std::unique_ptr<Transport> transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = std::move(transport);
}

void TelegramBot::start() {
    if (!enabled() || !config_.telegramPoll) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || pollThread_.joinable()) return;
    pollThread_ = std::thread([this] { pollLoop(); });
    SELFVOY_LOG(log::Level::Info, "telegram") << "бот @" << config_.telegramBotUsername << " запущен";
}

void TelegramBot::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        stopped_ = true;
        stopDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    stopping_.store(true);
    cv_.notify_all();
    if (pollThread_.joinable()) pollThread_.join();
    if (sendThread_.joinable()) sendThread_.join();
}

bool TelegramBot::waitStop(int seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping_.load(); });
}

// ------------------------------------------------------------- подписи и ссылки

std::string TelegramBot::sign(const std::string& data) const {
    std::string sig = crypto::base64UrlEncode(crypto::hmacSha256(signKey_, data).substr(0, 16));
    while (!sig.empty() && sig.back() == '=') sig.pop_back();
    return sig;
}

std::string TelegramBot::ownerPayload(long long userId, std::time_t expires) const {
    const std::string id = std::to_string(userId);
    const std::string until = std::to_string(static_cast<long long>(expires));
    return "o_" + id + "_" + until + "_" + sign("o:" + id + ":" + until);
}

std::string TelegramBot::guestPayload(long long bookingId, const std::string& manageHash) const {
    const std::string id = std::to_string(bookingId);
    return "g_" + id + "_" + sign("g:" + id + ":" + manageHash);
}

std::string TelegramBot::waitlistPayload(long long entryId, const std::string& tokenHash) const {
    const std::string id = std::to_string(entryId);
    return "w_" + id + "_" + sign("w:" + id + ":" + tokenHash);
}

bool TelegramBot::parseWaitlist(const std::string& payload, long long& entryId, std::string& signature) {
    entryId = 0;
    signature.clear();
    if (payload.size() > kMaxPayload || payload.rfind("w_", 0) != 0) return false;
    std::size_t pos = 2;
    long long id = 0;
    if (!takeNumber(payload, pos, id) || id <= 0) return false;
    const std::string sig = payload.substr(pos);
    if (!validSignature(sig)) return false;
    entryId = id;
    signature = sig;
    return true;
}

bool TelegramBot::verifyWaitlist(const std::string& payload, long long entryId, const std::string& tokenHash) const {
    long long id = 0;
    std::string sig;
    if (!parseWaitlist(payload, id, sig) || id != entryId || tokenHash.empty()) return false;
    return crypto::constantTimeEquals(sig, sign("w:" + std::to_string(id) + ":" + tokenHash));
}

bool TelegramBot::verifyOwner(const std::string& payload, std::time_t now, long long& userId) const {
    userId = 0;
    if (payload.size() > kMaxPayload || payload.rfind("o_", 0) != 0) return false;
    std::size_t pos = 2;
    long long id = 0;
    long long expires = 0;
    if (!takeNumber(payload, pos, id) || !takeNumber(payload, pos, expires) || id <= 0) return false;
    const std::string sig = payload.substr(pos);
    if (!validSignature(sig)) return false;
    if (!crypto::constantTimeEquals(sig, sign("o:" + std::to_string(id) + ":" + std::to_string(expires)))) {
        return false;
    }
    // Срок: не истёк и не дальше, чем выдаёт сервер (с запасом на расхождение часов).
    if (expires < static_cast<long long>(now) || expires > static_cast<long long>(now) + kOwnerLinkTtlSec + 120) {
        return false;
    }
    userId = id;
    return true;
}

bool TelegramBot::parseGuest(const std::string& payload, long long& bookingId, std::string& signature) {
    bookingId = 0;
    signature.clear();
    if (payload.size() > kMaxPayload || payload.rfind("g_", 0) != 0) return false;
    std::size_t pos = 2;
    long long id = 0;
    if (!takeNumber(payload, pos, id) || id <= 0) return false;
    const std::string sig = payload.substr(pos);
    if (!validSignature(sig)) return false;
    bookingId = id;
    signature = sig;
    return true;
}

bool TelegramBot::verifyGuest(const std::string& payload, long long bookingId, const std::string& manageHash) const {
    long long id = 0;
    std::string sig;
    if (!parseGuest(payload, id, sig) || id != bookingId || manageHash.empty()) return false;
    return crypto::constantTimeEquals(sig, sign("g:" + std::to_string(id) + ":" + manageHash));
}

std::string TelegramBot::linkUrl(const std::string& payload) const {
    return "https://t.me/" + config_.telegramBotUsername + "?start=" + payload;
}

std::string TelegramBot::guestUrl(const BookingRecord& booking) const {
    if (!enabled() || booking.id <= 0 || booking.status != "active" || booking.manageHash.empty()) return "";
    return linkUrl(guestPayload(booking.id, booking.manageHash));
}

// ------------------------------------------------------------ протокол владельца

Json TelegramBot::status(long long userId) {
    Json json = Json::object();
    const auto user = database_.db().findUserById(userId);
    json.set("enabled", Json(enabled()));
    json.set("bot_username", Json(username()));
    json.set("linked", Json(user && user->telegramChatId != 0));
    json.set("username", Json(user && user->telegramChatId != 0 ? user->telegramUsername : std::string()));
    return json;
}

bool TelegramBot::link(long long userId, Json& out, std::string& error) {
    if (!enabled()) {
        error = "Telegram-бот не настроен на сервере";
        return false;
    }
    const std::time_t expires = std::time(nullptr) + kOwnerLinkTtlSec;
    out = Json::object();
    out.set("url", Json(linkUrl(ownerPayload(userId, expires))));
    out.set("expires_at", Json(isoUtc(expires)));
    return true;
}

Json TelegramBot::unlink(long long userId) {
    const DatabaseError cleared = database_.db().setUserTelegram(userId, 0, "");
    if (!cleared.ok) SELFVOY_LOG(log::Level::Warn, "telegram") << "отвязка Telegram: " << cleared.message;
    Json json = Json::object();
    json.set("linked", Json(false));
    return json;
}

// ----------------------------------------------------------------- апдейты

void TelegramBot::handleUpdate(const Json& update) {
    if (!update.isObject()) return;
    if (update.get("message").isObject()) {
        handleMessage(update.get("message"));
    } else if (update.get("callback_query").isObject()) {
        handleCallback(update.get("callback_query"));
    }
}

void TelegramBot::handleMessage(const Json& message) {
    const Json chat = message.get("chat");
    // Только личные чаты: в группе ссылку и напоминания увидели бы посторонние.
    if (chat.getString("type") != "private") return;
    const long long chatId = chat.getInt("id");
    if (chatId == 0) return;
    const std::string text = message.getString("text");
    std::string command;
    std::string argument;
    splitCommand(text, command, argument);
    if (command == "/start") {
        if (argument.rfind("o_", 0) == 0) {
            linkOwner(chatId, message.get("from").getString("username"), argument);
        } else if (argument.rfind("g_", 0) == 0) {
            linkGuest(chatId, argument);
        } else if (argument.rfind("w_", 0) == 0) {
            linkWaitlist(chatId, argument);
        } else if (argument.rfind("p_", 0) == 0 && admin_) {
            admin_->start(chatId, message.get("from"), argument);  // F7: ИИ-администратор страницы
        } else {
            reply(chatId, kHelp);
        }
        return;
    }
    if (command == "/stop") {
        const DatabaseError cleared = database_.db().unlinkTelegramChat(chatId);
        if (!cleared.ok) SELFVOY_LOG(log::Level::Warn, "telegram") << "/stop: " << cleared.message;
        reply(chatId, "Готово: больше не пишу сюда — ни уведомлений, ни напоминаний. "
                      "Подключить снова можно по ссылке из Selfvoy.");
        return;
    }
    // F7: чат, привязанный к странице, разговаривает с ИИ-администратором.
    if (command.rfind("/", 0) != 0 && admin_ && admin_->handleText(chatId, message.get("from"), text)) return;
    reply(chatId, kHelp);
}

void TelegramBot::linkOwner(long long chatId, const std::string& username, const std::string& payload) {
    long long userId = 0;
    if (!verifyOwner(payload, std::time(nullptr), userId)) {
        reply(chatId, kInvalidLink);
        return;
    }
    const auto user = database_.db().findUserById(userId);
    if (!user || !user->active) {
        reply(chatId, kInvalidLink);
        return;
    }
    std::string handle = username;
    if (handle.size() > 64 || !std::all_of(handle.begin(), handle.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_';
        })) {
        handle.clear();
    }
    const DatabaseError saved = database_.db().setUserTelegram(userId, chatId, handle);
    if (!saved.ok) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "привязка владельца не сохранена: " << saved.message;
        reply(chatId, "Не получилось подключить — попробуйте ещё раз чуть позже.");
        return;
    }
    SELFVOY_LOG(log::Level::Info, "telegram") << "Telegram привязан к пользователю " << userId;
    reply(chatId, "Готово: сюда будут приходить новые записи, переносы и отмены. Отключить — /stop");
}

void TelegramBot::linkGuest(long long chatId, const std::string& payload) {
    long long bookingId = 0;
    std::string sig;
    if (!parseGuest(payload, bookingId, sig)) {
        reply(chatId, kInvalidLink);
        return;
    }
    const auto booking = database_.db().findBooking(bookingId);
    if (!booking || !verifyGuest(payload, booking->id, booking->manageHash)) {
        reply(chatId, kInvalidLink);
        return;
    }
    std::time_t ends = 0;
    const std::string endsAt = bookingEndsAt(booking->occurrences);
    if (booking->status != "active" || endsAt.empty() || !parseIsoUtc(endsAt, ends) || ends <= std::time(nullptr)) {
        reply(chatId, "У этой записи нет предстоящих встреч — напоминать не о чем.");
        return;
    }
    if (!database_.db().linkBookingTelegram(booking->id, chatId, kMaxBookingChats)) {
        reply(chatId, "К этой записи уже подключено 3 чата. Отключите лишний командой /stop в нём.");
        return;
    }
    reply(chatId, bookings_ ? bookings_->telegramGreeting(*booking)
                            : std::string("Готово: напоминания о записи будут приходить сюда. Отписаться — /stop"));
}

void TelegramBot::linkWaitlist(long long chatId, const std::string& payload) {
    long long entryId = 0;
    std::string sig;
    if (!parseWaitlist(payload, entryId, sig)) {
        reply(chatId, kInvalidLink);
        return;
    }
    const auto entry = database_.db().findWaitlistEntry(entryId);
    if (!entry || !verifyWaitlist(payload, entry->id, entry->tokenHash)) {
        reply(chatId, kInvalidLink);
        return;
    }
    if (entry->status != "waiting" && entry->status != "offered") {
        reply(chatId, "Эта запись в листе ожидания уже закрыта — предлагать нечего.");
        return;
    }
    const DatabaseError saved = database_.db().setWaitlistTelegram(entry->id, chatId);
    if (!saved.ok) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "подписка листа ожидания не сохранена: " << saved.message;
        reply(chatId, "Не получилось подключить — попробуйте ещё раз чуть позже.");
        return;
    }
    const auto page = database_.db().findBookingPage(entry->pageId);
    reply(chatId, "Готово: как только на «" + (page ? page->title : std::string("Selfvoy")) +
                      "» освободится подходящее время, напишу сюда. Отписаться — /stop");
}

void TelegramBot::handleCallback(const Json& callback) {
    const std::string callbackId = callback.getString("id");
    if (callbackId.empty()) return;
    const Json message = callback.get("message");
    const long long chatId = message.get("chat").getInt("id");
    const long long messageId = message.getInt("message_id");
    const std::string data = callback.getString("data");
    std::string answer = "Кнопка устарела";
    bool done = false;

    // cf:<booking_id>:<start_unix> — «Приду» (F1).
    if (data.rfind("cf:", 0) == 0) {
        const std::size_t colon = data.find(':', 3);
        long long bookingId = 0;
        long long start = 0;
        if (colon != std::string::npos && parseNumber(data.substr(3, colon - 3), bookingId) &&
            parseNumber(data.substr(colon + 1), start) && bookingId > 0) {
            const auto chats = database_.db().bookingTelegramChats(bookingId);
            if (chatId == 0 || std::find(chats.begin(), chats.end(), chatId) == chats.end()) {
                answer = "Эта кнопка — не для этого чата";
            } else if (!bookings_) {
                answer = "Подтверждение сейчас недоступно";
            } else {
                const auto result = bookings_->telegramConfirm(bookingId, static_cast<std::time_t>(start));
                if (result.ok) {
                    answer = "Спасибо, ждём вас";
                    done = true;
                } else {
                    answer = result.message.empty() ? "Подтверждение сейчас недоступно" : result.message;
                }
            }
        }
    } else if (admin_) {
        // F7: кнопки ИИ-администратора (время, «Да, записать», «Нет»).
        bool clear = false;
        if (admin_->handleCallback(chatId, data, answer, clear)) done = clear;
    }

    Json body = Json::object();
    body.set("callback_query_id", Json(callbackId));
    body.set("text", Json(clip(answer, 190)));
    enqueue("answerCallbackQuery", body);
    if (done && chatId != 0 && messageId != 0) {
        Json markup = Json::object();
        markup.set("inline_keyboard", Json::array());
        Json edit = Json::object();
        edit.set("chat_id", Json(chatId));
        edit.set("message_id", Json(messageId));
        edit.set("reply_markup", markup);
        enqueue("editMessageReplyMarkup", edit, chatId);
    }
}

// ------------------------------------------------------------------- опрос

bool TelegramBot::pollOnce(int timeoutSec) {
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    if (!transport) return false;
    long long offset = 0;
    parseNumber(database_.db().getState(kOffsetKey), offset);
    Json body = Json::object();
    if (offset > 0) body.set("offset", Json(offset));
    body.set("timeout", Json(static_cast<long long>(std::max(0, timeoutSec))));
    Json allowed = Json::array();
    allowed.push(Json("message"));
    allowed.push(Json("callback_query"));
    body.set("allowed_updates", allowed);

    std::string error;
    const net::HttpResponse response =
        transport->call("getUpdates", body, (std::max(0, timeoutSec) + 10) * 1000, &stopping_, error);
    if (stopping_.load()) return true;
    if (!response.ok()) {
        // 409 — опрашивает кто-то ещё (другой процесс с тем же токеном или webhook).
        SELFVOY_LOG(log::Level::Warn, "telegram")
            << "getUpdates не удался: " << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
        return false;
    }
    const Json answer = Json::parse(response.body);
    if (!answer.getBool("ok") || !answer.get("result").isArray()) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "getUpdates: некорректный ответ Bot API";
        return false;
    }
    const Json updates = answer.get("result");
    for (const auto& update : updates.items()) {
        const long long id = update.getInt("update_id");
        if (id <= 0 || id < offset) continue;  // уже обработан
        try {
            handleUpdate(update);
        } catch (const std::exception& exception) {
            SELFVOY_LOG(log::Level::Error, "telegram") << "ошибка обработки апдейта: " << exception.what();
        }
        // Смещение — после каждого апдейта: перезапуск не обработает его второй раз.
        offset = id + 1;
        const DatabaseError saved = database_.db().setState(kOffsetKey, std::to_string(offset));
        if (!saved.ok) SELFVOY_LOG(log::Level::Warn, "telegram") << "смещение не сохранено: " << saved.message;
    }
    return true;
}

void TelegramBot::pollLoop() {
    while (!stopping_.load()) {
        if (!database_.db().tryLeaderLock(kLeaderLockKey)) {
            if (leader_) SELFVOY_LOG(log::Level::Warn, "telegram") << "опрос Telegram перешёл другому экземпляру";
            leader_ = false;
            if (waitStop(kLeaderRetrySec)) break;
            continue;
        }
        if (!leader_) {
            leader_ = true;
            SELFVOY_LOG(log::Level::Info, "telegram") << "этот экземпляр опрашивает Telegram (getUpdates)";
        }
        if (!pollOnce(kPollTimeoutSec) && waitStop(kErrorRetrySec)) break;
    }
    if (leader_) database_.db().releaseLeaderLock(kLeaderLockKey);
    leader_ = false;
}

// ----------------------------------------------------------------- отправка

void TelegramBot::sendMessage(long long chatId, const std::string& text, const Json& markup) {
    if (chatId == 0 || text.empty()) return;
    Json body = Json::object();
    body.set("chat_id", Json(chatId));
    body.set("text", Json(clip(text, kMaxText)));
    body.set("disable_web_page_preview", Json(true));
    if (markup.isObject()) body.set("reply_markup", markup);
    enqueue("sendMessage", body, chatId);
}

void TelegramBot::enqueue(const std::string& method, Json body, long long chatId) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    if (queue_.size() >= kMaxQueue) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "очередь отправки переполнена — сообщение (" << method
                                                  << ") отброшено";
        return;
    }
    queue_.push_back(Outgoing{method, std::move(body), chatId});
    if (!sendThread_.joinable()) sendThread_ = std::thread([this] { sendLoop(); });
    cv_.notify_all();
}

void TelegramBot::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return queue_.empty() && !sending_; });
}

void TelegramBot::sendLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
        if (queue_.empty()) return;  // остановка, всё отправлено
        if (stopping_.load() && std::chrono::steady_clock::now() > stopDeadline_) {
            SELFVOY_LOG(log::Level::Warn, "telegram") << "остановка: не отправлено сообщений " << queue_.size();
            queue_.clear();
            cv_.notify_all();
            return;
        }
        Outgoing item = std::move(queue_.front());
        queue_.pop_front();
        sending_ = true;
        lock.unlock();
        try {
            deliver(item);
        } catch (const std::exception& exception) {
            SELFVOY_LOG(log::Level::Error, "telegram") << "ошибка отправки: " << exception.what();
        }
        lock.lock();
        sending_ = false;
        cv_.notify_all();
    }
}

void TelegramBot::deliver(const Outgoing& item) {
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    if (!transport) return;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        const net::HttpResponse response = transport->call(item.method, item.body, kSendTimeoutMs, nullptr, error);
        if (response.ok()) return;
        if (response.status == 403 && item.chatId != 0) {
            // Пользователь заблокировал бота: чат больше не наш — отвязываем везде.
            const DatabaseError cleared = database_.db().unlinkTelegramChat(item.chatId);
            SELFVOY_LOG(log::Level::Info, "telegram")
                << "бот заблокирован в чате — чат отвязан" << (cleared.ok ? "" : " (ошибка: " + cleared.message + ")");
            return;
        }
        if (response.status == 429 && attempt == 0) {
            // Лимит Bot API: ждём retry_after (не дольше 30 с и не дольше остановки).
            const long long wait =
                std::min<long long>(30, std::max<long long>(1, Json::parse(response.body).get("parameters").getInt(
                                                                   "retry_after", 1)));
            for (long long i = 0; i < wait * 5 && !stopping_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (stopping_.load()) return;
            continue;
        }
        SELFVOY_LOG(log::Level::Warn, "telegram")
            << item.method << " не удался: " << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
        return;
    }
}

}  // namespace selfvoy
