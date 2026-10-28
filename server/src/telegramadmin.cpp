// selfvoy/telegramadmin.cpp — ИИ-администратор страницы записи в Telegram (F7).
#include "selfvoy/telegramadmin.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc, parseIsoUtc
#include "selfvoy/net.h"
#include "selfvoy/plan.h"
#include "selfvoy/protocol.h"
#include "selfvoy/slotspost.h"
#include "selfvoy/telegrambot.h"
#include "bookingutil.h"
#include "notificationsmanager.h"

namespace selfvoy {

using namespace bookingutil;

namespace {

constexpr std::size_t kSignatureLength = 22;
constexpr std::size_t kMaxSlotsToCheck = 200;  // подтверждение: время ищется среди всех свободных
const char* const kDefaultGuestName = "Гость из Telegram";

// Только цифры, ≤ 18 знаков, без ведущих нулей.
bool parseNumber(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) return false;
    if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) return false;
    if (text.size() > 1 && text[0] == '0') return false;
    out = std::atoll(text.c_str());
    return true;
}

// Текст без управляющих символов: \n оставляем (multiline), \r и \t → пробел.
std::string cleanText(const std::string& text, bool multiline) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        if (ch == '\n' && multiline) {
            out.push_back('\n');
        } else if (ch == '\n' || ch == '\r' || ch == '\t') {
            out.push_back(' ');
        } else if (ch >= 0x20 && ch != 0x7F) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return trim(out);
}

// Имя гостя из Telegram: first_name + last_name, одна строка, ≤ 80 символов.
std::string guestNameOf(const Json& from) {
    std::string name = cleanText(from.getString("first_name"), false);
    const std::string last = cleanText(from.getString("last_name"), false);
    if (!last.empty()) name += (name.empty() ? "" : " ") + last;
    name = trim(utf8Prefix(name, 80));
    return name.empty() ? std::string(kDefaultGuestName) : name;
}

// Ник без @: [A-Za-z0-9_], ≤ 64; иначе "".
std::string usernameOf(const Json& from) {
    const std::string handle = from.getString("username");
    if (handle.empty() || handle.size() > 64 ||
        !std::all_of(handle.begin(), handle.end(), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; })) {
        return "";
    }
    return handle;
}

Json button(const std::string& text, const std::string& key, const std::string& value) {
    Json json = Json::object();
    json.set("text", Json(text));
    json.set(key, Json(value));
    return json;
}

Json keyboardOf(const Json& rows) {
    Json markup = Json::object();
    markup.set("inline_keyboard", rows);
    return markup;
}

Json single(const Json& item) {
    Json row = Json::array();
    row.push(item);
    return row;
}

}  // namespace

TelegramAdmin::TelegramAdmin(DatabaseManager& database, const Config& config, TelegramBot& bot,
                             BookingManager& bookings, NotificationsManager* notifications)
    : database_(database), config_(config), bot_(bot), bookings_(bookings), notifications_(notifications) {}

bool TelegramAdmin::limited(const std::string& key, int perWindow, int windowSeconds) {
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitHit(key, windowSeconds) > perWindow;
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(windowSeconds);
    std::lock_guard<std::mutex> lock(limitsMutex_);
    if (limits_.size() > 10000) {
        for (auto it = limits_.begin(); it != limits_.end();) {
            it = now - it->second.start > std::chrono::hours(25) ? limits_.erase(it) : std::next(it);
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

std::string TelegramAdmin::ownerName(long long ownerId) const {
    const auto owner = database_.db().findUserById(ownerId);
    return owner && !owner->displayName.empty() ? owner->displayName : std::string("Специалист");
}

bool TelegramAdmin::serves(const BookingPageRecord& page) const {
    if (!page.aiAdmin || page.status != "active" || !bot_.enabled()) return false;
    const auto owner = database_.db().findUserById(page.ownerId);
    if (!owner || !owner->active || !owner->emailVerified) return false;
    return plan::proAllowed(database_.db(), page.ownerId);
}

std::string TelegramAdmin::callbackData(const std::string& kind, long long chatId, long long pageId,
                                        std::time_t start) const {
    const std::string page = std::to_string(pageId);
    const std::string at = std::to_string(static_cast<long long>(start));
    return kind + ":" + page + ":" + at + ":" + bot_.sign(kind + ":" + std::to_string(chatId) + ":" + page + ":" + at);
}

void TelegramAdmin::remember(TelegramGuestChatRecord& chat, const std::string& role, const std::string& text) {
    Json item = Json::object();
    item.set("role", Json(role));
    item.set("text", Json(utf8Prefix(text, kMaxMessageChars)));
    Json history = Json::array();
    const auto& previous = chat.history.isArray() ? chat.history.items() : Json::Array{};
    const std::size_t keep = kMaxHistory - 1;
    const std::size_t from = previous.size() > keep ? previous.size() - keep : 0;
    for (std::size_t i = from; i < previous.size(); ++i) history.push(previous[i]);
    history.push(item);
    chat.history = history;
}

void TelegramAdmin::sendFallback(long long chatId, const BookingPageRecord* page) {
    const std::string link = page ? bookings_.pageUrl(*page) : std::string();
    std::string text = "Сейчас не получается ответить автоматически.";
    text += link.empty() ? " Попробуйте написать чуть позже." : " Записаться можно на странице: " + link;
    Json markup;
    if (link.rfind("https://", 0) == 0) {
        Json rows = Json::array();
        rows.push(single(button("Открыть страницу записи", "url", link)));
        markup = keyboardOf(rows);
    }
    bot_.sendMessage(chatId, text, markup);
}

// ------------------------------------------------------------------- /start p_

void TelegramAdmin::start(long long chatId, const Json& from, const std::string& payload) {
    const std::string slug = payload.size() > 2 ? payload.substr(2) : std::string();
    const auto page = BookingManager::validSlug(slug) ? database_.db().findBookingPageBySlug(slug) : std::nullopt;
    const auto owner = page ? database_.db().findUserById(page->ownerId) : std::nullopt;
    if (!page || page->status == "archived" || !owner || !owner->active || !owner->emailVerified) {
        bot_.sendMessage(chatId, "Страница записи не найдена — проверьте ссылку.");
        return;
    }
    const std::string link = bookings_.pageUrl(*page);
    if (page->status != "active") {
        bot_.sendMessage(chatId, "Запись на странице «" + page->title + "» сейчас закрыта — загляните позже.");
        return;
    }
    if (!serves(*page)) {
        bot_.sendMessage(chatId, "У страницы «" + page->title + "» нет ИИ-администратора в Telegram." +
                                     (link.empty() ? std::string() : " Записаться можно на странице: " + link));
        return;
    }
    TelegramGuestChatRecord chat;
    chat.chatId = chatId;
    chat.pageId = page->id;
    chat.ownerId = page->ownerId;
    chat.guestName = guestNameOf(from);
    chat.username = usernameOf(from);
    chat.history = Json::array();  // новая привязка — новый разговор
    const DatabaseError saved = database_.db().saveTelegramGuestChat(chat);
    if (!saved.ok) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "чат гостя не привязан к странице " << page->id << ": "
                                                  << saved.message;
        bot_.sendMessage(chatId, "Не получилось подключиться — попробуйте ещё раз чуть позже.");
        return;
    }
    SELFVOY_LOG(log::Level::Info, "telegram") << "чат гостя привязан к странице " << page->id
                                              << " (ИИ-администратор)";
    bot_.sendMessage(chatId, "Здравствуйте! Я ИИ-администратор «" + page->title + "» (" + ownerName(page->ownerId) +
                                 "). Подскажу цену и условия, покажу свободное время и запишу вас. Вопрос, на "
                                 "который не отвечу сам, передам специалисту.\nПросто напишите, что вас интересует. "
                                 "Отключить — /stop");
}

// ------------------------------------------------------------------ текст гостя

TelegramAdmin::Decision TelegramAdmin::sanitize(const Json& answer, const std::vector<std::time_t>& free) {
    Decision decision;
    if (!answer.isObject()) return decision;
    decision.reply = utf8Prefix(cleanText(answer.get("reply").isString() ? answer.getString("reply") : "", true),
                                kMaxReplyChars);
    const std::string action = answer.get("action").isString() ? answer.getString("action") : std::string();
    decision.action = (action == "offer_slots" || action == "book" || action == "escalate") ? action : "none";
    const std::set<std::time_t> allowed(free.begin(), free.end());
    const Json slots = answer.get("slots");
    if (slots.isArray()) {
        for (const auto& item : slots.items()) {
            std::time_t start = 0;
            if (!item.isString() || !parseIsoUtc(item.asString(), start) || allowed.count(start) == 0) continue;
            if (std::find(decision.slots.begin(), decision.slots.end(), start) != decision.slots.end()) continue;
            decision.slots.push_back(start);
            if (decision.slots.size() >= kMaxButtons) break;
        }
    }
    if (decision.action != "offer_slots" && decision.action != "book") decision.slots.clear();
    if ((decision.action == "offer_slots" || decision.action == "book") && decision.slots.empty()) {
        decision.action = "none";
    }
    if (decision.action == "escalate" && answer.get("escalate_reason").isString()) {
        decision.reason = utf8Prefix(cleanText(answer.getString("escalate_reason"), false), 200);
    }
    if (decision.reply.empty() && decision.action == "offer_slots") decision.reply = "Выберите удобное время:";
    // Пустой ответ без действия — как сбой ИИ (гостю — ссылка на страницу).
    decision.usable = !decision.reply.empty() || decision.action == "escalate";
    return decision;
}

Json TelegramAdmin::pageContext(const BookingPageRecord& page) const {
    const auto owner = database_.db().findUserById(page.ownerId);
    Json json = Json::object();
    json.set("title", Json(page.title));
    json.set("description", Json(page.description));
    json.set("duration_minutes", Json(static_cast<long long>(page.durationMinutes)));
    json.set("price", Json(page.price));
    json.set("currency", Json(page.currency));
    json.set("prepay", Json(page.prepay));
    json.set("deposit", Json(page.deposit));
    json.set("policy_text", Json(page.policyText));
    json.set("cancel_hours", Json(static_cast<long long>(page.cancelHours)));
    json.set("timezone", Json(owner && !owner->timezone.empty() ? owner->timezone : std::string("UTC")));
    json.set("owner_name", Json(ownerName(page.ownerId)));
    // Готовые подписи сумм — как в посте «свободные окна» и письмах.
    json.set("price_text", Json(slotspost::moneyLabel(page.price, page.currency)));
    json.set("deposit_text", Json(slotspost::moneyLabel(page.deposit, page.currency)));
    return json;
}

bool TelegramAdmin::askAi(const Json& request, Json& answer) const {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;
    std::string url = config_.aiServiceUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", url + "/v1/agent/admin", headers, request.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Warn, "telegram")
            << "ИИ-администратор: AI-сервис не ответил (" << (error.empty() ? "HTTP " + std::to_string(response.status) : error)
            << ")";
        return false;
    }
    answer = Json::parse(response.body);
    if (!answer.isObject()) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "ИИ-администратор: некорректный ответ /v1/agent/admin";
        return false;
    }
    return true;
}

void TelegramAdmin::escalate(TelegramGuestChatRecord& chat, const BookingPageRecord& page, const std::string& text,
                             std::string& replyText) {
    if (limited("tg.admin.esc:" + std::to_string(chat.chatId), kEscalationsPerChatDay, 86400)) {
        replyText = "Я уже передал ваши вопросы специалисту — он ответит вам здесь или напишет.";
        return;
    }
    TelegramQuestionRecord question;
    question.ownerId = page.ownerId;
    question.pageId = page.id;
    question.chatId = chat.chatId;
    question.guestName = chat.guestName;
    question.username = chat.username;
    question.text = text;
    long long id = 0;
    const DatabaseError saved = database_.db().createTelegramQuestion(question, id);
    if (!saved.ok || id <= 0) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "вопрос гостя не сохранён: " << saved.message;
        replyText = "Не получилось передать вопрос — попробуйте ещё раз чуть позже.";
        return;
    }
    if (notifications_) {
        Json payload = Json::object();
        payload.set("question_id", Json(id));
        payload.set("page_id", Json(page.id));
        payload.set("guest_name", Json(chat.guestName));
        payload.set("telegram_username", Json(chat.username));
        payload.set("text", Json(text));
        notifications_->create(page.ownerId, "telegram.question", "Вопрос из Telegram: " + chat.guestName,
                               page.title + ": " + utf8Prefix(text, 200), payload);
    }
    replyText = kEscalated;
}

bool TelegramAdmin::handleText(long long chatId, const Json& from, const std::string& text) {
    auto chat = database_.db().findTelegramGuestChat(chatId);
    if (!chat) return false;
    chat->guestName = guestNameOf(from);
    chat->username = usernameOf(from);
    const auto page = database_.db().findBookingPage(chat->pageId);
    if (!page || !serves(*page)) {
        sendFallback(chatId, page ? &*page : nullptr);
        return true;
    }
    const std::string message = utf8Prefix(cleanText(text, true), kMaxMessageChars);
    if (message.empty()) {
        bot_.sendMessage(chatId, "Напишите, пожалуйста, вопрос текстом.");
        return true;
    }
    // Лимиты: сообщения чата в сутки, затем вызовы ИИ владельца в сутки.
    if (limited("tg.admin.chat:" + std::to_string(chatId), chatPerDay_, 86400) ||
        limited("tg.admin.owner:" + std::to_string(page->ownerId), ownerPerDay_, 86400)) {
        SELFVOY_LOG(log::Level::Info, "telegram") << "ИИ-администратор: лимит сообщений (страница " << page->id << ")";
        sendFallback(chatId, &*page);
        return true;
    }

    std::vector<BookingManager::FreeSlot> free;
    if (!bookings_.freeSlots(*page, kMaxFreeSlots, free)) free.clear();
    std::vector<std::time_t> starts;
    Json freeJson = Json::array();
    for (const auto& slot : free) {
        starts.push_back(slot.start);
        freeJson.push(Json(isoUtc(slot.start)));
    }
    Json request = Json::object();
    request.set("page", pageContext(*page));
    request.set("free_slots", freeJson);
    request.set("history", chat->history.isArray() ? chat->history : Json::array());
    request.set("message", Json(message));

    Json answer;
    const Decision decision = askAi(request, answer) ? sanitize(answer, starts) : Decision{};
    remember(*chat, "guest", message);
    if (!decision.usable) {
        sendFallback(chatId, &*page);
        remember(*chat, "admin", "(не удалось ответить автоматически)");
        database_.db().saveTelegramGuestChat(*chat);
        return true;
    }

    std::string replyText = decision.reply;
    Json markup;
    if (decision.action == "escalate") {
        escalate(*chat, *page, message, replyText);
    } else if (!decision.slots.empty()) {
        Json rows = Json::array();
        for (const std::time_t start : decision.slots) {
            const auto slot = std::find_if(free.begin(), free.end(),
                                           [start](const BookingManager::FreeSlot& item) { return item.start == start; });
            if (slot == free.end()) continue;
            rows.push(single(button(slot->label, "callback_data", callbackData("as", chatId, page->id, start))));
        }
        markup = keyboardOf(rows);
    }
    bot_.sendMessage(chatId, replyText, markup);
    remember(*chat, "admin", replyText);
    const DatabaseError saved = database_.db().saveTelegramGuestChat(*chat);
    if (!saved.ok) SELFVOY_LOG(log::Level::Warn, "telegram") << "история чата гостя не сохранена: " << saved.message;
    return true;
}

// ---------------------------------------------------------------------- кнопки

bool TelegramAdmin::handleCallback(long long chatId, const std::string& data, std::string& answer,
                                   bool& clearKeyboard) {
    clearKeyboard = false;
    if (data == "an") {
        answer = "Хорошо, не записываю";
        clearKeyboard = true;
        return true;
    }
    if (data.rfind("as:", 0) != 0 && data.rfind("ab:", 0) != 0) return false;
    answer = "Кнопка устарела";
    const std::string kind = data.substr(0, 2);
    // <kind>:<page>:<start>:<sig>
    const std::size_t first = data.find(':', 3);
    const std::size_t second = first == std::string::npos ? std::string::npos : data.find(':', first + 1);
    long long pageId = 0;
    long long start = 0;
    if (second == std::string::npos || !parseNumber(data.substr(3, first - 3), pageId) ||
        !parseNumber(data.substr(first + 1, second - first - 1), start) || pageId <= 0 || chatId == 0) {
        return true;
    }
    const std::string sig = data.substr(second + 1);
    const std::string expected =
        bot_.sign(kind + ":" + std::to_string(chatId) + ":" + std::to_string(pageId) + ":" + std::to_string(start));
    if (sig.size() != kSignatureLength || !crypto::constantTimeEquals(sig, expected)) {
        answer = "Эта кнопка — не для этого чата";
        return true;
    }
    auto chat = database_.db().findTelegramGuestChat(chatId);
    if (!chat || chat->pageId != pageId) return true;  // чат перепривязан или /stop
    const auto page = database_.db().findBookingPage(pageId);
    if (!page || !serves(*page)) {
        answer = "Запись через бота сейчас недоступна";
        return true;
    }
    std::vector<BookingManager::FreeSlot> free;
    if (!bookings_.freeSlots(*page, kMaxSlotsToCheck, free)) {
        answer = "Не удалось проверить свободное время — попробуйте позже";
        return true;
    }
    const auto slot = std::find_if(free.begin(), free.end(), [start](const BookingManager::FreeSlot& item) {
        return item.start == static_cast<std::time_t>(start);
    });
    if (slot == free.end()) {
        answer = "Это время уже заняли — выберите другое";
        return true;
    }

    if (kind == "as") {
        // Первое нажатие: только вопрос «Записать на …?» — запись по второму.
        std::string text = "Записать вас на " + slot->when + "?\n«" + page->title + "» — " + ownerName(page->ownerId) +
                           "\nИмя для записи: " + chat->guestName;
        const std::string price = slotspost::moneyLabel(page->price, page->currency);
        if (!price.empty()) text += "\nСтоимость: " + price;
        if (page->prepay == "full" && page->price > 0) {
            text += "\nНужна предоплата — ссылку на оплату пришлю после записи.";
        } else if (page->prepay == "deposit" && page->deposit > 0) {
            text += "\nНужен депозит " + slotspost::moneyLabel(page->deposit, page->currency) +
                    " — ссылку на оплату пришлю после записи.";
        }
        Json rows = Json::array();
        rows.push(single(button("Да, записать", "callback_data", callbackData("ab", chatId, pageId, slot->start))));
        rows.push(single(button("Нет", "callback_data", "an")));
        bot_.sendMessage(chatId, text, keyboardOf(rows));
        answer = "Подтвердите запись";
        return true;
    }

    // ab: подтверждение — обычный путь booking.book.
    const std::string comment =
        "Запись через Telegram-бота" + (chat->username.empty() ? std::string() : " (@" + chat->username + ")");
    const auto result = bookings_.telegramBook(*page, chatId, slot->start, chat->guestName, comment);
    if (!result.ok) {
        answer = result.code == protocol::code::kConflict ? std::string("Это время уже заняли — выберите другое")
                                                          : (result.message.empty() ? std::string("Не получилось записать")
                                                                                    : result.message);
        return true;
    }
    answer = "Записано";
    clearKeyboard = true;
    std::string text = "Готово: вы записаны на " + slot->when + ".";
    Json markup;
    const Json payment = result.payload.get("booking").get("payment");
    if (payment.isObject() && payment.getString("status") == "pending") {
        const std::string amount = slotspost::moneyLabel(payment.getInt("amount"), payment.getString("currency", "RUB"));
        std::time_t due = 0;
        std::string within;
        if (parseIsoUtc(payment.getString("due_at"), due) && due > std::time(nullptr)) {
            within = " в течение " + std::to_string(std::max<long long>(1, (due - std::time(nullptr)) / 60)) + " мин.";
        }
        text += "\nЧтобы запись сохранилась, внесите предоплату " + amount + (within.empty() ? "." : within);
        std::string payUrl = payment.getString("action_url");
        if (payUrl.empty()) payUrl = page->paymentUrl;
        if (!payUrl.empty()) text += "\nОплатить: " + payUrl;
        if (!page->paymentNote.empty()) text += "\nРеквизиты: " + page->paymentNote;
        if (payment.getString("method") == "link") {
            text += "\nПосле оплаты нажмите «Я оплатил(а)» на странице записи (кнопка «Перенести или отменить»).";
        }
        text += "\nБез оплаты запись отменится автоматически.";
        if (payUrl.rfind("https://", 0) == 0) {
            Json rows = Json::array();
            rows.push(single(button("Оплатить", "url", payUrl)));
            markup = keyboardOf(rows);
        }
    }
    text += "\nНапоминания о встрече придут сюда.";
    bot_.sendMessage(chatId, text, markup);
    remember(*chat, "admin", "Записал(а) гостя на " + slot->when);
    database_.db().saveTelegramGuestChat(*chat);
    return true;
}

// --------------------------------------------------------------- протокол владельца

TelegramAdmin::Result TelegramAdmin::questions(long long ownerId, const Json& payload) {
    const std::string status = payload.getString("status");
    if (!status.empty() && status != "open" && status != "answered") {
        return Result::failure(protocol::code::kBadRequest, "status — open или answered");
    }
    long long limit = 50;
    if (!intField(payload, "limit", 1, 200, limit)) {
        return Result::failure(protocol::code::kBadRequest, "limit — от 1 до 200");
    }
    std::map<long long, std::string> titles;
    Json list = Json::array();
    for (const auto& question : database_.db().listTelegramQuestions(ownerId, status, static_cast<int>(limit))) {
        if (titles.find(question.pageId) == titles.end()) {
            const auto page = database_.db().findBookingPage(question.pageId);
            titles[question.pageId] = page ? page->title : std::string();
        }
        const bool reachable = bot_.enabled() && database_.db().findTelegramGuestChat(question.chatId).has_value();
        Json item = Json::object();
        item.set("id", Json(question.id));
        item.set("page_id", Json(question.pageId));
        item.set("page_title", Json(titles[question.pageId]));
        item.set("guest_name", Json(question.guestName));
        item.set("telegram_username", Json(question.username));
        item.set("text", Json(question.text));
        item.set("status", Json(question.status));
        item.set("answer", Json(question.answer));
        item.set("created_at", Json(question.createdAt));
        item.set("answered_at", Json(question.answeredAt));
        item.set("can_reply", Json(reachable));
        list.push(item);
    }
    Result result;
    result.ok = true;
    result.payload.set("questions", list);
    return result;
}

TelegramAdmin::Result TelegramAdmin::reply(long long ownerId, const Json& payload) {
    if (!bot_.enabled()) return Result::failure(protocol::code::kBadRequest, "Telegram-бот не настроен на сервере");
    const long long id = payload.getInt("question_id");
    const auto question = id > 0 ? database_.db().findTelegramQuestion(id) : std::nullopt;
    if (!question || question->ownerId != ownerId) {
        return Result::failure(protocol::code::kNotFound, "вопрос не найден");
    }
    const std::string text = trim(payload.getString("text"));
    if (text.empty() || !validText(text, kMaxReplyChars, true)) {
        return Result::failure(protocol::code::kBadRequest, "ответ — от 1 до 1000 символов");
    }
    auto chat = database_.db().findTelegramGuestChat(question->chatId);
    if (!chat) {
        return Result::failure(protocol::code::kConflict, "гость отключил бота (/stop) — ответить в Telegram нельзя");
    }
    if (limited("tg.admin.reply:" + std::to_string(ownerId), kRepliesPerOwnerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много ответов — попробуйте через час");
    }
    const std::string message = ownerName(ownerId) + ": " + text;
    bot_.sendMessage(question->chatId, message);
    if (!database_.db().answerTelegramQuestion(question->id, text)) {
        SELFVOY_LOG(log::Level::Warn, "telegram") << "ответ на вопрос " << question->id << " не сохранён";
    }
    if (chat->pageId == question->pageId) {
        remember(*chat, "admin", message);
        database_.db().saveTelegramGuestChat(*chat);
    }
    const auto stored = database_.db().findTelegramQuestion(question->id);
    const TelegramQuestionRecord& view = stored ? *stored : *question;
    Json item = Json::object();
    item.set("id", Json(view.id));
    item.set("page_id", Json(view.pageId));
    item.set("guest_name", Json(view.guestName));
    item.set("telegram_username", Json(view.username));
    item.set("text", Json(view.text));
    item.set("status", Json(view.status));
    item.set("answer", Json(view.answer));
    item.set("created_at", Json(view.createdAt));
    item.set("answered_at", Json(view.answeredAt));
    item.set("can_reply", Json(true));
    Result result;
    result.ok = true;
    result.payload.set("question", item);
    return result;
}

}  // namespace selfvoy
