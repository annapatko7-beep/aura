// selfvoy/server/notificationsmanager.cpp — in-app + push (этап 13).
#include "notificationsmanager.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "selfvoy/log.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"
#include "selfvoy/telegrambot.h"

namespace selfvoy {

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
        SELFVOY_LOG(log::Level::Warn, "notify") << "не удалось сохранить уведомление: " << saved.message;
        return 0;
    }
    record.id = id;
    record.createdAt = isoNow();

    // In-app: событие живым сессиям владельца.
    connections_.deliverToUser(userId, protocol::event("notification.new", record.toJson()));

    // Push и Telegram: с уважением к тихим часам и отключённым типам (in-app уже сохранено).
    if (!kindMuted(userId, kind) && !inQuietHours(userId)) {
        deliverPush(record, unreadCount(userId));
        if (telegram_ && telegramKind(kind)) deliverTelegram(record);
    }
    return id;
}

bool NotificationsManager::telegramKind(const std::string& kind) {
    for (const char* prefix : {"booking.", "package.", "invite.", "referral.", "waitlist.", "tax."}) {
        if (kind.rfind(prefix, 0) == 0) return true;
    }
    return kind == "task.due" || kind == "telegram.question";  // F7: вопрос гостя специалисту
}

void NotificationsManager::deliverTelegram(const NotificationRecord& record) {
    if (!telegram_->enabled()) return;
    const auto user = database_.db().findUserById(record.userId);
    if (!user || user->telegramChatId == 0) return;
    const Json prefs = database_.db().getPreferences(record.userId);
    if (!prefs.get("notifications").getBool("telegram", true)) return;
    telegram_->sendMessage(user->telegramChatId, record.body.empty() ? record.title : record.title + "\n" + record.body);
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
    if (platform != "apns" && platform != "fcm" && platform != "webhook" && platform != "dev") {
        return Result::failure("bad_request", "platform: apns | fcm | webhook | dev");
    }
    if (token.empty()) return Result::failure("bad_request", "нужен token устройства");
    // Формат токена: он попадает в путь запроса к шлюзу (APNs: /3/device/<token>) —
    // только символы, которые выдают сами платформы.
    const auto only = [&token](auto allowed) { return std::all_of(token.begin(), token.end(), allowed); };
    bool valid = false;
    if (platform == "apns") {
        valid = token.size() >= 64 && token.size() <= 200 && only([](unsigned char ch) { return std::isxdigit(ch) != 0; });
    } else if (platform == "fcm") {
        valid = token.size() <= 4096 && only([](unsigned char ch) {
                    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == ':' || ch == '.';
                });
    } else {
        valid = token.size() <= 512 && only([](unsigned char ch) { return ch > 0x20 && ch < 0x7f; });
    }
    if (!valid) return Result::failure("bad_request", "некорректный token устройства");
    // Не больше 20 устройств на пользователя: иначе каждое уведомление — десятки вызовов шлюза.
    const auto devices = database_.db().listPushDevices(userId);
    const bool known = std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return device.platform == platform && device.token == token;
    });
    if (!known && devices.size() >= kMaxDevices) {
        return Result::failure("forbidden", "не больше 20 устройств для уведомлений — отключите ненужные");
    }
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
        const Json envelope = pushEnvelope(device, record, badge);

        if (config_.pushDriver == "webhook" && !config_.pushWebhookUrl.empty()) {
            std::string error;
            const auto response = net::httpRequest(
                "POST", config_.pushWebhookUrl,
                {{"Content-Type", "application/json"}}, envelope.dump(), config_.aiTimeoutMs, error);
            if (error.empty() && response.ok()) {
                database_.db().touchPushDevice(device.id);
                continue;
            }
            SELFVOY_LOG(log::Level::Warn, "push")
                << "шлюз не принял push для устройства " << device.id << ": "
                << (error.empty() ? "HTTP " + std::to_string(response.status) : error);
            continue;
        }

        // Драйвер dev (по умолчанию): честный лог вместо имитации доставки.
        database_.db().touchPushDevice(device.id);
        SELFVOY_LOG(log::Level::Info, "push")
            << "dev-push user=" << record.userId << " device=" << device.id
            << " kind=" << record.kind << " title=" << record.title;
    }
}

Json NotificationsManager::pushEnvelope(const PushDeviceRecord& device,
                                        const NotificationRecord& record,
                                        long long badge) const {
    Json envelope = Json::object();
    envelope.set("platform", Json(device.platform));
    envelope.set("token", Json(device.token));
    envelope.set("notification_id", Json(record.id));
    envelope.set("kind", Json(record.kind));

    if (device.platform == "fcm") {
        // Android/FCM: тело готово к форварду в FCM HTTP v1 API — шлюзу
        // остаётся получить OAuth2-токен сервисного аккаунта и отправить
        // POST {fcm.url} с заголовком Authorization: Bearer <токен>.
        Json notification = Json::object();
        notification.set("title", Json(record.title));
        notification.set("body", Json(record.body));

        Json androidNotification = Json::object();
        androidNotification.set("channel_id", Json("selfvoy"));
        androidNotification.set("sound", Json("default"));

        Json androidConfig = Json::object();
        androidConfig.set("priority", Json("high"));
        androidConfig.set("notification", androidNotification);

        Json message = Json::object();
        message.set("token", Json(device.token));
        message.set("notification", notification);
        message.set("android", androidConfig);
        Json data = Json::object();
        data.set("kind", Json(record.kind));
        data.set("notification_id", Json(record.id));
        data.set("data", record.payload.dump());
        message.set("data", data);

        Json fcm = Json::object();
        fcm.set("url", Json(config_.fcmUrl));
        fcm.set("message", message);
        envelope.set("fcm", fcm);
        return envelope;
    }

    // APNs (и dev/webhook без специальной платформы): формат готов к форварду
    // в APNs — шлюзу остаётся подписать JWT и отправить POST {apns.url}
    // с заголовками apns.headers и телом apns.payload.
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

    envelope.set("apns", apns);
    return envelope;
}

}  // namespace selfvoy
