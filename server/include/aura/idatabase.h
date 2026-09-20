// aura/idatabase.h — контракт хранилища.
//
// DatabaseManager — единственный класс, который знает про SQL. Остальные
// менеджеры (Auth/Chat/Memory/Agent/Tool) работают только с этим интерфейсом,
// поэтому сервер можно поднять и без PostgreSQL (EmbeddedDatabase) — например,
// в тестах или в демо-режиме.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aura/json.h"

namespace aura {

struct UserRecord {
    long long id = 0;
    std::string email;
    std::string passwordHash;
    std::string displayName;
    std::string avatarUrl;
    std::string timezone = "Europe/Moscow";
    std::string createdAt;
    std::string lastSeenAt;
    bool active = true;
    bool emailVerified = true;  // подтверждён ли email

    Json toJson(bool includeSecret = false) const;
};

struct SessionRecord {
    std::string id;
    long long userId = 0;
    std::string jwtId;
    std::string device;
    std::string remoteAddr;
    std::string createdAt;
    std::string expiresAt;
    std::string revokedAt;
};

// Одноразовый код/токен: подтверждение email или сброс пароля.
// Храним только SHA-256 от кода, срок действия — в expiresAt.
struct AuthTokenRecord {
    std::string id;
    long long userId = 0;
    std::string purpose;    // "email_verify" | "password_reset"
    std::string tokenHash;  // sha256(code) hex
    std::string createdAt;
    std::string expiresAt;
    std::string usedAt;
};

// Refresh-токен: ротация по «семействам» — повторное использование
// отозванного токена отзывает всё семейство (защита от кражи).
struct RefreshTokenRecord {
    std::string id;
    long long userId = 0;
    std::string familyId;
    std::string tokenHash;  // sha256(token) hex
    std::string device;
    std::string remoteAddr;
    std::string createdAt;
    std::string expiresAt;
    std::string revokedAt;
    std::string replacedBy;  // id токена, выданного при ротации
};

// Запись журнала безопасности.
struct AuditRecord {
    long long id = 0;
    long long userId = 0;  // 0 = системное событие без пользователя
    std::string kind;
    Json detail = Json::object();
    std::string remoteAddr;
    std::string createdAt;
};

// Настройка 2FA: TOTP-секрет, зашифрованный AES-256-GCM (ключ — env AURA_2FA_KEY).
// Одна запись на пользователя. enabled=true только после того, как пользователь
// подтвердил настройку одноразовым кодом — нельзя «включить» непроверенный секрет.
struct TwoFactorRecord {
    long long userId = 0;
    std::string secretEncrypted;  // AES-256-GCM blob (base64)
    bool enabled = false;
    long long lastUsedCounter = 0;  // защита от повторного использования кода
    std::string createdAt;
    std::string enabledAt;
    std::string updatedAt;
};

// Резервный код восстановления: храним только SHA-256(code). Код одноразовый.
struct RecoveryCodeRecord {
    std::string id;
    long long userId = 0;
    std::string codeHash;  // sha256(code) hex
    std::string createdAt;
    std::string usedAt;
};

// Доверенное устройство: после подтверждения 2FA освобождает устройство от
// повторного ввода кода на 90 дней (или до отзыва).
struct TrustedDeviceRecord {
    std::string id;
    long long userId = 0;
    std::string deviceHash;  // sha256(device_id) hex
    std::string device;      // человекочитаемое имя
    std::string remoteAddr;
    std::string createdAt;
    std::string expiresAt;
    std::string lastUsedAt;
    std::string revokedAt;
};

struct ChatRecord {
    long long id = 0;
    std::string kind = "direct";
    std::string title;
    long long createdBy = 0;
    std::string createdAt;
    std::string lastMessageAt;
    std::string lastMessageBody;
    long long lastMessageSenderId = 0;
    std::string lastMessageSender;

    Json toJson() const;
};

struct MemberRecord {
    long long chatId = 0;
    long long userId = 0;
    std::string role = "member";
    std::string displayName;
    std::string email;
    std::string lastReadAt;

    Json toJson() const;
};

struct MessageRecord {
    long long id = 0;
    long long chatId = 0;
    long long senderId = 0;
    std::string senderName;
    std::string kind = "text";
    std::string body;
    Json payload = Json::object();
    std::string createdAt;

    Json toJson() const;
};

struct MemoryRecord {
    long long id = 0;
    long long userId = 0;
    std::string kind = "fact";
    std::string text;
    double weight = 1.0;
    std::vector<std::string> tags;
    std::string updatedAt;

    Json toJson() const;
};

// Разрешение пользователя на инструмент Ауры (этап 8).
// mode: "allow" | "ask" | "deny". Пустая строка mode = «не задано» (нет строки).
struct ToolPermissionRecord {
    long long userId = 0;
    std::string tool;
    std::string mode = "ask";
    std::string updatedAt;

    Json toJson() const;
};

// Отложенное действие, требующее подтверждения (барьер подтверждения).
// status: pending | approved | denied | executed | failed | expired.
struct PendingActionRecord {
    long long id = 0;
    long long userId = 0;
    long long chatId = 0;
    std::string tool;
    Json args = Json::object();
    std::string summary;
    std::string status = "pending";
    Json result = Json::object();
    std::string createdAt;
    std::string resolvedAt;

    Json toJson() const;
};

// Задача/напоминание (этап 8). status: pending | done | cancelled.
// remindAt — когда планировщик должен прислать уведомление; remindedAt
// отмечается после отправки (одноразово).
struct TaskRecord {
    long long id = 0;
    long long userId = 0;
    long long chatId = 0;
    std::string title;
    std::string notes;
    std::string status = "pending";
    int priority = 0;
    std::string dueAt;
    std::string remindAt;
    std::string remindedAt;
    std::string createdAt;
    std::string completedAt;

    Json toJson() const;
};

// Подключение внешнего сервиса (этап 9). Токен хранится зашифрованным
// (AES-256-GCM); наружу record отдаётся без tokenEncrypted (includeSecret).
// status: active | expired | revoked | error.
struct IntegrationConnectionRecord {
    long long id = 0;
    long long userId = 0;
    std::string provider;  // google_calendar | google_gmail
    std::string account;
    std::string scope;
    std::string tokenEncrypted;
    std::string status = "active";
    std::string lastError;
    std::string createdAt;
    std::string lastUsedAt;

    Json toJson(bool includeSecret = false) const;
};

// Одноразовое состояние OAuth 2.0 + PKCE (этап 9).
struct OauthStateRecord {
    std::string state;
    long long userId = 0;
    std::string provider;
    std::string verifier;      // PKCE code_verifier
    std::string redirectUri;
    std::string createdAt;
    std::string expiresAt;
    std::string usedAt;
};

// In-app уведомление (этап 13). kind: task.due | confirmation.requested |
// a2a.proposal | login.new | twofactor.enabled | twofactor.disabled.
struct NotificationRecord {
    long long id = 0;
    long long userId = 0;
    std::string kind;
    std::string title;
    std::string body;
    Json payload = Json::object();
    std::string readAt;
    std::string createdAt;

    Json toJson() const;
};

// Устройство для push-доставки (этап 13). platform: apns | webhook | dev.
struct PushDeviceRecord {
    long long id = 0;
    long long userId = 0;
    std::string platform = "apns";
    std::string token;
    bool enabled = true;
    std::string createdAt;
    std::string lastUsedAt;

    Json toJson() const;  // без token: наружу отдаём только id/platform
};

struct DatabaseError {
    bool ok = true;
    std::string message;

    static DatabaseError success() { return DatabaseError{true, ""}; }
    static DatabaseError failure(std::string message) { return DatabaseError{false, std::move(message)}; }
};

// Репозиторий: типизированные операции поверх семи таблиц схемы.
class IDatabase {
public:
    virtual ~IDatabase() = default;

    virtual std::string name() const = 0;
    virtual bool connect(std::string& error) = 0;
    virtual bool healthy() const = 0;

    // Users
    virtual DatabaseError createUser(const std::string& email,
                                     const std::string& passwordHash,
                                     const std::string& displayName,
                                     long long& outId) = 0;
    virtual std::optional<UserRecord> findUserByEmail(const std::string& email) = 0;
    virtual std::optional<UserRecord> findUserById(long long id) = 0;
    virtual std::vector<UserRecord> searchUsers(const std::string& query, int limit) = 0;
    virtual DatabaseError touchUser(long long userId) = 0;
    // Миграция хэша пароля (PBKDF2 → Argon2id) при успешном входе.
    virtual DatabaseError updatePasswordHash(long long userId, const std::string& hash) = 0;
    virtual DatabaseError setEmailVerified(long long userId, bool verified) = 0;

    // Sessions
    virtual DatabaseError createSession(const SessionRecord& record) = 0;
    virtual DatabaseError revokeSession(const std::string& jwtId) = 0;
    virtual std::optional<SessionRecord> findSessionByJwt(const std::string& jwtId) = 0;
    virtual std::vector<SessionRecord> listSessions(long long userId) = 0;
    virtual DatabaseError revokeSessionById(long long userId, const std::string& sessionId) = 0;
    // Отозвать все сессии пользователя, кроме текущей (exceptJwtId может быть пустым).
    virtual DatabaseError revokeAllSessions(long long userId, const std::string& exceptJwtId) = 0;

    // Одноразовые коды: подтверждение email, сброс пароля.
    virtual DatabaseError createAuthToken(const AuthTokenRecord& record) = 0;
    virtual std::optional<AuthTokenRecord> findAuthToken(const std::string& purpose,
                                                         const std::string& tokenHash) = 0;
    virtual DatabaseError useAuthToken(const std::string& id) = 0;
    virtual DatabaseError deleteAuthTokens(long long userId, const std::string& purpose) = 0;

    // Refresh-токены с ротацией и детектом повторного использования.
    virtual DatabaseError createRefreshToken(const RefreshTokenRecord& record) = 0;
    virtual std::optional<RefreshTokenRecord> findRefreshToken(const std::string& tokenHash) = 0;
    virtual DatabaseError revokeRefreshToken(const std::string& id, const std::string& replacedBy) = 0;
    virtual DatabaseError revokeRefreshFamily(const std::string& familyId) = 0;
    virtual DatabaseError revokeAllUserRefreshTokens(long long userId) = 0;

    // Журнал безопасности.
    // --- 2FA: TOTP-настройки, резервные коды, доверенные устройства ---
    virtual DatabaseError upsertTwoFactor(const TwoFactorRecord& record) = 0;
    virtual std::optional<TwoFactorRecord> findTwoFactor(long long userId) = 0;
    virtual DatabaseError setTwoFactorEnabled(long long userId, bool enabled) = 0;
    virtual DatabaseError updateTwoFactorCounter(long long userId, long long counter) = 0;
    virtual DatabaseError deleteTwoFactor(long long userId) = 0;

    // Резервные коды: replaceRecoveryCodes полностью заменяет набор пользователя.
    virtual DatabaseError replaceRecoveryCodes(long long userId,
                                               const std::vector<RecoveryCodeRecord>& codes) = 0;
    virtual std::optional<RecoveryCodeRecord> findRecoveryCode(long long userId,
                                                               const std::string& codeHash) = 0;
    virtual DatabaseError useRecoveryCode(const std::string& id) = 0;
    virtual int countUnusedRecoveryCodes(long long userId) = 0;

    // Доверенные устройства.
    virtual DatabaseError addTrustedDevice(const TrustedDeviceRecord& record) = 0;
    virtual std::optional<TrustedDeviceRecord> findTrustedDevice(long long userId,
                                                                 const std::string& deviceHash) = 0;
    virtual std::vector<TrustedDeviceRecord> listTrustedDevices(long long userId) = 0;
    virtual DatabaseError touchTrustedDevice(long long userId, const std::string& deviceHash) = 0;
    virtual DatabaseError revokeTrustedDevice(long long userId, const std::string& id) = 0;
    virtual DatabaseError revokeAllTrustedDevices(long long userId) = 0;

    virtual DatabaseError insertAudit(long long userId,
                                      const std::string& kind,
                                      const Json& detail,
                                      const std::string& remoteAddr) = 0;
    virtual std::vector<AuditRecord> listAudit(long long userId, int limit) = 0;

    // Chats / ChatMembers
    virtual DatabaseError createChat(const std::string& kind,
                                     const std::string& title,
                                     long long createdBy,
                                     const std::vector<long long>& members,
                                     long long& outId) = 0;
    virtual std::vector<ChatRecord> listChats(long long userId, int limit) = 0;
    virtual std::optional<ChatRecord> findChat(long long chatId) = 0;
    virtual std::optional<long long> findDirectChat(long long first, long long second) = 0;
    virtual std::vector<MemberRecord> listMembers(long long chatId) = 0;
    virtual bool isMember(long long chatId, long long userId) = 0;
    virtual DatabaseError markRead(long long chatId, long long userId) = 0;

    // Messages
    virtual DatabaseError insertMessage(const MessageRecord& record, long long& outId) = 0;
    virtual std::vector<MessageRecord> listMessages(long long chatId, long long beforeId, int limit) = 0;

    // UserMemory
    virtual std::vector<MemoryRecord> listMemory(long long userId, int limit) = 0;
    virtual DatabaseError upsertMemory(long long userId,
                                       const std::string& kind,
                                       const std::string& text,
                                       double weight,
                                       const std::vector<std::string>& tags) = 0;

    // UserPreferences
    virtual Json getPreferences(long long userId) = 0;
    virtual DatabaseError setPreferences(long long userId, const Json& preferences) = 0;

    // ToolPermissions (этап 8, ядро безопасности).
    // getToolPermission возвращает "" если разрешение не задано (нет строки).
    virtual std::string getToolPermission(long long userId, const std::string& tool) = 0;
    virtual std::vector<ToolPermissionRecord> listToolPermissions(long long userId) = 0;
    virtual DatabaseError setToolPermission(long long userId,
                                            const std::string& tool,
                                            const std::string& mode) = 0;

    // PendingActions (барьер подтверждения опасных операций).
    // createPendingAction возвращает id созданной записи (0 при ошибке).
    virtual long long createPendingAction(const PendingActionRecord& record) = 0;
    virtual std::optional<PendingActionRecord> findPendingAction(long long id) = 0;
    virtual std::vector<PendingActionRecord> listPendingActions(long long userId,
                                                                const std::string& status,
                                                                int limit) = 0;
    // resolvePendingAction переводит запись в итоговый статус и сохраняет result.
    virtual DatabaseError resolvePendingAction(long long id,
                                               const std::string& status,
                                               const Json& result) = 0;

    // Tasks (задачи и напоминания, этап 8).
    virtual DatabaseError createTask(const TaskRecord& record, long long& outId) = 0;
    virtual std::optional<TaskRecord> findTask(long long id) = 0;
    // status пустой → все задачи пользователя.
    virtual std::vector<TaskRecord> listTasks(long long userId, const std::string& status, int limit) = 0;
    virtual DatabaseError setTaskStatus(long long id, const std::string& status) = 0;
    virtual DatabaseError deleteTask(long long id) = 0;
    // Планировщик: задачи с remind_at <= now, статус pending, ещё не напомненные.
    virtual std::vector<TaskRecord> listDueTasks(int limit) = 0;
    virtual DatabaseError markTaskReminded(long long id) = 0;

    // IntegrationConnections (этап 9): одна запись на (user, provider).
    virtual DatabaseError upsertIntegrationConnection(const IntegrationConnectionRecord& record,
                                                      long long& outId) = 0;
    virtual std::optional<IntegrationConnectionRecord> findIntegrationConnection(
        long long userId, const std::string& provider) = 0;
    virtual std::vector<IntegrationConnectionRecord> listIntegrationConnections(long long userId) = 0;
    virtual DatabaseError updateIntegrationToken(long long id,
                                                 const std::string& tokenEncrypted,
                                                 const std::string& status,
                                                 const std::string& lastError) = 0;
    virtual DatabaseError touchIntegrationUsed(long long id) = 0;
    virtual DatabaseError deleteIntegrationConnection(long long id) = 0;

    // IntegrationOauthStates (этап 9): одноразовые state + PKCE verifier.
    virtual DatabaseError createOauthState(const OauthStateRecord& record) = 0;
    virtual std::optional<OauthStateRecord> findOauthState(const std::string& state) = 0;
    virtual DatabaseError useOauthState(const std::string& state) = 0;

    // Notifications (этап 13): in-app «входящая» уведомлений.
    virtual DatabaseError createNotification(const NotificationRecord& record, long long& outId) = 0;
    virtual std::vector<NotificationRecord> listNotifications(long long userId,
                                                              bool unreadOnly,
                                                              int limit) = 0;
    virtual long long unreadNotificationCount(long long userId) = 0;
    virtual DatabaseError markNotificationRead(long long userId, long long id) = 0;
    virtual DatabaseError markAllNotificationsRead(long long userId) = 0;

    // PushDevices (этап 13): токены push-доставки (APNs).
    virtual DatabaseError registerPushDevice(long long userId,
                                             const std::string& platform,
                                             const std::string& token,
                                             long long& outId) = 0;
    virtual std::vector<PushDeviceRecord> listPushDevices(long long userId) = 0;
    virtual DatabaseError touchPushDevice(long long id) = 0;
    virtual DatabaseError deletePushDevice(long long userId, long long id) = 0;
};

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url);
std::unique_ptr<IDatabase> makeEmbeddedDatabase(const std::string& path);

std::string isoNow();  // "2026-09-17T12:00:00Z"

}  // namespace aura
