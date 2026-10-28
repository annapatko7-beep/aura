// selfvoy/plan.h — тариф пользователя (Free / Pro).
//
// Pro действует, пока users.plan_until > now(). SELFVOY_PLANS_ENFORCED=0 (по
// умолчанию, бета) — все функции Pro открыты всем; 1 — Pro-функции требуют
// действующего Pro, иначе ошибка pro_required (protocol::code::kProRequired).
// В каждом объекте user: plan {name: "pro"|"free", until: ISO|"", enforced}
// — name честный (по plan_until) независимо от enforced.
#pragma once

#include <ctime>
#include <string>

#include "selfvoy/json.h"

namespace selfvoy {

class IDatabase;
struct UserRecord;

namespace plan {

constexpr int kMaxDays = 3650;  // admin.setPlan / selfvoy pro: 0–3650 дней

// Режим тарифов процесса (Server::start из SELFVOY_PLANS_ENFORCED).
void setEnforced(bool enforced);
bool enforced();

bool isPro(const UserRecord& user, std::time_t now = std::time(nullptr));
Json toJson(const UserRecord& user, std::time_t now = std::time(nullptr));

// Можно ли пользователю Pro-функцию: !enforced || Pro.
bool proAllowed(IDatabase& db, long long userId);

}  // namespace plan
}  // namespace selfvoy
