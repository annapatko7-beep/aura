// selfvoy/referralmanager.cpp
#include "selfvoy/referralmanager.h"

#include <algorithm>
#include <cctype>

#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/plan.h"
#include "selfvoy/protocol.h"
#include "notificationsmanager.h"

namespace selfvoy {

namespace {

constexpr std::size_t kCodeLength = 8;
// Без 0/O и 1/I — код удобно продиктовать.
constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::string randomCode() {
    const std::string bytes = crypto::randomBytes(kCodeLength);
    std::string code;
    code.reserve(kCodeLength);
    for (const unsigned char byte : bytes) code.push_back(kAlphabet[byte % (sizeof kAlphabet - 1)]);
    return code;
}

}  // namespace

ReferralManager::ReferralManager(DatabaseManager& database, NotificationsManager* notifications, const Config& config)
    : database_(database), notifications_(notifications), config_(config) {}

std::string ReferralManager::normalizeCode(const std::string& code) {
    if (code.size() != kCodeLength) return "";
    std::string upper;
    upper.reserve(code.size());
    for (const unsigned char ch : code) {
        if (std::isalnum(ch) == 0) return "";
        upper.push_back(static_cast<char>(std::toupper(ch)));
    }
    return upper;
}

std::string ReferralManager::codeFor(long long userId) {
    std::string code = database_.db().referralCode(userId);
    if (!code.empty()) return code;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (database_.db().setReferralCode(userId, randomCode())) break;
    }
    // Параллельный запрос мог успеть первым — берём то, что в БД.
    code = database_.db().referralCode(userId);
    if (code.empty()) SELFVOY_LOG(log::Level::Warn, "referral") << "не удалось выдать код пользователю " << userId;
    return code;
}

ReferralManager::Result ReferralManager::get(long long userId, const Json& payload) {
    Result result;
    const std::string code = codeFor(userId);
    if (code.empty()) {
        result.code = protocol::code::kInternal;
        result.message = "не удалось выдать реферальный код";
        return result;
    }
    long long limit = payload.getInt("limit", 50);
    limit = std::max(1LL, std::min(100LL, limit));
    const auto referrals = database_.db().listReferrals(userId, 1000);
    long long verified = 0;
    long long rewarded = 0;
    Json users = Json::array();
    for (const auto& referral : referrals) {
        if (!referral.verifiedAt.empty()) ++verified;
        if (!referral.rewardedAt.empty()) ++rewarded;
        if (static_cast<long long>(users.items().size()) >= limit) continue;
        Json user = Json::object();
        // Только имя: email и id приглашённых пригласившему не раскрываются.
        user.set("display_name", Json(referral.displayName));
        user.set("source", Json(referral.source));
        user.set("joined_at", Json(referral.createdAt));
        user.set("verified", Json(!referral.verifiedAt.empty()));
        users.push(user);
    }
    result.ok = true;
    result.payload.set("code", Json(code));
    result.payload.set("url", Json(config_.publicUrl.empty() ? std::string()
                                                              : config_.publicUrl + "/#/signup?ref=" + code));
    result.payload.set("joined", Json(static_cast<long long>(referrals.size())));
    result.payload.set("verified", Json(verified));
    result.payload.set("referrals", users);
    // Награды пригласившему: первые kRewardCap подтверждённых (дальше — только счётчик).
    result.payload.set("rewarded", Json(std::min<long long>(rewarded, kRewardCap)));
    result.payload.set("reward_days", Json(static_cast<long long>(kRewardDays)));
    result.payload.set("reward_cap", Json(static_cast<long long>(kRewardCap)));
    const auto user = database_.db().findUserById(userId);
    result.payload.set("plan", user ? plan::toJson(*user) : Json::object());
    return result;
}

void ReferralManager::attach(long long referredId, const std::string& rawCode, const std::string& rawSource) {
    const std::string code = normalizeCode(rawCode);
    if (code.empty() || referredId <= 0) return;
    const long long referrerId = database_.db().findReferrer(code);
    if (referrerId <= 0 || referrerId == referredId) return;
    const std::string source = rawSource == "booking" || rawSource == "invite" ? rawSource : "link";
    const DatabaseError saved = database_.db().createReferral(referrerId, referredId, source);
    if (!saved.ok) SELFVOY_LOG(log::Level::Info, "referral") << "реферал не записан: " << saved.message;
}

void ReferralManager::verified(long long userId) {
    const long long referrerId = database_.db().markReferralVerified(userId);
    // Награда — отдельным атомарным шагом: rewarded_at не даёт начислить дважды (в том
    // числе если подтверждение уже было отмечено, а начисление тогда не записалось).
    bool referrerRewarded = false;
    const bool rewarded = database_.db().rewardReferral(userId, kRewardDays, kRewardCap, referrerRewarded) > 0;
    if (!notifications_) return;
    if (referrerId > 0) {
        std::string name = "Новый пользователь";
        if (const auto user = database_.db().findUserById(userId)) {
            if (!user->displayName.empty()) name = user->displayName;
        }
        Json notify = Json::object();
        notify.set("display_name", Json(name));
        notify.set("reward_days", Json(static_cast<long long>(rewarded && referrerRewarded ? kRewardDays : 0)));
        notifications_->create(referrerId, "referral.joined", name + " теперь с Selfvoy",
                               std::string("по вашей ссылке-приглашению") +
                                   (rewarded && referrerRewarded ? " · +1 месяц Pro" : ""),
                               notify);
    }
    if (rewarded) {
        Json notify = Json::object();
        notify.set("days", Json(static_cast<long long>(kRewardDays)));
        if (const auto user = database_.db().findUserById(userId)) notify.set("until", Json(user->planUntil));
        notifications_->create(userId, "referral.reward", "Месяц Pro в подарок",
                               "Вы зарегистрировались по приглашению — 30 дней Pro уже действуют", notify);
    }
}

}  // namespace selfvoy
