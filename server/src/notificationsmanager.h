// aura/notificationsmanager.h — in-app уведомления и push-доставка (этап 13).
//
// Единая «входящая»: напоминания, подтверждения, A2A-предложения, новые входы,
// изменения 2FA. In-app — таблица notifications + WS-событие notification.new;
// push — драйвер dev (лог) или webhook (шлюз, который доставляет в APNs:
// TLS/HTTP2 и JWT ES256 живут в шлюзе, net.h/crypto их намеренно не содержат).
// Настройки пользователя: тихие часы (notifications.quiet_hours) и отключённые
// типы (notifications.muted_kinds) — push не уходит, in-app всё равно пишется.
#pragma once

#include <string>

#include "aura/config.h"
#include "aura/connectionmanager.h"
#include "aura/databasemanager.h"
#include "aura/idatabase.h"
#include "aura/json.h"

namespace aura {

class NotificationsManager {
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

    NotificationsManager(DatabaseManager& database, ConnectionManager& connections,
                         const Config& config);

    // Создаёт уведомление: БД → событие владельцу → push устройствам.
    // Возвращает id (0 — ошибка записи).
    long long create(long long userId, const std::string& kind, const std::string& title,
                     const std::string& body, const Json& payload = Json::object());

    Result list(long long userId, bool unreadOnly, int limit);
    // id = 0 — отметить все прочитанными.
    Result markRead(long long userId, long long id);
    long long unreadCount(long long userId) const;

    // Устройства push-доставки.
    Result registerDevice(long long userId, const std::string& platform, const std::string& token);
    Result listDevices(long long userId);
    Result revokeDevice(long long userId, long long id);

private:
    bool inQuietHours(long long userId) const;
    bool kindMuted(long long userId, const std::string& kind) const;
    void deliverPush(const NotificationRecord& record, long long badge);
    Json apnsEnvelope(const PushDeviceRecord& device, const NotificationRecord& record,
                      long long badge) const;

    DatabaseManager& database_;
    ConnectionManager& connections_;
    const Config& config_;
};

}  // namespace aura
