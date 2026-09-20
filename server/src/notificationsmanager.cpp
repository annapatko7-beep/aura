// aura/server/notificationsmanager.cpp — in-app + push (этап 13).
#include "notificationsmanager.h"

#include <ctime>

#include "aura/log.h"
#include "aura/net.h"
#include "aura/protocol.h"

namespace aura {

NotificationsManager::NotificationsManager(DatabaseManager& database,
                                           ConnectionManager& connections,
                                           const Config& config)
    : database_(database), connections_(connections), config_(config) {}

long long NotificationsManager::create(long long userId,
                                       const std::string& kind,
                                       const std::string& title,
                                       const std::string& body,
                                       const Json& payload) {
    NotificationRecord record;
    record.userId = userId;
    record.kind = kind;
    record.title = title;
    record.body = body;
    record.payload = payload;

    long long id = 0;
    const auto saved = database_.db().createNotification(record, id);
    if (!saved.ok) {
        AURA_LOG(log::Level::Warn, "notify") << "не удалось сохранить уведомление: " << saved.message;
        return 0;
    }
    record.id = id;
    record.createdAt = isoNow();

    // In-app: событие живым сессиям владельца.
    connections_.deliverToUser(userId, protocol::event("notification.new", record.toJson()));

    // Push: с уважением к тихим часам и отключённым типам (in-app уже сохранено).
    if (!kindMuted(userId, kind) && !inQuietHours(userId)) {
        deliverPush(record, unreadCount(userId));
    }
    return id;
}

NotificationsManager::Result NotificationsManager::list(long long userId,
                                                        bool unreadOnly,
                                                        int limit) {
    Result result;
    result.ok = true;
    Json notifications = Json::array();
    for (const auto& record : database_.db().listNotifications(userId, unreadOnly, limit)) {
        notifications.push(record.toJson());
    }
    result.payload.set("notifications", notifications);
    result.payload.set("unread", Json(unreadCount(userId)));
    return result;
}

NotificationsManager::Result NotificationsManager::markRead(long long userId, long long id) {
    if (id > 0) {
        const auto marked = database_.db().markNotificationRead(userId, id);
        if (!marked.ok) return Result::failure("not_found", marked.message);
    } else {
        const auto marked = database_.db().markAllNotificationsRead(userId);
        if (!marked.ok) return Result::failure("internal", marked.message);
    }
    Result result;
    result.ok = true;
    result.payload.set("unread", Json(unreadCount(userId)));
    return result;
}

long long NotificationsManager::unreadCount(long long userId) const {
    return database_.db().unreadNotificationCount(userId);
}

// ------------------------------------------------------------ push-устройства

NotificationsManager::Result NotificationsManager::registerDevice(long long userId,
                                                                  const std::string& platform,
                                                                  const std::string& token) {
    if (platform != "apns" && platform != "webhook" && platform != "dev") {
        return Result::failure("bad_request", "platform: apns | webhook | dev");
    }
    if (token.empty()) return Result::failure("bad_request", "нужен token устройства");
    long long id = 0;
    const auto registered = database_.db().registerPushDevice(userId, platform, token, id);
    if (!registered.ok) return Result::failure("internal", registered.message);
    Result result;
    result.ok = true;
    result.payload.set("id", Json(id));
    result.payload.set("platform", Json(platform));
    return result;
}

NotificationsManager::Result NotificationsManager::listDevices(long long userId) {
    Result result;
    result.ok = true;
    Json devices = Json::array();
    for (const auto& device : database_.db().listPushDevices(userId)) {
        devices.push(device.toJson());  // без токена
    }
    result.payload.set("devices", devices);
    return result;
}

NotificationsManager::Result NotificationsManager::revokeDevice(long long userId, long long id) {
    const auto removed = database_.db().deletePushDevice(userId, id);
    if (!removed.ok) return Result::failure("not_found", removed.message);
    Result result;
    result.ok = true;
    result.payload.set("id", Json(id));
    result.payload.set("revoked", Json(true));
    return result;
}

// ------------------------------------------------------------------- private

bool NotificationsManager::inQuietHours(long long userId) const {
    // Настройки: preferences.notifications.quiet_hours = {"start":"22:00","end":"08:00"} (UTC).
    const Json prefs = database_.db().getPreferences(userId);
    const Json quiet = prefs.get("notifications").get("quiet_hours");
    const std::string start = quiet.getString("start");
    const std::string end = quiet.getString("end");
    if (start.size() < 5 || end.size() < 5) return false;

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buffer[8];
    std::strftime(buffer, sizeof(buffer), "%H:%M", &tm);
    const std::string current(buffer);

    if (start <= end) return current >= start && current < end;   // окно внутри суток
    return current >= start || current < end;                     // окно через полночь
}

bool NotificationsManager::kindMuted(long long userId, const std::string& kind) const {
    const Json prefs = database_.db().getPreferences(userId);
    const Json muted = prefs.get("notifications").get("muted_kinds");
    for (const auto& item : muted.items()) {
        if (item.asString() == kind) return true;
    }
    return false;
}

void NotificationsManager::deliverPush(const NotificationRecord& record, long long badge) {
    for (const auto& device : database_.db().listPushDevices(record.userId)) {
        if (!device.enabled || device.token.empty()) continue;
        const Json envelope = apnsEnvelope(device, record, badge);

        if (config_.pushDriver == "webhook" && !config_.pushWebhookUrl.empty()) {
            std::string error;
            const auto response = net::httpRequest(
                "POST", config_.pushWebhookUrl,
                {{"Content-Type", "application/json"}}, envelope.dump(), config_.aiTimeoutMs, error);
            if (error.empty() && response.ok()) {
                database_.db().touchPushDevice(device.id);
                continue;
            }
            AURA_LOG(log::Level::Warn, "push")
                << "шлюз не принял push для устройства " << device.id << ": "
                << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
            continue;
        }

        // Драйвер dev (по умолчанию): честный лог вместо имитации доставки.
        database_.db().touchPushDevice(device.id);
        AURA_LOG(log::Level::Info, "push")
            << "dev-push user=" << record.userId << " device=" << device.id
            << " kind=" << record.kind << " title=" << record.title;
    }
}

Json NotificationsManager::apnsEnvelope(const PushDeviceRecord& device,
                                        const NotificationRecord& record,
                                        long long badge) const {
    // Формат готов к форварду в APNs: шлюзу остаётся подписать JWT и отправить
    // POST {apns.url} с заголовками apns.headers и телом apns.payload.
    Json aps = Json::object();
    Json alert = Json::object();
    alert.set("title", Json(record.title));
    alert.set("body", Json(record.body));
    aps.set("alert", alert);
    aps.set("badge", Json(badge));
    aps.set("sound", Json("default"));

    Json apnsPayload = Json::object();
    apnsPayload.set("aps", aps);
    apnsPayload.set("kind", Json(record.kind));
    apnsPayload.set("data", record.payload);

    Json headers = Json::object();
    headers.set("apns-topic", Json(config_.apnsTopic));
    headers.set("apns-push-type", Json("alert"));

    Json apns = Json::object();
    apns.set("url", Json(config_.apnsUrl + "/3/device/" + device.token));
    apns.set("headers", headers);
    apns.set("payload", apnsPayload);

    Json envelope = Json::object();
    envelope.set("platform", Json(device.platform));
    envelope.set("token", Json(device.token));
    envelope.set("notification_id", Json(record.id));
    envelope.set("kind", Json(record.kind));
    envelope.set("apns", apns);
    return envelope;
}

}  // namespace aura
