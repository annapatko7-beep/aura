// selfvoy/idatabase.h — контракт хранилища.
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

#include "selfvoy/json.h"

namespace selfvoy {

struct UserRecord {
    long long id = 0;
    std::string email;
    std::string passwordHash;
    std::string displayName;
    std::string avatarUrl;
    std::string bio;
    std::string timezone = "Europe/Moscow";
    std::string createdAt;
    std::string lastSeenAt;
    bool active = true;
    bool emailVerified = true;  // подтверждён ли email
    std::string role = "user";  // user | moderator | admin (админ-панель)
    // Тариф и Telegram (S1a). planUntil — ISO UTC или "" (NULL): Pro, пока planUntil > now.
    std::string planUntil;
    long long telegramChatId = 0;  // 0 — чат не привязан
    std::string telegramUsername;

    Json toJson(bool includeSecret = false) const;  // + plan (selfvoy/plan.h)
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

// Настройка 2FA: TOTP-секрет, зашифрованный AES-256-GCM (ключ — env SELFVOY_2FA_KEY).
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

// Разрешение пользователя на инструмент Selfvoy (этап 8).
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

// Привязанный OAuth-аккаунт пользователя (Google, GitHub, Yandex, VK, Discord).
struct UserOauthAccountRecord {
    long long id = 0;
    long long userId = 0;
    std::string provider;
    std::string providerUserId;
    std::string email;
    std::string createdAt;
};

// Одноразовое состояние OAuth 2.0 для регистрации/входа.
struct AuthOauthStateRecord {
    std::string state;
    std::string provider;
    std::string verifier;
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

// Страница записи (booking.*): постоянная публичная ссылка владельца, по которой
// гости без Selfvoy сами записываются на свободное время.
// status: active | paused | archived. slug — публичная часть ссылки (#/b/<slug>).
struct BookingPageRecord {
    long long id = 0;
    long long ownerId = 0;
    std::string slug;
    std::string title;
    std::string description;
    int durationMinutes = 30;
    int daysAhead = 14;     // насколько вперёд открыта запись
    int maxPerDay = 4;      // не больше записей в местный день владельца
    int noticeHours = 2;    // минимум часов до начала встречи
    int maxSeries = 1;      // до скольких еженедельных встреч гость берёт сразу
    bool weekends = false;  // записывать и на выходные
    std::string status = "active";
    std::string createdAt;
    std::string updatedAt;
    // F1: напоминания гостю за сутки и за 2 часа (проверяется в момент отправки).
    bool guestReminders = true;
    // F3: скрыть подпись «Запись через Selfvoy» (Pro; действует, пока у владельца Pro).
    bool hideBranding = false;
    // F4: цена (мин. единицы, 0 — не показывать), предоплата и политика отмены.
    long long price = 0;
    std::string currency = "RUB";        // RUB | USD | EUR | KZT | BYN
    std::string prepay = "none";         // none | full | deposit
    long long deposit = 0;               // мин. единицы (prepay = deposit)
    std::string paymentUrl;              // https-ссылка оплаты владельца
    std::string paymentNote;             // реквизиты / СБП (одна строка)
    int payWithinMinutes = 60;           // слот держится до оплаты (15–1440)
    int cancelHours = 0;                 // отмена с возвратом — не позже чем за N ч (0 — всегда)
    std::string lateCancel = "allow";    // allow | block — позже срока
    std::string policyText;              // текст политики отмены
    // F5: предложения пакетов (абонементов) — [{id, title, sessions, price, valid_days}], ≤ 5.
    Json packages = Json::array();
    // F8: лист ожидания (Pro) — сколько минут освободившееся время держится за человеком из
    // листа (15–240) — и «пора на повторный приём» через N дней после последней встречи
    // (0 — выкл., иначе 7–180; Pro).
    bool waitlist = false;
    int waitlistHoldMinutes = 60;
    int rebookAfterDays = 0;
    // F7: ИИ-администратор в Telegram (Pro; нужен настроенный бот).
    bool aiAdmin = false;
};

// Запись гостя на странице. occurrences — встречи (одна или еженедельная серия):
// [{start, end, day, local, status: active|cancelled, task_id, confirmed_at?}] (ISO UTC,
// day — местная дата владельца; confirmed_at — гость нажал «Приду», F1).
// status записи: active | cancelled (все встречи отменены).
// manageHash — hex sha256 токена ссылки управления (#/m/<токен>), сам токен не хранится.
struct BookingRecord {
    long long id = 0;
    long long pageId = 0;
    long long ownerId = 0;
    std::string manageHash;
    std::string guestName;
    std::string guestEmail;
    std::string guestComment;
    Json occurrences = Json::array();
    std::string status = "active";
    std::string cancelledBy;  // guest | owner (у отменённой)
    int reschedules = 0;
    std::string createdAt;
    std::string updatedAt;
    // Токен ссылки управления, зашифрованный AES-256-GCM (Config::dataKey, AAD
    // "selfvoy-manage-token"): напоминания дают гостю ссылку. "" — старая запись.
    // Ключ поиска — по-прежнему только manageHash.
    std::string manageTokenEnc;
    // F4: оплата записи (selfvoy/payments.h) или null — оплаты нет.
    Json payment;
    // F5: второй получатель извещений гостя ("" — нет) и пакет, по которому сделана
    // запись (0 — без пакета). Пишутся при создании записи.
    std::string ccEmail;
    long long packageId = 0;
    // Снимок цены одной встречи на момент записи (мин. единицы, как BookingPageRecord::price):
    // 0 — запись по пакету (оплачена абонементом) или бесплатно. Пишется при создании записи;
    // перенос и отмена её не меняют.
    long long price = 0;
    std::string currency = "RUB";
    // F8: «пора записаться снова» — когда планировщик обработал запись (ISO UTC, "" — ещё нет)
    // и итог: "" | claimed (взята планировщиком) | sent | skipped | cancelled (поле страницы
    // выключено). Пишутся только claimDueRebooks / setRebookStatus / cancelPendingRebooks.
    std::string rebookSentAt;
    std::string rebookStatus;
};

// Лист ожидания страницы записи (F8, waitlist_entries). Гость оставляет имя и email,
// желаемые дни (местные даты владельца; пусто — любой) и часть дня. Освободилось
// подходящее время — оно держится за самой старой записью листа offer_expires_at
// (предложение: offer_*), гость забирает его по ссылке #/w/<токен> (в БД — sha256 и шифр
// токена, AAD "selfvoy-waitlist-token"). status: waiting | offered | booked | left | expired;
// offerStatus: "" (не было) | offered | claimed | expired | cancelled. Одна активная
// (waiting/offered) запись на страницу и email (без учёта регистра).
struct WaitlistEntryRecord {
    long long id = 0;
    long long ownerId = 0;
    long long pageId = 0;
    std::string guestName;
    std::string guestEmail;
    Json days = Json::array();        // ["YYYY-MM-DD", …], ≤ 14
    std::string partOfDay = "any";    // any | morning | afternoon | evening
    std::string comment;
    std::string status = "waiting";
    std::string tokenHash;
    std::string tokenEnc;
    long long telegramChatId = 0;     // 0 — бот не подписан
    std::string offerStart;           // ISO UTC; "" — предложения не было
    std::string offerEnd;
    std::string offerDay;             // местная дата владельца
    std::string offerLocal;           // местное время владельца YYYY-MM-DDTHH:MM
    std::string offerExpiresAt;
    std::string offerStatus;
    long long bookingId = 0;          // запись, созданная по предложению
    std::string queuedAt;             // место в очереди (старше — раньше)
    std::string expiresAt;            // запись листа живёт 30 дней
    std::string createdAt;
    std::string updatedAt;
};

// Покупка пакета (абонемента) гостем, F5 (package_purchases). Пакет — снимок
// предложения страницы на момент покупки (название, число занятий, цена, срок).
// status: pending (ждём оплату) | active | used (занятий не осталось) | expired
// (истёк valid_until) | cancelled. tokenHash — hex sha256 токена ссылки #/p/<токен>;
// tokenEnc — сам токен, AES-256-GCM (Config::dataKey, AAD "selfvoy-package-token").
struct PackagePurchaseRecord {
    long long id = 0;
    long long ownerId = 0;
    long long pageId = 0;
    long long offerId = 0;      // id предложения страницы (0 — выдан владельцем вне предложений)
    std::string title;
    int sessionsTotal = 0;
    int sessionsUsed = 0;
    int validDays = 90;         // срок действия с момента активации
    long long price = 0;        // мин. единицы валюты
    std::string currency = "RUB";
    std::string validUntil;     // ISO UTC; "" — ещё не активирован
    std::string status = "pending";
    Json payment;               // selfvoy/payments.h (kind: package) или null
    std::string guestName;
    std::string guestEmail;
    std::string ccEmail;
    std::string tokenHash;
    std::string tokenEnc;
    std::string createdAt;
    std::string updatedAt;
};

// Подключённый владельцем магазин ЮKassa (payment_accounts). secretEnc — секретный
// ключ, AES-256-GCM (Config::dataKey, AAD "selfvoy-payment-secret").
struct PaymentAccountRecord {
    long long userId = 0;
    std::string provider = "yookassa";
    std::string shopId;
    std::string secretEnc;
    std::string connectedAt;
};

// Конец последней активной встречи записи (ISO UTC) или "" — активных нет.
std::string bookingEndsAt(const Json& occurrences);

// Приглашённый по реферальной ссылке (referral.*).
struct ReferralRecord {
    long long referrerId = 0;
    long long referredId = 0;
    std::string displayName;  // имя приглашённого
    std::string source;       // link | booking | invite
    std::string createdAt;
    std::string verifiedAt;   // пусто — email ещё не подтверждён
    std::string rewardedAt;   // пусто — награда (+30 дней Pro) ещё не начислялась
};

// Напоминание гостю о встрече (F1, booking_reminders): kind 24h | 2h; dueAt — когда
// отправить; sentAt пусто — ещё не отправлено (ISO UTC).
struct BookingReminderRecord {
    long long id = 0;
    long long bookingId = 0;
    long long ownerId = 0;
    std::string occurrenceStart;
    std::string kind;
    std::string dueAt;
    std::string sentAt;
};

// Приглашение по ссылке (invite.*): встреча с человеком без Selfvoy.
// status: open | accepted | declined | cancelled (expired вычисляется по expiresAt).
// tokenHash — hex sha256 токена ссылки; сам токен не хранится.
struct InviteRecord {
    long long id = 0;
    long long ownerId = 0;
    std::string tokenHash;
    std::string topic;
    std::string note;
    int durationMinutes = 60;
    Json slots = Json::array();   // [{start, end}] ISO UTC
    std::string status = "open";
    std::string guestName;
    std::string guestEmail;
    std::string guestComment;
    Json chosen = Json::object();  // {start, end} после accept
    std::string expiresAt;
    std::string createdAt;
    std::string respondedAt;
};

// F7: чат гостя с ИИ-администратором (telegram_guest_chats): /start p_<slug> привязывает
// чат к странице (один чат — одна страница; повтор с другой страницы перепривязывает и
// очищает историю). history — последние ≤ 10 сообщений [{role: guest|admin, text}].
struct TelegramGuestChatRecord {
    long long chatId = 0;
    long long pageId = 0;
    long long ownerId = 0;
    std::string guestName;  // имя из Telegram (first_name + last_name)
    std::string username;   // ник без @ ("" — нет)
    Json history = Json::array();
    std::string createdAt;
    std::string updatedAt;
};

// F7: вопрос гостя, переданный специалисту (telegram_questions). status: open | answered.
struct TelegramQuestionRecord {
    long long id = 0;
    long long ownerId = 0;
    long long pageId = 0;
    long long chatId = 0;
    std::string guestName;
    std::string username;
    std::string text;
    std::string status = "open";
    std::string answer;      // последний ответ специалиста ("" — нет)
    std::string createdAt;
    std::string answeredAt;  // "" — ещё не отвечен
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
    // Поиск по подстроке display_name (email в поиске не участвует: подбор
    // адресов по подстроке раскрывал базу пользователей).
    virtual std::vector<UserRecord> searchUsers(const std::string& query, int limit) = 0;
    virtual DatabaseError touchUser(long long userId) = 0;
    // Профиль (profile.update): меняются только переданные поля
    // display_name / bio / avatar_url; значения уже проверены сервером.
    virtual DatabaseError updateProfile(long long userId, const Json& fields) = 0;
    // Миграция хэша пароля (PBKDF2 → Argon2id) при успешном входе.
    virtual DatabaseError updatePasswordHash(long long userId, const std::string& hash) = 0;
    virtual DatabaseError setEmailVerified(long long userId, bool verified) = 0;
    // Блокировка/разблокировка аккаунта (is_active). Отключённый пользователь
    // не может войти, а его токены отклоняются (AuthManager::verifyToken).
    virtual DatabaseError setUserActive(long long userId, bool active) = 0;
    // Полное удаление аккаунта (auth.deleteAccount) со всеми каскадно связанными данными.
    virtual DatabaseError deleteUser(long long userId) = 0;

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
    // Атомарно: отзывает токен, только если он ещё не отозван (иначе failure).
    // Так из двух одновременных auth.refresh одним токеном проходит один.
    virtual DatabaseError revokeRefreshToken(const std::string& id, const std::string& replacedBy) = 0;
    virtual DatabaseError revokeRefreshFamily(const std::string& familyId) = 0;
    virtual DatabaseError revokeAllUserRefreshTokens(long long userId) = 0;
    // Все refresh-токены пользователя, кроме семейства keepFamilyId (текущее
    // устройство при sessions.revokeAll). Пустой keepFamilyId — отозвать все.
    virtual DatabaseError revokeUserRefreshTokensExcept(long long userId, const std::string& keepFamilyId) = 0;

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
    // Атомарный переход pending → status для записи владельца userId.
    // true — переход выполнен этим вызовом; false — запись не найдена, чужая
    // или уже обработана (двойное confirmation.approve не исполнит дважды).
    virtual bool claimPendingAction(long long id, long long userId, const std::string& status) = 0;

    // Tasks (задачи и напоминания, этап 8).
    virtual DatabaseError createTask(const TaskRecord& record, long long& outId) = 0;
    virtual std::optional<TaskRecord> findTask(long long id) = 0;
    // status пустой → все задачи пользователя.
    virtual std::vector<TaskRecord> listTasks(long long userId, const std::string& status, int limit) = 0;
    virtual DatabaseError setTaskStatus(long long id, const std::string& status) = 0;
    virtual DatabaseError deleteTask(long long id) = 0;
    // Планировщик: задачи с remind_at <= now, статус pending, ещё не напомненные.
    // userId = 0 — все пользователи (фоновый планировщик), иначе только его задачи.
    virtual std::vector<TaskRecord> listDueTasks(int limit, long long userId = 0) = 0;
    // Атомарно: помечает напоминание отправленным, только если оно ещё не
    // отправлено (иначе failure) — планировщик и tasks.due не дублируют push.
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

    // Auth OAuth: регистрация и вход через внешних провайдеров.
    virtual DatabaseError linkOauthAccount(const UserOauthAccountRecord& record) = 0;
    virtual std::optional<UserOauthAccountRecord> findOauthAccount(const std::string& provider,
                                                                   const std::string& providerUserId) = 0;
    virtual std::vector<UserOauthAccountRecord> listUserOauthAccounts(long long userId) = 0;
    virtual DatabaseError createAuthOauthState(const AuthOauthStateRecord& record) = 0;
    virtual std::optional<AuthOauthStateRecord> findAuthOauthState(const std::string& state) = 0;
    virtual DatabaseError useAuthOauthState(const std::string& state) = 0;

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

    // Админ-панель (admin.*): роли, блокировка, выборки и сводка.
    virtual DatabaseError setUserRole(long long userId, const std::string& role) = 0;
    // query — подстрока email или имени без учёта регистра; role — "" или роль;
    // status — "" | active | disabled (is_active=false) | unverified (email не подтверждён).
    // Порядок: id по убыванию; total — число строк без учёта limit/offset.
    virtual std::vector<UserRecord> adminListUsers(const std::string& query,
                                                   const std::string& role,
                                                   const std::string& status,
                                                   int limit,
                                                   int offset,
                                                   long long& total) = 0;
    // users_total, users_verified, users_disabled, users_2fa, users_new_7d,
    // moderators, admins, sessions_active (не отозванные и не истёкшие).
    virtual Json adminStats() = 0;
    // Журнал аудита страницами: userId 0 — все записи; beforeId 0 — с самых новых.
    virtual std::vector<AuditRecord> listAuditPage(long long userId, long long beforeId, int limit) = 0;

    // Общие для всех экземпляров сервера счётчики попыток (вход, коды, лимиты гостей).
    // По умолчанию (встроенное хранилище — один процесс) не поддерживаются: менеджеры
    // считают в памяти. PostgreSQL — таблица rate_limits.
    virtual bool sharedRateLimits() const { return false; }
    // Засчитать событие в окне windowSec; возвращает число событий в текущем окне.
    virtual int rateLimitHit(const std::string& /*key*/, int /*windowSec*/) { return 0; }
    // Событий в текущем окне (0 — окна нет или оно истекло).
    virtual int rateLimitCount(const std::string& /*key*/) { return 0; }
    virtual void rateLimitClear(const std::string& /*key*/) {}
    // Межпроцессная блокировка расписания владельца (ScheduleLock). true — захвачена.
    virtual bool tryLockSchedule(long long /*ownerId*/) { return true; }
    virtual void unlockSchedule(long long /*ownerId*/) {}

    // --- Рост (S1a): служебное состояние, тариф, Telegram ---------------------
    // server_state: ключ → значение (смещение getUpdates Telegram и т.п.). "" — ключа нет.
    virtual std::string getState(const std::string& key) = 0;
    virtual DatabaseError setState(const std::string& key, const std::string& value) = 0;
    // Ведущий экземпляр фоновой задачи (опрос Telegram): advisory lock PostgreSQL на
    // соединении блокировок. Держатель при повторном вызове получает true без второго
    // захвата; после обрыва соединения блокировка берётся заново (если свободна).
    // Встроенное хранилище работает в одном процессе — всегда ведущий.
    virtual bool tryLeaderLock(long long /*key*/) { return true; }
    virtual void releaseLeaderLock(long long /*key*/) {}
    // Тариф: plan_until = now + days; days = 0 — сброс в NULL (Free).
    virtual DatabaseError setUserPlanDays(long long userId, int days) = 0;
    // Продление: plan_until = greatest(now, plan_until) + days (days > 0).
    virtual DatabaseError extendUserPlanDays(long long userId, int days) = 0;
    // Telegram владельца. chatId = 0 — отвязать. Чат, привязанный к другому
    // аккаунту, у того отвязывается (один чат — один аккаунт).
    virtual DatabaseError setUserTelegram(long long userId, long long chatId, const std::string& username) = 0;
    // Telegram-чаты записи (booking_telegram). true — чат привязан (или уже был);
    // false — у записи уже maxChats чатов или ошибка.
    virtual bool linkBookingTelegram(long long bookingId, long long chatId, int maxChats) = 0;
    virtual std::vector<long long> bookingTelegramChats(long long bookingId) = 0;
    // /stop или 403 от Telegram: чат отвязывается от пользователя, от всех записей, от
    // подписок листа ожидания (F8) и от страницы ИИ-администратора (F7: строка
    // telegram_guest_chats удаляется).
    virtual DatabaseError unlinkTelegramChat(long long chatId) = 0;

    // --- F7: ИИ-администратор в Telegram ------------------------------------
    // Вставка или замена по chat_id (created_at сохраняется, updated_at = now).
    virtual DatabaseError saveTelegramGuestChat(const TelegramGuestChatRecord& record) = 0;
    virtual std::optional<TelegramGuestChatRecord> findTelegramGuestChat(long long chatId) = 0;
    virtual DatabaseError createTelegramQuestion(const TelegramQuestionRecord& record, long long& outId) = 0;
    virtual std::optional<TelegramQuestionRecord> findTelegramQuestion(long long id) = 0;
    // Вопросы владельца, новые сверху; status "" — любые.
    virtual std::vector<TelegramQuestionRecord> listTelegramQuestions(long long ownerId, const std::string& status,
                                                                      int limit) = 0;
    // Ответ специалиста: status = answered, answer, answered_at = now. false — вопроса нет.
    virtual bool answerTelegramQuestion(long long id, const std::string& answer) = 0;

    // Страницы записи (booking.*).
    virtual DatabaseError createBookingPage(const BookingPageRecord& record, long long& outId) = 0;
    virtual std::optional<BookingPageRecord> findBookingPage(long long id) = 0;
    virtual std::optional<BookingPageRecord> findBookingPageBySlug(const std::string& slug) = 0;
    // Все страницы владельца, кроме archived; новые сверху.
    virtual std::vector<BookingPageRecord> listBookingPages(long long ownerId) = 0;
    // Перезаписывает изменяемые поля (title … status) по id; updated_at = now.
    virtual DatabaseError updateBookingPage(const BookingPageRecord& record) = 0;

    virtual DatabaseError createBooking(const BookingRecord& record, long long& outId) = 0;
    virtual std::optional<BookingRecord> findBooking(long long id) = 0;
    virtual std::optional<BookingRecord> findBookingByToken(const std::string& manageHash) = 0;
    // Записи владельца (pageId = 0 — всех страниц), новые сверху; activeOnly — без отменённых.
    virtual std::vector<BookingRecord> listBookings(long long ownerId, long long pageId, bool activeOnly,
                                                    int limit) = 0;
    // Активные записи с предстоящими встречами (конец последней активной встречи позже
    // текущего момента; в PostgreSQL — колонка ends_at с индексом). Прошедшие записи
    // остаются active навсегда — без этого фильтра они вытесняли бы будущие из лимита.
    virtual std::vector<BookingRecord> listUpcomingBookings(long long ownerId, long long pageId, int limit) = 0;
    // Перезаписывает occurrences, status, cancelled_by, reschedules, payment и непустой
    // manage_hash (новая ссылка управления); updated_at = now.
    virtual DatabaseError updateBooking(const BookingRecord& record) = 0;
    // F4: записи с ожидаемой оплатой (payment.status = pending), раньше истекающие — первыми.
    virtual std::vector<BookingRecord> listAwaitingPayments(int limit) = 0;

    // F5: записи по пакету (bookings.package_id), старые сверху.
    virtual std::vector<BookingRecord> listBookingsByPackage(long long packageId, int limit) = 0;

    // F5: покупки пакетов (package_purchases).
    virtual DatabaseError createPackagePurchase(const PackagePurchaseRecord& record, long long& outId) = 0;
    virtual std::optional<PackagePurchaseRecord> findPackagePurchase(long long id) = 0;
    virtual std::optional<PackagePurchaseRecord> findPackagePurchaseByToken(const std::string& tokenHash) = 0;
    // Пакеты владельца (pageId = 0 — всех страниц; status "" — любые), новые сверху.
    virtual std::vector<PackagePurchaseRecord> listPackagePurchases(long long ownerId, long long pageId,
                                                                   const std::string& status, int limit) = 0;
    // Перезаписывает sessions_total, sessions_used, valid_until, status, payment; updated_at = now.
    virtual DatabaseError updatePackagePurchase(const PackagePurchaseRecord& record) = 0;
    // Покупки, ждущие оплату (status = pending), раньше истекающие — первыми.
    virtual std::vector<PackagePurchaseRecord> listPendingPackagePurchases(int limit) = 0;
    // Активные пакеты с valid_until <= nowIso (планировщик переводит их в expired).
    virtual std::vector<PackagePurchaseRecord> listExpiredPackagePurchases(const std::string& nowIso, int limit) = 0;

    // F8: лист ожидания (waitlist_entries). create — failure, если у страницы уже есть
    // активная (waiting/offered) запись с этим email (без учёта регистра) или токен занят.
    virtual DatabaseError createWaitlistEntry(const WaitlistEntryRecord& record, long long& outId) = 0;
    virtual std::optional<WaitlistEntryRecord> findWaitlistEntry(long long id) = 0;
    virtual std::optional<WaitlistEntryRecord> findWaitlistEntryByToken(const std::string& tokenHash) = 0;
    // Активная (waiting/offered) запись страницы с этим email (emailLower — в нижнем регистре).
    virtual std::optional<WaitlistEntryRecord> findActiveWaitlistEntry(long long pageId,
                                                                       const std::string& emailLower) = 0;
    // Записи владельца (pageId = 0 — всех страниц; status "" — любые), новые сверху.
    virtual std::vector<WaitlistEntryRecord> listWaitlistEntries(long long ownerId, long long pageId,
                                                                 const std::string& status, int limit) = 0;
    // Очередь страницы: waiting с expires_at > nowIso, по queued_at (старые первыми), затем id.
    virtual std::vector<WaitlistEntryRecord> listWaitingEntries(long long pageId, const std::string& nowIso,
                                                                int limit) = 0;
    virtual int countActiveWaitlistEntries(long long pageId) = 0;
    // Перезаписывает guest_name, days, part_of_day, comment, status, offer_*, booking_id,
    // queued_at, expires_at; updated_at = now. Вызывающий держит блокировку расписания владельца.
    virtual DatabaseError updateWaitlistEntry(const WaitlistEntryRecord& record) = 0;
    // Telegram-чат подписки на предложения (0 — отвязать); остальные поля не меняются.
    virtual DatabaseError setWaitlistTelegram(long long entryId, long long chatId) = 0;
    // Держания владельца: offered с offer_expires_at > nowIso (занятость при подборе времени).
    virtual std::vector<WaitlistEntryRecord> listWaitlistHolds(long long ownerId, const std::string& nowIso) = 0;
    // Планировщик: offered с offer_expires_at <= nowIso и waiting с expires_at <= nowIso.
    virtual std::vector<WaitlistEntryRecord> listDueWaitlist(const std::string& nowIso, int limit) = 0;

    // F8: «пора записаться снова». Атомарный захват (PostgreSQL — FOR UPDATE SKIP LOCKED):
    // до limit активных записей, у страницы которых rebook_after_days > 0, ещё не
    // обработанных (rebook_sent_at пуст), с ends_at + rebook_after_days <= nowIso, получают
    // rebook_sent_at = nowIso, rebook_status = claimed и возвращаются (раньше закончившиеся — первыми).
    virtual std::vector<BookingRecord> claimDueRebooks(const std::string& nowIso, int limit) = 0;
    virtual DatabaseError setRebookStatus(long long bookingId, const std::string& status) = 0;
    // Выключение поля страницы: необработанные активные записи с ends_at <= nowIso → cancelled.
    virtual DatabaseError cancelPendingRebooks(long long pageId, const std::string& nowIso) = 0;
    // У гостя (email без учёта регистра) есть другая активная запись этой страницы,
    // заканчивающаяся позже endsAtIso (будущая или более поздняя прошедшая).
    virtual bool hasLaterBooking(long long pageId, const std::string& emailLower, long long exceptBookingId,
                                 const std::string& endsAtIso) = 0;

    // F10: оплаты, по которым владельцу напомнили выдать чек «Мой налог» (payment.npd = true),
    // получены (status paid) и чек не отмечен (receipt_at пуст) — новые сверху.
    virtual std::vector<BookingRecord> listReceiptPendingBookings(long long ownerId, int limit) = 0;
    virtual std::vector<PackagePurchaseRecord> listReceiptPendingPackages(long long ownerId, int limit) = 0;

    // F4: магазин ЮKassa владельца (payment_accounts). set — вставка или замена.
    virtual std::optional<PaymentAccountRecord> findPaymentAccount(long long userId) = 0;
    virtual DatabaseError setPaymentAccount(const PaymentAccountRecord& record) = 0;
    virtual DatabaseError deletePaymentAccount(long long userId) = 0;

    // Рефералы (referral.*). Код — 8 символов [A-Z0-9], один на пользователя.
    virtual std::string referralCode(long long userId) = 0;
    // false — код уже занят (другим пользователем) или у пользователя уже есть код.
    virtual bool setReferralCode(long long userId, const std::string& code) = 0;
    // 0 — код не найден.
    virtual long long findReferrer(const std::string& code) = 0;
    // Один пригласивший на пользователя: повтор для того же referredId — failure.
    virtual DatabaseError createReferral(long long referrerId, long long referredId, const std::string& source) = 0;
    // Атомарно отмечает подтверждение email приглашённого; возвращает id пригласившего,
    // если отметка поставлена этим вызовом (иначе 0 — не приглашён или уже отмечен).
    virtual long long markReferralVerified(long long referredId) = 0;
    // Приглашённые пользователем, новые сверху.
    virtual std::vector<ReferralRecord> listReferrals(long long referrerId, int limit) = 0;
    // Награда за подтверждённого приглашённого (F3), атомарно и один раз: у строки
    // referrals (verified_at задан, rewarded_at пуст) ставится rewarded_at, приглашённому
    // +days дней Pro, пригласившему — тоже, если до этого наград у него было меньше cap.
    // Возвращает id пригласившего (0 — не приглашён, не подтверждён или уже награждён);
    // referrerRewarded — начислено ли пригласившему.
    virtual long long rewardReferral(long long referredId, int days, int cap, bool& referrerRewarded) = 0;

    // Напоминания гостю (F1, booking_reminders). Повтор (booking, start, kind) не дублируется.
    virtual DatabaseError addBookingReminders(const std::vector<BookingReminderRecord>& reminders) = 0;
    // Удалить неотправленные напоминания встречи (occurrenceStart ISO) или всей записи ("").
    virtual DatabaseError deleteBookingReminders(long long bookingId, const std::string& occurrenceStart) = 0;
    // Все напоминания записи по due_at.
    virtual std::vector<BookingReminderRecord> listBookingReminders(long long bookingId) = 0;
    // Атомарный захват: до limit неотправленных с due_at <= nowIso получают sent_at = now
    // и возвращаются (PostgreSQL — FOR UPDATE SKIP LOCKED: два экземпляра не возьмут одно).
    virtual std::vector<BookingReminderRecord> claimDueReminders(const std::string& nowIso, int limit) = 0;

    // Приглашения по ссылке (invite.*).
    virtual DatabaseError createInvite(const InviteRecord& record, long long& outId) = 0;
    virtual std::optional<InviteRecord> findInvite(long long id) = 0;
    virtual std::optional<InviteRecord> findInviteByToken(const std::string& tokenHash) = 0;
    // Новые сверху.
    virtual std::vector<InviteRecord> listInvites(long long ownerId, int limit) = 0;
    // Открытые (status=open, срок не истёк) приглашения владельца — для лимита.
    virtual int countOpenInvites(long long ownerId) = 0;
    // Атомарный переход open → status (accepted | declined | cancelled) с ответом
    // гостя (guest*, chosen из response). false — запись не найдена или уже закрыта:
    // два одновременных ответа не займут время дважды.
    virtual bool closeInvite(long long id, const std::string& status, const InviteRecord& response) = 0;

    // ICS-подписка владельца (calendar.feed*, GET /ics/<токен>.ics): users.ics_token_hash —
    // hex sha256 токена ссылки (сам токен не хранится); "" — подписка выключена.
    virtual std::string icsTokenHash(long long userId) = 0;
    // tokenHash "" — выключить (NULL). failure — пользователя нет или хэш уже занят.
    virtual DatabaseError setIcsTokenHash(long long userId, const std::string& tokenHash) = 0;
    // 0 — такого токена нет.
    virtual long long findUserByIcsToken(const std::string& tokenHash) = 0;
};

std::unique_ptr<IDatabase> makePostgresDatabase(const std::string& url);
std::unique_ptr<IDatabase> makeEmbeddedDatabase(const std::string& path);

std::string isoNow();  // "2026-09-17T12:00:00Z"

}  // namespace selfvoy
