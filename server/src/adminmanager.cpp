// selfvoy/adminmanager.cpp — роли, блокировка пользователей, журнал аудита.
#include "selfvoy/adminmanager.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/idatabase.h"
#include "selfvoy/log.h"
#include "selfvoy/plan.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace {

std::string lowerTrim(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

std::size_t utf8Length(const std::string& value) {
    std::size_t length = 0;
    for (unsigned char ch : value) {
        if ((ch & 0xC0) != 0x80) ++length;
    }
    return length;
}

// Живые сессии: не отозваны и не истекли (сравнение ISO-строк по секундам).
long long liveSessions(IDatabase& db, long long userId) {
    const std::string now = isoNow().substr(0, 19);
    long long count = 0;
    for (const auto& session : db.listSessions(userId)) {
        if (!session.revokedAt.empty()) continue;
        if (!session.expiresAt.empty() && session.expiresAt.substr(0, 19) <= now) continue;
        ++count;
    }
    return count;
}

Json auditJson(const AuditRecord& entry, bool withAddress) {
    Json item = Json::object();
    item.set("id", Json(entry.id));
    item.set("user_id", Json(entry.userId));
    item.set("kind", Json(entry.kind));
    item.set("detail", entry.detail.isObject() ? entry.detail : Json::object());
    // IP-адреса — только администраторам; модератору хватает «что и когда».
    item.set("remote_addr", Json(withAddress ? entry.remoteAddr : std::string()));
    item.set("created_at", Json(entry.createdAt));
    return item;
}

}  // namespace

AdminManager::AdminManager(DatabaseManager& database, const Config& config) : database_(database) {
    std::string item;
    for (char ch : config.adminEmails + ",") {
        if (ch == ',' || ch == ';' || ch == ' ') {
            const std::string email = lowerTrim(item);
            if (email.find('@') != std::string::npos) bootstrapEmails_.insert(email);
            item.clear();
        } else {
            item += ch;
        }
    }
}

bool AdminManager::validRole(const std::string& role) {
    return role == "user" || role == "moderator" || role == "admin";
}

int AdminManager::rank(const std::string& role) {
    if (role == "admin") return 2;
    if (role == "moderator") return 1;
    return 0;
}

void AdminManager::bootstrapAll() {
    for (const auto& email : bootstrapEmails_) {
        if (const auto user = database_.db().findUserByEmail(email)) bootstrap(*user, "");
    }
    if (!bootstrapEmails_.empty()) {
        SELFVOY_LOG(log::Level::Info, "admin") << "SELFVOY_ADMIN_EMAILS: адресов " << bootstrapEmails_.size();
    }
}

void AdminManager::bootstrap(const UserRecord& user, const std::string& remoteAddr) {
    // Только подтверждённый email: иначе роль получил бы тот, кто первым
    // зарегистрировал чужой адрес из списка.
    if (user.role == "admin" || !user.emailVerified || !user.active) return;
    if (!bootstrapEmails_.count(lowerTrim(user.email))) return;
    if (database_.db().setUserRole(user.id, "admin").ok) {
        Json detail = Json::object();
        detail.set("role", Json("admin"));
        audit(user.id, "role_bootstrap", detail, remoteAddr);
        SELFVOY_LOG(log::Level::Info, "admin") << user.email << " назначен администратором (SELFVOY_ADMIN_EMAILS)";
    }
}

bool AdminManager::requireRole(long long actorId, const std::string& minRole, UserRecord& actor, Result& failure) {
    const auto found = database_.db().findUserById(actorId);
    if (!found || !found->active || rank(found->role) < rank(minRole)) {
        failure = Result::failure(protocol::code::kForbidden,
                                  minRole == "admin" ? "нужны права администратора" : "нужны права модератора");
        return false;
    }
    actor = *found;
    return true;
}

bool AdminManager::canModerate(const UserRecord& actor, const UserRecord& target, std::string& reason) {
    if (actor.id == target.id) {
        reason = "свою учётную запись менять здесь нельзя";
        return false;
    }
    if (actor.role == "admin") return true;
    if (actor.role == "moderator" && target.role == "user") return true;
    reason = "модератор управляет только обычными пользователями";
    return false;
}

Json AdminManager::adminUserJson(const UserRecord& user) {
    Json json = Json::object();
    json.set("id", Json(user.id));
    json.set("email", Json(user.email));
    json.set("display_name", Json(user.displayName));
    json.set("avatar_url", Json(user.avatarUrl));
    json.set("role", Json(user.role.empty() ? std::string("user") : user.role));
    json.set("is_active", Json(user.active));
    json.set("email_verified", Json(user.emailVerified));
    const auto twoFactor = database_.db().findTwoFactor(user.id);
    json.set("two_factor", Json(twoFactor && twoFactor->enabled));
    json.set("created_at", Json(user.createdAt));
    json.set("last_seen_at", Json(user.lastSeenAt));
    json.set("plan", plan::toJson(user));
    return json;
}

void AdminManager::audit(long long userId, const std::string& kind, const Json& detail, const std::string& remoteAddr) {
    const DatabaseError stored = database_.db().insertAudit(userId, kind, detail, remoteAddr);
    if (!stored.ok) SELFVOY_LOG(log::Level::Warn, "admin") << "audit(" << kind << "): " << stored.message;
}

AdminManager::Result AdminManager::stats(long long actorId, std::size_t onlineNow) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "moderator", actor, result)) return result;
    result.ok = true;
    result.payload = database_.db().adminStats();
    result.payload.set("online_now", Json(static_cast<long long>(onlineNow)));
    return result;
}

AdminManager::Result AdminManager::users(long long actorId, const Json& payload) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "moderator", actor, result)) return result;
    const std::string role = payload.getString("role");
    const std::string status = payload.getString("status");
    if (!role.empty() && !validRole(role)) return Result::failure(protocol::code::kBadRequest, "неизвестная роль");
    if (!status.empty() && status != "active" && status != "disabled" && status != "unverified") {
        return Result::failure(protocol::code::kBadRequest, "статус: active, disabled или unverified");
    }
    const std::string query = payload.getString("query");
    if (utf8Length(query) > 100) return Result::failure(protocol::code::kBadRequest, "слишком длинный запрос");
    const int limit = static_cast<int>(std::clamp<long long>(payload.getInt("limit", 50), 1, 100));
    const int offset = static_cast<int>(std::clamp<long long>(payload.getInt("offset", 0), 0, 1000000));

    long long total = 0;
    Json list = Json::array();
    for (const auto& user : database_.db().adminListUsers(query, role, status, limit, offset, total)) {
        list.push(adminUserJson(user));
    }
    result.ok = true;
    result.payload.set("users", list);
    result.payload.set("total", Json(total));
    return result;
}

AdminManager::Result AdminManager::user(long long actorId, const Json& payload) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "moderator", actor, result)) return result;
    const auto target = database_.db().findUserById(payload.getInt("user_id"));
    if (!target) return Result::failure(protocol::code::kNotFound, "пользователь не найден");

    const long long sessions = liveSessions(database_.db(), target->id);
    // Последние события пользователя (как в журнале, без IP для модератора).
    Json audit = Json::array();
    for (const auto& entry : database_.db().listAuditPage(target->id, 0, 30)) {
        audit.push(auditJson(entry, actor.role == "admin"));
    }
    result.ok = true;
    result.payload.set("user", adminUserJson(*target));
    result.payload.set("sessions_active", Json(sessions));
    result.payload.set("audit", audit);
    return result;
}

AdminManager::Result AdminManager::setActive(long long actorId, const Json& payload, const std::string& remoteAddr) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "moderator", actor, result)) return result;
    const auto target = database_.db().findUserById(payload.getInt("user_id"));
    if (!target) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    const Json activeValue = payload.get("active");
    if (!activeValue.isBool()) return Result::failure(protocol::code::kBadRequest, "нужно поле active (true/false)");
    const bool active = activeValue.asBool();
    const std::string reason = payload.getString("reason");
    if (utf8Length(reason) > 200) return Result::failure(protocol::code::kBadRequest, "причина — до 200 символов");
    if (!active && reason.find_first_not_of(" \t") == std::string::npos) {
        return Result::failure(protocol::code::kBadRequest, "укажите причину блокировки");
    }
    std::string why;
    if (!canModerate(actor, *target, why)) return Result::failure(protocol::code::kForbidden, why);

    const DatabaseError saved = database_.db().setUserActive(target->id, active);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить");
    long long revoked = 0;
    if (!active) {
        // Блокировка = выход везде: сессии, refresh-токены, открытые соединения.
        revoked = liveSessions(database_.db(), target->id);
        database_.db().revokeAllSessions(target->id, "");
        database_.db().revokeAllUserRefreshTokens(target->id);
        result.kick.push_back(target->id);
    }
    Json detail = Json::object();
    detail.set("actor_id", Json(actor.id));
    detail.set("actor_email", Json(actor.email));
    detail.set("reason", Json(reason));
    if (!active) detail.set("revoked", Json(revoked));
    audit(target->id, active ? "admin_user_enabled" : "admin_user_disabled", detail, remoteAddr);
    Json mine = Json::object();
    mine.set("target_id", Json(target->id));
    mine.set("target_email", Json(target->email));
    mine.set("reason", Json(reason));
    audit(actor.id, active ? "admin_user_enabled" : "admin_user_disabled", mine, remoteAddr);

    auto updated = database_.db().findUserById(target->id);
    result.ok = true;
    result.payload.set("user", adminUserJson(updated ? *updated : *target));
    return result;
}

AdminManager::Result AdminManager::setRole(long long actorId, const Json& payload, const std::string& remoteAddr) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "admin", actor, result)) return result;
    const auto target = database_.db().findUserById(payload.getInt("user_id"));
    if (!target) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    const std::string role = payload.getString("role");
    if (!validRole(role)) return Result::failure(protocol::code::kBadRequest, "роль: user, moderator или admin");
    if (target->id == actor.id) return Result::failure(protocol::code::kForbidden, "свою роль менять нельзя");
    if (target->role == "admin" && role != "admin") {
        // Считаем только активных администраторов: заблокированный панелью не управляет.
        long long activeAdmins = 0;
        database_.db().adminListUsers("", "admin", "active", 1, 0, activeAdmins);
        if (activeAdmins <= 1 && target->active) {
            return Result::failure(protocol::code::kForbidden, "нельзя понизить последнего администратора");
        }
    }
    if (role != "user" && (!target->emailVerified || !target->active)) {
        return Result::failure(protocol::code::kBadRequest, "роль можно дать только активному пользователю с подтверждённым email");
    }
    if (target->role != role) {
        const DatabaseError saved = database_.db().setUserRole(target->id, role);
        if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить роль");
        Json detail = Json::object();
        detail.set("role", Json(role));
        detail.set("old_role", Json(target->role));
        detail.set("actor_id", Json(actor.id));
        detail.set("actor_email", Json(actor.email));
        audit(target->id, "admin_role_changed", detail, remoteAddr);
        Json mine = Json::object();
        mine.set("role", Json(role));
        mine.set("old_role", Json(target->role));
        mine.set("target_id", Json(target->id));
        mine.set("target_email", Json(target->email));
        audit(actor.id, "admin_role_changed", mine, remoteAddr);
    }
    auto updated = database_.db().findUserById(target->id);
    result.ok = true;
    result.payload.set("user", adminUserJson(updated ? *updated : *target));
    return result;
}

AdminManager::Result AdminManager::setPlan(long long actorId, const Json& payload, const std::string& remoteAddr) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "admin", actor, result)) return result;
    const auto target = database_.db().findUserById(payload.getInt("user_id"));
    if (!target) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    const Json days = payload.get("days");
    const double number = days.asDouble(-1);
    if (!days.isNumber() || number != static_cast<double>(static_cast<long long>(number)) || number < 0 ||
        number > plan::kMaxDays) {
        return Result::failure(protocol::code::kBadRequest, "days — целое число дней от 0 до 3650 (0 — сбросить Pro)");
    }
    const int value = static_cast<int>(number);
    const DatabaseError saved = database_.db().setUserPlanDays(target->id, value);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось сохранить тариф");
    auto updated = database_.db().findUserById(target->id);
    const UserRecord& now = updated ? *updated : *target;
    Json detail = Json::object();
    detail.set("days", Json(static_cast<long long>(value)));
    detail.set("plan_until", Json(now.planUntil));
    detail.set("old_plan_until", Json(target->planUntil));
    detail.set("actor_id", Json(actor.id));
    detail.set("actor_email", Json(actor.email));
    audit(target->id, "admin_plan_changed", detail, remoteAddr);
    if (actor.id != target->id) {
        Json mine = Json::object();
        mine.set("days", Json(static_cast<long long>(value)));
        mine.set("plan_until", Json(now.planUntil));
        mine.set("target_id", Json(target->id));
        mine.set("target_email", Json(target->email));
        audit(actor.id, "admin_plan_changed", mine, remoteAddr);
    }
    result.ok = true;
    result.payload.set("user", adminUserJson(now));
    return result;
}

AdminManager::Result AdminManager::revokeSessions(long long actorId, const Json& payload, const std::string& remoteAddr) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "moderator", actor, result)) return result;
    const auto target = database_.db().findUserById(payload.getInt("user_id"));
    if (!target) return Result::failure(protocol::code::kNotFound, "пользователь не найден");
    std::string why;
    if (!canModerate(actor, *target, why)) return Result::failure(protocol::code::kForbidden, why);

    const long long revoked = liveSessions(database_.db(), target->id);
    database_.db().revokeAllSessions(target->id, "");
    database_.db().revokeAllUserRefreshTokens(target->id);
    result.kick.push_back(target->id);
    Json detail = Json::object();
    detail.set("actor_id", Json(actor.id));
    detail.set("actor_email", Json(actor.email));
    detail.set("revoked", Json(revoked));
    audit(target->id, "admin_sessions_revoked", detail, remoteAddr);
    Json mine = Json::object();
    mine.set("target_id", Json(target->id));
    mine.set("target_email", Json(target->email));
    mine.set("revoked", Json(revoked));
    audit(actor.id, "admin_sessions_revoked", mine, remoteAddr);
    result.ok = true;
    result.payload.set("revoked", Json(revoked));
    return result;
}

AdminManager::Result AdminManager::audit(long long actorId, const Json& payload) {
    UserRecord actor;
    Result result;
    if (!requireRole(actorId, "admin", actor, result)) return result;
    const long long userId = std::max<long long>(0, payload.getInt("user_id", 0));
    const long long beforeId = std::max<long long>(0, payload.getInt("before_id", 0));
    const int limit = static_cast<int>(std::clamp<long long>(payload.getInt("limit", 100), 1, 200));
    Json entries = Json::array();
    std::map<long long, std::string> emails;  // один запрос на пользователя, а не на запись
    for (const auto& entry : database_.db().listAuditPage(userId, beforeId, limit)) {
        Json item = auditJson(entry, true);
        if (entry.userId > 0) {
            auto found = emails.find(entry.userId);
            if (found == emails.end()) {
                const auto user = database_.db().findUserById(entry.userId);
                found = emails.emplace(entry.userId, user ? user->email : std::string()).first;
            }
            if (!found->second.empty()) item.set("user_email", Json(found->second));
        }
        entries.push(item);
    }
    result.ok = true;
    result.payload.set("entries", entries);
    return result;
}

}  // namespace selfvoy
