// selfvoy/invitemanager.cpp
#include "selfvoy/invitemanager.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"
#include "selfvoy/referralmanager.h"
#include "selfvoy/schedulelock.h"
#include "selfvoy/session.h"
#include "notificationsmanager.h"

namespace selfvoy {

namespace {

constexpr std::size_t kTokenBytes = 32;    // 256 бит случайности
constexpr std::size_t kTokenLength = 43;   // base64url без '=' от 32 байт
constexpr int kMaxSlots = 6;
constexpr int kReminderLeadSec = 15 * 60;

std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// Длина в символах UTF-8 (продолжающие байты 10xxxxxx не считаются).
std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (const unsigned char ch : value) {
        if ((ch & 0xC0) != 0x80) ++count;
    }
    return count;
}

// Проверка текстового поля: длина в символах и управляющие символы
// (переводы строк допустимы только в многострочных полях).
bool validText(const std::string& value, std::size_t maxChars, bool multiline) {
    if (utf8Length(value) > maxChars) return false;
    for (const unsigned char ch : value) {
        if (ch == 0x7F) return false;
        if (ch < 0x20 && !(multiline && (ch == '\n' || ch == '\r' || ch == '\t'))) return false;
    }
    return true;
}

bool plausibleEmail(const std::string& email) {
    if (email.size() < 3 || email.size() > 254) return false;
    const std::size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= email.size()) return false;
    if (email.find('@', at + 1) != std::string::npos) return false;
    for (const unsigned char ch : email) {
        if (ch <= 0x20 || ch == 0x7F || ch == '<' || ch == '>' || ch == '"' || ch == ',' || ch == ';') return false;
    }
    return true;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    std::string url = base;
    if (!url.empty() && url.back() == '/') url.pop_back();
    return url + path;
}

// Интервал слота {start, end}; false — запись повреждена.
bool slotTimes(const Json& slot, std::time_t& start, std::time_t& end) {
    return parseIsoUtc(slot.getString("start"), start) && parseIsoUtc(slot.getString("end"), end) && end > start;
}

Json slotJson(std::time_t start, std::time_t end) {
    Json slot = Json::object();
    slot.set("start", Json(isoUtc(start)));
    slot.set("end", Json(isoUtc(end)));
    return slot;
}

}  // namespace

InviteManager::InviteManager(DatabaseManager& database, MemoryManager& memory, NotificationsManager* notifications,
                             const Config& config, ReferralManager* referrals, ScheduleLock* scheduleLock)
    : database_(database),
      memory_(memory),
      notifications_(notifications),
      config_(config),
      referrals_(referrals),
      scheduleLock_(scheduleLock) {}

std::string InviteManager::tokenHash(const std::string& token) {
    if (token.size() != kTokenLength) return "";
    for (const unsigned char ch : token) {
        if (std::isalnum(ch) == 0 && ch != '-' && ch != '_') return "";
    }
    return crypto::toHex(crypto::sha256(token));
}

std::string InviteManager::effectiveStatus(const InviteRecord& record, std::time_t now) {
    if (record.status != "open") return record.status;
    std::time_t expires = 0;
    if (!parseIsoUtc(record.expiresAt, expires) || expires <= now) return "expired";
    return "open";
}

Json InviteManager::ownerJson(const InviteRecord& record, std::time_t now) const {
    Json json = Json::object();
    json.set("id", Json(record.id));
    json.set("topic", Json(record.topic));
    json.set("note", Json(record.note));
    json.set("duration_minutes", Json(static_cast<long long>(record.durationMinutes)));
    json.set("status", Json(effectiveStatus(record, now)));
    json.set("slots", record.slots);
    json.set("guest_name", Json(record.guestName));
    json.set("guest_email", Json(record.guestEmail));
    json.set("guest_comment", Json(record.guestComment));
    json.set("chosen", record.chosen);
    json.set("expires_at", Json(record.expiresAt));
    json.set("created_at", Json(record.createdAt));
    json.set("responded_at", Json(record.respondedAt));
    return json;
}

// Вид для гостя: без email владельца и данных других гостей; у открытого
// приглашения — только будущие слоты, всё ещё свободные у владельца.
Json InviteManager::guestJson(const InviteRecord& record, std::time_t now) {
    const std::string status = effectiveStatus(record, now);
    Json json = Json::object();
    json.set("topic", Json(record.topic));
    json.set("note", Json(record.note));
    json.set("duration_minutes", Json(static_cast<long long>(record.durationMinutes)));
    std::string ownerName = "Пользователь Selfvoy";
    std::string ownerTimezone = "UTC";
    if (const auto owner = database_.db().findUserById(record.ownerId)) {
        if (!owner->displayName.empty()) ownerName = owner->displayName;
        if (!owner->timezone.empty()) ownerTimezone = owner->timezone;
    }
    json.set("owner_name", Json(ownerName));
    json.set("owner_timezone", Json(ownerTimezone));
    json.set("status", Json(status));
    Json slots = Json::array();
    if (status == "open") {
        const Busy busy = memory_.busy(record.ownerId);
        for (const auto& slot : record.slots.items()) {
            std::time_t start = 0;
            std::time_t end = 0;
            if (slotTimes(slot, start, end) && start > now && isFree(busy, start, end)) {
                slots.push(slotJson(start, end));
            }
        }
    }
    json.set("slots", slots);
    json.set("guest_name", Json(record.guestName));
    json.set("chosen", status == "accepted" ? record.chosen : Json::object());
    json.set("expires_at", Json(record.expiresAt));
    json.set("ref", Json(referrals_ ? referrals_->codeFor(record.ownerId) : std::string()));
    return json;
}

bool InviteManager::isFree(const Busy& busy, std::time_t start, std::time_t end) {
    return std::none_of(busy.begin(), busy.end(),
                        [&](const auto& interval) { return start < interval.second && interval.first < end; });
}

bool InviteManager::limited(const std::string& key, int perMinute) {
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitHit(key, 60) > perMinute;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(limitsMutex_);
    if (limits_.size() > 10000) {
        for (auto it = limits_.begin(); it != limits_.end();) {
            it = now - it->second.start > std::chrono::seconds(60) ? limits_.erase(it) : std::next(it);
        }
    }
    auto& window = limits_[key];
    if (window.count == 0 || now - window.start > std::chrono::seconds(60)) {
        window.start = now;
        window.count = 0;
    }
    if (window.count >= perMinute) return true;
    ++window.count;
    return false;
}

bool InviteManager::requestSlots(long long ownerId, const std::string& topic, int durationMinutes, int windowHours,
                                 Json& slots, std::string& error) {
    Json request = Json::object();
    request.set("context", memory_.context(ownerId, topic));
    request.set("duration_minutes", Json(static_cast<long long>(durationMinutes)));
    request.set("window_hours", Json(static_cast<long long>(windowHours)));
    request.set("max_slots", Json(static_cast<long long>(kMaxSlots)));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;
    const net::HttpResponse response = net::httpRequest(
        "POST", joinUrl(config_.aiServiceUrl, "/v1/agent/slots"), headers, request.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        if (error.empty()) error = "HTTP " + std::to_string(response.status);
        return false;
    }
    const Json answer = Json::parse(response.body, &error);
    if (!answer.isObject() || !answer.get("slots").isArray()) {
        error = "некорректный ответ /v1/agent/slots";
        return false;
    }

    // AI-сервис не источник истины: сервер сам проверяет длительность, окно,
    // будущее время и занятость, отбрасывает дубли и пересечения.
    const std::time_t now = std::time(nullptr);
    const std::time_t windowEnd = now + static_cast<std::time_t>(windowHours) * 3600;
    const Busy busy = memory_.busy(ownerId);
    Busy accepted;
    const Json offered = answer.get("slots");  // get() возвращает копию: держим её до конца цикла
    for (const auto& slot : offered.items()) {
        std::time_t start = 0;
        std::time_t end = 0;
        if (!slotTimes(slot, start, end)) continue;
        if (end - start != static_cast<std::time_t>(durationMinutes) * 60) continue;
        if (start <= now || end > windowEnd) continue;
        if (!isFree(busy, start, end) || !isFree(accepted, start, end)) continue;
        accepted.emplace_back(start, end);
        if (static_cast<int>(accepted.size()) >= kMaxSlots) break;
    }
    std::sort(accepted.begin(), accepted.end());
    slots = Json::array();
    for (const auto& [start, end] : accepted) slots.push(slotJson(start, end));
    return true;
}

InviteManager::Result InviteManager::create(long long ownerId, const Json& payload) {
    const std::string topic = trim(payload.getString("topic"));
    if (topic.empty() || !validText(topic, 120, false)) {
        return Result::failure(protocol::code::kBadRequest, "тема — от 1 до 120 символов в одну строку");
    }
    const std::string note = trim(payload.getString("note"));
    if (!validText(note, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "заметка — не больше 500 символов");
    }
    const std::string guestName = trim(payload.getString("guest_name"));
    if (!validText(guestName, 80, false)) {
        return Result::failure(protocol::code::kBadRequest, "имя гостя — не больше 80 символов");
    }
    const long long duration = payload.getInt("duration_minutes", 60);
    if (duration < 15 || duration > 240) {
        return Result::failure(protocol::code::kBadRequest, "длительность — от 15 до 240 минут");
    }
    const long long window = payload.getInt("window_hours", 168);
    if (window < 1 || window > 336) {
        return Result::failure(protocol::code::kBadRequest, "период — от 1 до 336 часов");
    }
    if (database_.db().countOpenInvites(ownerId) >= kMaxOpenInvites) {
        return Result::failure(protocol::code::kForbidden,
                               "слишком много открытых приглашений — отмените ненужные");
    }

    Json slots;
    std::string error;
    if (!requestSlots(ownerId, topic, static_cast<int>(duration), static_cast<int>(window), slots, error)) {
        SELFVOY_LOG(log::Level::Warn, "invite") << "подбор слотов не удался: " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен — не удалось подобрать время");
    }
    if (slots.items().empty()) {
        return Result::failure(protocol::code::kConflict, "нет свободных окон в выбранный период");
    }

    InviteRecord record;
    record.ownerId = ownerId;
    record.topic = topic;
    record.note = note;
    record.guestName = guestName;
    record.durationMinutes = static_cast<int>(duration);
    record.slots = slots;
    // Ссылка живёт, пока не прошло начало последнего предложенного слота.
    record.expiresAt = slots.items().back().getString("start");

    std::string token;
    long long id = 0;
    for (int attempt = 0; attempt < 3 && id == 0; ++attempt) {
        token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
        while (!token.empty() && token.back() == '=') token.pop_back();
        record.tokenHash = tokenHash(token);
        if (record.tokenHash.empty()) continue;
        const DatabaseError saved = database_.db().createInvite(record, id);
        if (!saved.ok) {
            SELFVOY_LOG(log::Level::Warn, "invite") << "приглашение не сохранено: " << saved.message;
            id = 0;
        }
    }
    if (id == 0) return Result::failure(protocol::code::kInternal, "не удалось сохранить приглашение");

    const auto stored = database_.db().findInvite(id);
    Result result;
    result.ok = true;
    result.payload.set("invite", ownerJson(stored ? *stored : record, std::time(nullptr)));
    result.payload.set("token", Json(token));
    result.payload.set("url", Json(config_.publicUrl.empty() ? std::string()
                                                              : joinUrl(config_.publicUrl, "/#/i/" + token)));
    return result;
}

InviteManager::Result InviteManager::list(long long ownerId, const Json& payload) {
    long long limit = payload.getInt("limit", 50);
    limit = std::max(1LL, std::min(100LL, limit));
    const std::time_t now = std::time(nullptr);
    Json invites = Json::array();
    for (const auto& record : database_.db().listInvites(ownerId, static_cast<int>(limit))) {
        invites.push(ownerJson(record, now));
    }
    Result result;
    result.ok = true;
    result.payload.set("invites", invites);
    return result;
}

InviteManager::Result InviteManager::cancel(long long ownerId, const Json& payload) {
    const long long id = payload.getInt("invite_id");
    const auto record = id > 0 ? database_.db().findInvite(id) : std::nullopt;
    if (!record || record->ownerId != ownerId) {
        return Result::failure(protocol::code::kNotFound, "приглашение не найдено");
    }
    if (!database_.db().closeInvite(id, "cancelled", InviteRecord{})) {
        return Result::failure(protocol::code::kConflict, "приглашение уже закрыто");
    }
    const auto updated = database_.db().findInvite(id);
    Result result;
    result.ok = true;
    result.payload.set("invite", ownerJson(updated ? *updated : *record, std::time(nullptr)));
    return result;
}

InviteManager::Result InviteManager::get(const Json& payload, const std::string& remoteAddr) {
    if (limited("invite.get:" + Session::ipOf(remoteAddr), kGetPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto record = hash.empty() ? std::nullopt : database_.db().findInviteByToken(hash);
    if (!record) return Result::failure(protocol::code::kNotFound, "приглашение не найдено");
    Result result;
    result.ok = true;
    result.payload.set("invite", guestJson(*record, std::time(nullptr)));
    return result;
}

InviteManager::Result InviteManager::respond(const Json& payload, const std::string& remoteAddr) {
    if (limited("invite.respond:" + Session::ipOf(remoteAddr), kRespondPerMinute)) {
        return Result::failure(protocol::code::kForbidden, "слишком много попыток, попробуйте через минуту");
    }
    const std::string hash = tokenHash(payload.getString("token"));
    const auto record = hash.empty() ? std::nullopt : database_.db().findInviteByToken(hash);
    if (!record) return Result::failure(protocol::code::kNotFound, "приглашение не найдено");

    const std::string action = payload.getString("action");
    if (action != "accept" && action != "decline") {
        return Result::failure(protocol::code::kBadRequest, "action — accept или decline");
    }
    InviteRecord response;
    response.guestName = trim(payload.getString("guest_name"));
    if (response.guestName.empty() || !validText(response.guestName, 80, false)) {
        return Result::failure(protocol::code::kBadRequest, "укажите имя — до 80 символов");
    }
    response.guestEmail = trim(payload.getString("guest_email"));
    if (!response.guestEmail.empty() && !plausibleEmail(response.guestEmail)) {
        return Result::failure(protocol::code::kBadRequest, "некорректный email");
    }
    response.guestComment = trim(payload.getString("comment"));
    if (!validText(response.guestComment, 500, true)) {
        return Result::failure(protocol::code::kBadRequest, "комментарий — не больше 500 символов");
    }

    // Проверка «свободно» и запись встречи — под блокировкой расписания владельца
    // (общей с booking.*; между экземплярами сервера — через PostgreSQL).
    std::unique_lock<std::mutex> localLock(respondMutex_, std::defer_lock);
    std::optional<ScheduleLock::Guard> scheduleLock;
    if (scheduleLock_) {
        scheduleLock.emplace(scheduleLock_->acquire(record->ownerId));
        if (!scheduleLock->ok()) return Result::failure(protocol::code::kConflict, "сервер занят — попробуйте ещё раз");
    } else {
        localLock.lock();
    }
    const std::time_t now = std::time(nullptr);
    const std::string status = effectiveStatus(*record, now);
    if (status == "expired") return Result::failure(protocol::code::kConflict, "срок приглашения истёк");
    if (status != "open") return Result::failure(protocol::code::kConflict, "приглашение уже закрыто");

    std::time_t start = 0;
    std::time_t end = 0;
    if (action == "accept") {
        std::time_t wanted = 0;
        if (!parseIsoUtc(payload.getString("start"), wanted)) {
            return Result::failure(protocol::code::kBadRequest, "start — время одного из предложенных слотов");
        }
        bool offered = false;
        for (const auto& slot : record->slots.items()) {
            std::time_t slotStart = 0;
            std::time_t slotEnd = 0;
            if (slotTimes(slot, slotStart, slotEnd) && slotStart == wanted) {
                start = slotStart;
                end = slotEnd;
                offered = true;
                break;
            }
        }
        if (!offered) return Result::failure(protocol::code::kBadRequest, "этого времени нет в приглашении");
        if (start <= now || !isFree(memory_.busy(record->ownerId), start, end)) {
            return Result::failure(protocol::code::kConflict, "это время уже занято — выберите другое");
        }
        response.chosen = slotJson(start, end);
    }

    if (!database_.db().closeInvite(record->id, action == "accept" ? "accepted" : "declined", response)) {
        return Result::failure(protocol::code::kConflict, "приглашение уже закрыто");
    }

    const std::string title = "Встреча: " + record->topic + " — " + response.guestName;
    if (action == "accept") {
        // Интервал «(start/end)» — встреча занимает время в календаре для
        // переговоров и других приглашений (MemoryManager::busy).
        const DatabaseError remembered = database_.db().upsertMemory(
            record->ownerId, "schedule", title + " (" + isoUtc(start) + "/" + isoUtc(end) + ")", 1.5,
            {"meeting", "invite"});
        if (!remembered.ok) {
            SELFVOY_LOG(log::Level::Warn, "invite") << "встреча не записана в расписание: " << remembered.message;
        }
    }
    // Время занято и записано — расписание владельца больше не держим: задача,
    // уведомление и пуш (сетевые вызовы) идут без блокировки.
    if (scheduleLock) scheduleLock->release();
    if (localLock.owns_lock()) localLock.unlock();

    if (action == "accept") {
        TaskRecord task;
        task.userId = record->ownerId;
        task.title = title;
        std::string notes;
        if (!response.guestEmail.empty()) notes += "Email гостя: " + response.guestEmail;
        if (!response.guestComment.empty()) notes += (notes.empty() ? "" : "\n") + response.guestComment;
        task.notes = notes;
        task.dueAt = isoUtc(start);
        task.remindAt = isoUtc(std::max(now, start - kReminderLeadSec));
        long long taskId = 0;
        const DatabaseError taskSaved = database_.db().createTask(task, taskId);
        if (!taskSaved.ok) {
            SELFVOY_LOG(log::Level::Warn, "invite") << "задача встречи не создана: " << taskSaved.message;
        }
        if (notifications_) {
            Json notify = Json::object();
            notify.set("invite_id", Json(record->id));
            notify.set("start", Json(isoUtc(start)));
            notify.set("end", Json(isoUtc(end)));
            notify.set("guest_name", Json(response.guestName));
            if (taskId > 0) notify.set("task_id", Json(taskId));
            notifications_->create(record->ownerId, "invite.accepted", response.guestName + " выбрал(а) время",
                                   record->topic, notify);
        }
    } else if (notifications_) {
        Json notify = Json::object();
        notify.set("invite_id", Json(record->id));
        notify.set("guest_name", Json(response.guestName));
        notify.set("comment", Json(response.guestComment));
        notifications_->create(record->ownerId, "invite.declined", response.guestName + " не сможет",
                               record->topic, notify);
    }

    const auto updated = database_.db().findInvite(record->id);
    Result result;
    result.ok = true;
    result.payload.set("invite", guestJson(updated ? *updated : *record, std::time(nullptr)));
    return result;
}

}  // namespace selfvoy
