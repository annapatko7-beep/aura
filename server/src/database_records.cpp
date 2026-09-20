// aura/database_records.cpp — сериализация записей и служебные хелперы.
#include <chrono>
#include <cstdio>
#include <ctime>

#include "aura/idatabase.h"

namespace aura {

namespace {

std::string toIso(std::chrono::system_clock::time_point moment) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(moment);
    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(moment.time_since_epoch()).count() % 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[48];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    std::string out = buffer;
    char tail[8];
    std::snprintf(tail, sizeof(tail), ".%03dZ", millis);
    return out + tail;
}

Json tagsToJson(const std::vector<std::string>& tags) {
    Json array = Json::array();
    for (const auto& tag : tags) array.push(Json(tag));
    return array;
}

}  // namespace

std::string isoNow() { return toIso(std::chrono::system_clock::now()); }

Json UserRecord::toJson(bool includeSecret) const {
    Json json = Json::object();
    json.set("id", Json(id));
    json.set("email", Json(email));
    json.set("display_name", Json(displayName));
    json.set("avatar_url", Json(avatarUrl));
    json.set("timezone", Json(timezone));
    json.set("is_active", Json(active));
    json.set("email_verified", Json(emailVerified));
    json.set("created_at", Json(createdAt));
    if (!lastSeenAt.empty()) json.set("last_seen_at", Json(lastSeenAt));
    if (includeSecret) json.set("password_hash", Json(passwordHash));
    return json;
}

Json ChatRecord::toJson() const {
    Json json = Json::object();
    json.set("id", Json(id));
    json.set("kind", Json(kind));
    json.set("title", Json(title));
    json.set("created_at", Json(createdAt));
    if (!lastMessageAt.empty()) json.set("last_message_at", Json(lastMessageAt));
    if (!lastMessageBody.empty()) json.set("last_message_body", Json(lastMessageBody));
    if (lastMessageSenderId != 0) json.set("last_message_sender_id", Json(lastMessageSenderId));
    if (!lastMessageSender.empty()) json.set("last_message_sender", Json(lastMessageSender));
    return json;
}

Json MemberRecord::toJson() const {
    Json json = Json::object();
    json.set("chat_id", Json(chatId));
    json.set("user_id", Json(userId));
    json.set("role", Json(role));
    json.set("display_name", Json(displayName));
    json.set("email", Json(email));
    if (!lastReadAt.empty()) json.set("last_read_at", Json(lastReadAt));
    return json;
}

Json MessageRecord::toJson() const {
    Json json = Json::object();
    json.set("id", Json(id));
    json.set("chat_id", Json(chatId));
    json.set("sender_id", Json(senderId));
    json.set("sender_name", Json(senderName));
    json.set("kind", Json(kind));
    json.set("body", Json(body));
    json.set("payload", payload.isObject() ? payload : Json::object());
    json.set("created_at", Json(createdAt));
    return json;
}

Json MemoryRecord::toJson() const {
    Json json = Json::object();
    json.set("id", Json(id));
    json.set("user_id", Json(userId));
    json.set("kind", Json(kind));
    json.set("text", Json(text));
    json.set("weight", Json(weight));
    json.set("tags", tagsToJson(tags));
    json.set("updated_at", Json(updatedAt));
    return json;
}

}  // namespace aura
