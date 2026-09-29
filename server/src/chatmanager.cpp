// aura/chatmanager.cpp
#include "aura/chatmanager.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include "aura/log.h"
#include "aura/protocol.h"

namespace aura {

namespace {

bool isNumeric(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

// tolower применяется только к ASCII: байты UTF-8 (кириллица) трогаем нельзя.
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch < 128 ? std::tolower(ch) : ch);
    });
    return value;
}

// «Аней» и «Аня» дают один ключ: срезаем типовое окончание.
std::string nameKey(const std::string& name) {
    static const std::vector<std::string> endings = {
        "ией", "ей", "ой", "ий", "ем", "ом", "ам", "ям", "у", "ю", "а", "я", "ы", "и"};
    std::string raw = lower(name);
    for (const auto& ending : endings) {
        if (raw.size() > ending.size() + 1 && raw.compare(raw.size() - ending.size(), ending.size(), ending) == 0) {
            return raw.substr(0, raw.size() - ending.size());
        }
    }
    return raw;
}

}  // namespace

ChatManager::ChatManager(DatabaseManager& database, ConnectionManager& connections)
    : database_(database), connections_(connections) {}

ChatManager::Result ChatManager::list(long long userId, int limit) {
    Result result;
    result.ok = true;
    Json chats = Json::array();
    for (const auto& chat : database_.db().listChats(userId, limit > 0 ? limit : 50)) {
        Json item = chat.toJson();
        Json members = Json::array();
        for (const auto& member : database_.db().listMembers(chat.id)) {
            members.push(member.toJson());
        }
        item.set("members", members);
        chats.push(item);
    }
    result.payload.set("chats", chats);
    return result;
}

ChatManager::Result ChatManager::open(long long userId, const std::string& contact, const std::string& title) {
    if (contact.empty()) return Result::failure(protocol::code::kBadRequest, "не указан собеседник");

    std::optional<UserRecord> peer;
    if (isNumeric(contact)) {
        peer = database_.db().findUserById(std::atoll(contact.c_str()));
    } else {
        peer = database_.db().findUserByEmail(contact);
    }
    if (!peer) {
        // Аура часто передаёт имя так, как оно прозвучало во фразе («с Аней») —
        // ищем по основе имени среди пользователей.
        const std::string key = nameKey(contact);
        if (!key.empty()) {
            // Ищем по основе имени целиком: обрезка по байтам сломала бы UTF-8.
            for (const auto& candidate : database_.db().searchUsers(key, 20)) {
                if (nameKey(candidate.displayName) == key) {
                    peer = candidate;
                    break;
                }
            }
        }
    }
    if (!peer) return Result::failure(protocol::code::kNotFound, "пользователь не найден: " + contact);
    if (peer->id == userId) return Result::failure(protocol::code::kBadRequest, "нельзя создать чат с собой");

    if (const auto existing = database_.db().findDirectChat(userId, peer->id)) {
        Result result;
        result.ok = true;
        const auto chat = database_.db().findChat(*existing);
        result.payload.set("chat", chat ? chat->toJson() : Json::object());
        result.payload.set("created", Json(false));
        return result;
    }

    long long chatId = 0;
    const DatabaseError created = database_.db().createChat(
        "agent", title.empty() ? std::string() : title, userId, {userId, peer->id}, chatId);
    if (!created.ok) return Result::failure(protocol::code::kInternal, created.message);

    const auto chat = database_.db().findChat(chatId);
    Result result;
    result.ok = true;
    result.payload.set("chat", chat ? chat->toJson() : Json::object());
    result.payload.set("created", Json(true));

    Json eventPayload = Json::object();
    eventPayload.set("chat", chat ? chat->toJson() : Json::object());
    eventPayload.set("invited_by", Json(userId));
    connections_.deliverToUser(peer->id, protocol::event("chat.created", eventPayload));
    AURA_LOG(log::Level::Info, "chat") << "создан чат " << chatId << " между " << userId << " и " << peer->id;
    return result;
}

ChatManager::Result ChatManager::history(long long userId, long long chatId, long long beforeId, int limit) {
    if (!database_.db().isMember(chatId, userId)) {
        return Result::failure(protocol::code::kForbidden, "вы не участник этого чата");
    }
    Result result;
    result.ok = true;
    Json messages = Json::array();
    for (const auto& message : database_.db().listMessages(chatId, beforeId, limit > 0 ? limit : 50)) {
        messages.push(message.toJson());
    }
    result.payload.set("chat_id", Json(chatId));
    result.payload.set("messages", messages);
    const auto chat = database_.db().findChat(chatId);
    if (chat) result.payload.set("chat", chat->toJson());
    return result;
}

ChatManager::Result ChatManager::members(long long userId, long long chatId) {
    if (!database_.db().isMember(chatId, userId)) {
        return Result::failure(protocol::code::kForbidden, "вы не участник этого чата");
    }
    Result result;
    result.ok = true;
    Json members = Json::array();
    for (const auto& member : database_.db().listMembers(chatId)) {
        Json item = member.toJson();
        item.set("online", Json(connections_.isOnline(member.userId)));
        members.push(item);
    }
    result.payload.set("members", members);
    return result;
}

ChatManager::Result ChatManager::markRead(long long userId, long long chatId) {
    // Как и в history/members/send: без участия в чате делать здесь нечего,
    // иначе запрос превращается в оракул существования чужих чатов.
    if (!database_.db().isMember(chatId, userId)) {
        return Result::failure(protocol::code::kForbidden, "вы не участник этого чата");
    }
    const DatabaseError marked = database_.db().markRead(chatId, userId);
    if (!marked.ok) return Result::failure(protocol::code::kNotFound, marked.message);
    Result result;
    result.ok = true;
    result.payload.set("chat_id", Json(chatId));
    result.payload.set("read", Json(true));
    return result;
}

ChatManager::Result ChatManager::searchUsers(const std::string& query, int limit) {
    Result result;
    // Пустой или односимвольный запрос позволял выгрузить всю таблицу users
    // вместе с адресами; короткая строка не даёт осмысленного поиска anyway.
    if (query.size() < 2) {
        return Result::failure(protocol::code::kBadRequest, "поисковый запрос короче 2 символов");
    }
    const int capped = limit > 0 ? std::min(limit, 50) : 20;
    result.ok = true;
    Json users = Json::array();
    for (const auto& user : database_.db().searchUsers(query, capped)) {
        Json item = user.toJson();
        item.set("online", Json(connections_.isOnline(user.id)));
        users.push(item);
    }
    result.payload.set("users", users);
    return result;
}

ChatManager::Result ChatManager::send(long long senderId,
                                      long long chatId,
                                      const std::string& body,
                                      const std::string& kind,
                                      const Json& payload) {
    if (body.empty() && (!payload.isObject() || payload.empty())) {
        return Result::failure(protocol::code::kBadRequest, "пустое сообщение");
    }
    const bool system = senderId == 0;
    if (!system && !database_.db().isMember(chatId, senderId)) {
        return Result::failure(protocol::code::kForbidden, "вы не участник этого чата");
    }

    MessageRecord record;
    record.chatId = chatId;
    record.senderId = senderId;
    record.kind = kind.empty() ? "text" : kind;
    record.body = body;
    record.payload = payload.isObject() ? payload : Json::object();
    record.createdAt = isoNow();
    if (const auto sender = database_.db().findUserById(senderId)) record.senderName = sender->displayName;

    long long messageId = 0;
    const DatabaseError inserted = database_.db().insertMessage(record, messageId);
    if (!inserted.ok) return Result::failure(protocol::code::kInternal, inserted.message);
    record.id = messageId;

    Result result;
    result.ok = true;
    result.record = record;
    result.payload = record.toJson();

    // Рассылка участникам (включая другие устройства отправителя).
    std::set<long long> recipients;
    for (const auto& member : database_.db().listMembers(chatId)) recipients.insert(member.userId);
    std::size_t delivered = 0;
    for (const long long recipient : recipients) {
        delivered += connections_.deliverToUser(recipient, protocol::event("chat.message", record.toJson()));
    }
    AURA_LOG(log::Level::Debug, "chat")
        << "сообщение " << messageId << " в чат " << chatId << ", доставлено " << delivered;
    return result;
}

std::vector<MessageRecord> ChatManager::recent(long long chatId, int limit) {
    return database_.db().listMessages(chatId, 0, limit);
}

}  // namespace aura
