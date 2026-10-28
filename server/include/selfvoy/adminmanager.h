// selfvoy/adminmanager.h — роли и админ-панель (admin.*).
//
// Роли: user (по умолчанию), moderator, admin.
//   moderator — видит обзор и пользователей, блокирует/разблокирует и завершает
//               сессии ОБЫЧНЫХ пользователей;
//   admin     — всё то же для любых пользователей (кроме себя), меняет роли,
//               читает журнал аудита.
// Первые администраторы задаются окружением SELFVOY_ADMIN_EMAILS (через запятую):
// подтверждённый email из списка получает роль admin при старте сервера и при
// входе. Последнего администратора понизить нельзя. Каждое действие пишется в
// audit_logs (кто, над кем, что, причина).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "selfvoy/json.h"

namespace selfvoy {

class DatabaseManager;
struct Config;
struct UserRecord;

class AdminManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();
        // Пользователи, чьи открытые соединения нужно закрыть (блокировка, отзыв сессий).
        std::vector<long long> kick;

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    AdminManager(DatabaseManager& database, const Config& config);

    static bool validRole(const std::string& role);
    static int rank(const std::string& role);  // user 0, moderator 1, admin 2

    // Роль из SELFVOY_ADMIN_EMAILS: при старте (всем найденным) и при входе (одному).
    void bootstrapAll();
    void bootstrap(const UserRecord& user, const std::string& remoteAddr);

    Result stats(long long actorId, std::size_t onlineNow);
    Result users(long long actorId, const Json& payload);
    Result user(long long actorId, const Json& payload);
    Result setActive(long long actorId, const Json& payload, const std::string& remoteAddr);
    Result setRole(long long actorId, const Json& payload, const std::string& remoteAddr);
    Result revokeSessions(long long actorId, const Json& payload, const std::string& remoteAddr);
    Result audit(long long actorId, const Json& payload);
    // Тариф (только admin): plan_until = now + days (0–3650; 0 — сброс в Free).
    Result setPlan(long long actorId, const Json& payload, const std::string& remoteAddr);

private:
    // Актор с ролью не ниже minRole; иначе — failure forbidden.
    bool requireRole(long long actorId, const std::string& minRole, UserRecord& actor, Result& failure);
    // Может ли actor менять статус/сессии target.
    static bool canModerate(const UserRecord& actor, const UserRecord& target, std::string& reason);
    Json adminUserJson(const UserRecord& user);
    void audit(long long userId, const std::string& kind, const Json& detail, const std::string& remoteAddr);

    DatabaseManager& database_;
    std::set<std::string> bootstrapEmails_;
};

}  // namespace selfvoy
