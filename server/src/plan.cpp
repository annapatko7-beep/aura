// selfvoy/plan.cpp — тариф пользователя.
#include "selfvoy/plan.h"

#include <atomic>

#include "selfvoy/idatabase.h"
#include "selfvoy/memorymanager.h"  // parseIsoUtc

namespace selfvoy::plan {

namespace {
std::atomic<bool> g_enforced{false};
}  // namespace

void setEnforced(bool enforced) { g_enforced.store(enforced); }
bool enforced() { return g_enforced.load(); }

bool isPro(const UserRecord& user, std::time_t now) {
    std::time_t until = 0;
    return !user.planUntil.empty() && parseIsoUtc(user.planUntil, until) && until > now;
}

Json toJson(const UserRecord& user, std::time_t now) {
    const bool pro = isPro(user, now);
    Json json = Json::object();
    json.set("name", Json(pro ? "pro" : "free"));
    json.set("until", Json(user.planUntil));
    json.set("enforced", Json(enforced()));
    return json;
}

bool proAllowed(IDatabase& db, long long userId) {
    if (!enforced()) return true;
    const auto user = db.findUserById(userId);
    return user && isPro(*user);
}

}  // namespace selfvoy::plan
