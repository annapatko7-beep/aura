// selfvoy/referralmanager.h — реферальная программа (referral.*).
//
// У каждого пользователя один код из 8 символов [A-Z0-9] (выдаётся при первом
// запросе). Ссылка <SELFVOY_PUBLIC_URL>/#/signup?ref=<код> ведёт на регистрацию;
// auth.register с полем ref записывает, кто пригласил. Приглашённый засчитывается
// после подтверждения email — тогда пригласившему приходит уведомление
// referral.joined. Неверный или свой код регистрацию не ломает: он молча
// игнорируется.
//
// Награда (F3): за подтверждённого приглашённого обоим +30 дней Pro
// (plan_until = greatest(now, plan_until) + 30 дней) один раз (referrals.rewarded_at);
// пригласивший — не больше чем за 12 приглашённых, дальше растёт только счётчик.
// Приглашённому — уведомление referral.reward.
#pragma once

#include <string>
#include <utility>

#include "selfvoy/json.h"

namespace selfvoy {

class DatabaseManager;
class NotificationsManager;
struct Config;

class ReferralManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();
    };

    static constexpr int kRewardDays = 30;
    static constexpr int kRewardCap = 12;

    ReferralManager(DatabaseManager& database, NotificationsManager* notifications, const Config& config);

    // Код пользователя; при первом обращении создаётся. "" — сбой БД.
    std::string codeFor(long long userId);
    // referral.get: код, ссылка, счётчики и последние приглашённые (только имена).
    Result get(long long userId, const Json& payload);
    // Регистрация с кодом: source — link | booking | invite (иное → link).
    void attach(long long referredId, const std::string& code, const std::string& source);
    // Email приглашённого подтверждён: отметка, награда и уведомления (один раз).
    void verified(long long userId);

    // Код в каноническом виде (верхний регистр) или "", если формат неверен.
    static std::string normalizeCode(const std::string& code);

private:
    DatabaseManager& database_;
    NotificationsManager* notifications_;
    const Config& config_;
};

}  // namespace selfvoy
