// selfvoy/telegramadmin.h — ИИ-администратор страницы записи в Telegram (F7).
//
// Владелец включает на странице поле ai_admin (Pro, нужен настроенный бот); гость
// открывает https://t.me/<бот>?start=p_<slug> — чат привязывается к странице
// (telegram_guest_chats: имя и ник из Telegram, последние ≤ 10 сообщений). Текст
// гостя → AI-сервис POST /v1/agent/admin {page, free_slots (≤ 20 ближайших, как в
// booking.get), history, message} → {reply, action, slots, escalate_reason}.
//
// LLM не источник истины: сервер обрезает и чистит reply (≤ 1000 символов), оставляет
// в slots только времена из free_slots, неизвестный action → none. Запись — только по
// нажатию гостем подписанной кнопки времени и подтверждению «Записать на …?» (второе
// нажатие), через обычный путь booking.book (BookingManager::telegramBook). escalate —
// вопрос специалисту (telegram_questions, уведомление telegram.question), ответ
// владельца telegram.reply бот пересылает гостю «<имя специалиста>: …».
//
// callback_data (≤ 64 байт), подпись — TelegramBot::sign над данными и chat_id:
//   as:<page_id>:<start_unix>:<sig> — выбрано время (sig над "as:<chat>:<page>:<start>");
//   ab:<page_id>:<start_unix>:<sig> — «Да, записать» (sig над "ab:<chat>:<page>:<start>");
//   an                              — «Нет».
// Чужой чат подпись не пройдёт; чат должен быть привязан к этой же странице.
//
// Лимиты: 30 сообщений гостя в сутки на чат, 300 вызовов ИИ в сутки на владельца,
// 5 вопросов специалисту в сутки на чат; превышение или ошибка ИИ — ответ со ссылкой
// на страницу записи. Сообщения длиннее 1000 символов обрезаются. Тексты гостей в лог
// не пишутся. Всё работает в потоке опроса бота (ведущий экземпляр).
#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "selfvoy/json.h"

namespace selfvoy {

class BookingManager;
class DatabaseManager;
class NotificationsManager;
class TelegramBot;
struct BookingPageRecord;
struct Config;
struct TelegramGuestChatRecord;

class TelegramAdmin {
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

    // Проверенное решение ИИ (sanitize).
    struct Decision {
        std::string reply;
        std::string action = "none";  // none | offer_slots | book | escalate
        std::vector<std::time_t> slots;
        std::string reason;
        bool usable = false;  // false — ответа нет или он пуст: запасной текст со ссылкой
    };

    static constexpr int kChatMessagesPerDay = 30;
    static constexpr int kOwnerAiCallsPerDay = 300;
    static constexpr int kEscalationsPerChatDay = 5;
    static constexpr int kRepliesPerOwnerHour = 120;
    static constexpr std::size_t kMaxMessageChars = 1000;
    static constexpr std::size_t kMaxReplyChars = 1000;
    static constexpr std::size_t kMaxHistory = 10;
    static constexpr std::size_t kMaxFreeSlots = 20;
    static constexpr std::size_t kMaxButtons = 6;
    static constexpr const char* kEscalated = "Передал специалисту, он ответит вам здесь или напишет.";

    TelegramAdmin(DatabaseManager& database, const Config& config, TelegramBot& bot, BookingManager& bookings,
                  NotificationsManager* notifications);

    // --- бот (поток опроса) ---
    // /start p_<slug>: привязать чат к странице. from — объект from апдейта.
    void start(long long chatId, const Json& from, const std::string& payload);
    // Текст гостя; false — чат не привязан к странице (бот ответит справкой).
    bool handleText(long long chatId, const Json& from, const std::string& text);
    // Кнопки as:/ab:/an; false — не наша кнопка. answer — текст answerCallbackQuery,
    // clearKeyboard — убрать клавиатуру сообщения.
    bool handleCallback(long long chatId, const std::string& data, std::string& answer, bool& clearKeyboard);

    // --- протокол владельца ---
    // telegram.questions {status?: open|answered, limit? (1–200, 50)} → {questions}.
    Result questions(long long ownerId, const Json& payload);
    // telegram.reply {question_id, text (1–1000)} → {question}.
    Result reply(long long ownerId, const Json& payload);

    // Ответ ИИ → проверенное решение: reply без управляющих символов и ≤ 1000 символов,
    // slots — только из free (без повторов, ≤ 6), неизвестный action → none; offer_slots/book
    // без времени → none.
    static Decision sanitize(const Json& answer, const std::vector<std::time_t>& free);
    // Подписанные callback_data (as / ab).
    std::string callbackData(const std::string& kind, long long chatId, long long pageId, std::time_t start) const;
    // Тесты: другие суточные лимиты.
    void setDailyLimits(int perChat, int perOwner) {
        chatPerDay_ = perChat;
        ownerPerDay_ = perOwner;
    }

private:
    // ИИ-администратор страницы работает сейчас: страница открыта, ai_admin, бот, Pro,
    // владелец активен и подтвердил email.
    bool serves(const BookingPageRecord& page) const;
    // «Сейчас не могу ответить… Записаться можно на странице: <url>» (+ url-кнопка).
    void sendFallback(long long chatId, const BookingPageRecord* page);
    // Запрос к AI-сервису; false — сервис недоступен или ответ не JSON-объект.
    bool askAi(const Json& request, Json& answer) const;
    void escalate(TelegramGuestChatRecord& chat, const BookingPageRecord& page, const std::string& text,
                  std::string& replyText);
    Json pageContext(const BookingPageRecord& page) const;
    static void remember(TelegramGuestChatRecord& chat, const std::string& role, const std::string& text);
    bool limited(const std::string& key, int perWindow, int windowSeconds);
    std::string ownerName(long long ownerId) const;

    DatabaseManager& database_;
    const Config& config_;
    TelegramBot& bot_;
    BookingManager& bookings_;
    NotificationsManager* notifications_;
    int chatPerDay_ = kChatMessagesPerDay;
    int ownerPerDay_ = kOwnerAiCallsPerDay;

    struct Window {
        std::chrono::steady_clock::time_point start;
        int count = 0;
    };
    std::mutex limitsMutex_;
    std::unordered_map<std::string, Window> limits_;
};

}  // namespace selfvoy
