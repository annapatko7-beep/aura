// selfvoy/invitemanager.h — приглашения по ссылке (invite.*).
//
// Владелец Selfvoy договаривается о встрече с человеком без Selfvoy: сервер подбирает
// свободные слоты владельца (AI-сервис, POST /v1/agent/slots) и выдаёт ссылку
// с одноразово показанным токеном. Гость открывает её без регистрации
// (invite.get / invite.respond — публичные методы), выбирает время или
// отказывается. При выборе сервер ещё раз проверяет, что время свободно, и
// атомарно закрывает приглашение; владельцу — запись в расписании, задача с
// напоминанием и уведомление. В БД хранится только sha256 токена.
#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "selfvoy/json.h"

namespace selfvoy {

class DatabaseManager;
class MemoryManager;
class NotificationsManager;
class ReferralManager;
class ScheduleLock;
struct Config;
struct InviteRecord;

class InviteManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    static constexpr int kMaxOpenInvites = 20;
    static constexpr int kGetPerMinute = 30;
    static constexpr int kRespondPerMinute = 10;

    // referrals (может быть nullptr) — код владельца в виде гостя (ссылка «Заведите свой Selfvoy»).
    InviteManager(DatabaseManager& database, MemoryManager& memory, NotificationsManager* notifications,
                  const Config& config, ReferralManager* referrals = nullptr,
                  ScheduleLock* scheduleLock = nullptr);

    // Владелец (нужен вход).
    Result create(long long ownerId, const Json& payload);
    Result list(long long ownerId, const Json& payload);
    Result cancel(long long ownerId, const Json& payload);

    // Гость (без входа): токен из ссылки в payload.token.
    Result get(const Json& payload, const std::string& remoteAddr);
    Result respond(const Json& payload, const std::string& remoteAddr);

    // Статус для клиента: open с истёкшим сроком — expired.
    static std::string effectiveStatus(const InviteRecord& record, std::time_t now);
    // Токен ссылки → ключ поиска в БД (hex sha256). Пустая строка — токен заведомо неверный.
    static std::string tokenHash(const std::string& token);

private:
    Json ownerJson(const InviteRecord& record, std::time_t now) const;
    Json guestJson(const InviteRecord& record, std::time_t now);
    // Слоты владельца от AI-сервиса; false — сервис недоступен или ответ некорректен.
    bool requestSlots(long long ownerId, const std::string& topic, int durationMinutes, int windowHours,
                      Json& slots, std::string& error);
    using Busy = std::vector<std::pair<std::time_t, std::time_t>>;
    // [start, end) не пересекается ни с одним интервалом busy.
    static bool isFree(const Busy& busy, std::time_t start, std::time_t end);
    // true — лимит по ключу исчерпан (окно 60 с); иначе событие засчитывается.
    bool limited(const std::string& key, int perMinute);

    DatabaseManager& database_;
    MemoryManager& memory_;
    NotificationsManager* notifications_;
    const Config& config_;
    ReferralManager* referrals_;
    ScheduleLock* scheduleLock_;  // nullptr — только respondMutex_ (один процесс)

    // Выбор времени: проверка «свободно» и запись встречи — под одной блокировкой,
    // чтобы два гостя разных приглашений не заняли одно время одновременно.
    std::mutex respondMutex_;

    struct Window {
        std::chrono::steady_clock::time_point start;
        int count = 0;
    };
    std::mutex limitsMutex_;
    std::unordered_map<std::string, Window> limits_;
};

}  // namespace selfvoy
