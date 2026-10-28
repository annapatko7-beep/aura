// selfvoy/server.h — сборка всех модулей и маршрутизация сообщений.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "selfvoy/adminmanager.h"
#include "selfvoy/agentmanager.h"
#include "selfvoy/authmanager.h"
#include "selfvoy/chatmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/connectionmanager.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/listener.h"
#include "selfvoy/memorymanager.h"
#include "selfvoy/protocol.h"
#include "selfvoy/taskmanager.h"
#include "selfvoy/toolmanager.h"
#include "selfvoy/invitemanager.h"
#include "selfvoy/bookingmanager.h"
#include "selfvoy/calendarfeed.h"
#include "selfvoy/payments.h"
#include "selfvoy/referralmanager.h"
#include "selfvoy/schedulelock.h"
#include "selfvoy/telegramadmin.h"
#include "selfvoy/telegrambot.h"

namespace selfvoy {

class IntegrationManager;   // server/src/integrationmanager.h (этап 9)
class NotificationsManager;  // server/src/notificationsmanager.h (этап 13)
class OAuthAuthManager;

class Server {
public:
    using Handler = std::function<Json(std::shared_ptr<Session>, const protocol::Request&)>;

    explicit Server(Config config = Config::fromEnv());
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(std::string& error);
    void stop();
    void wait();               // блокирует поток до stop()
    void requestStop();        // потокобезопасный сигнал остановки

    int port() const { return listener_ ? listener_->port() : config_.port; }
    const Config& config() const { return config_; }

    Json health() const;

    // Доступ к менеджерам (нужен тестам и расширяющим модулям).
    DatabaseManager& database() { return *database_; }
    AuthManager& auth() { return *auth_; }
    ChatManager& chats() { return *chats_; }
    MemoryManager& memory() { return *memory_; }
    ToolManager& tools() { return *tools_; }
    AgentManager& agent() { return *agent_; }
    TaskManager& taskManager() { return *taskManager_; }
    IntegrationManager& integrations() { return *integrations_; }
    NotificationsManager& notifications() { return *notifications_; }
    ConnectionManager& connections() { return connections_; }
    AdminManager& admin() { return *admin_; }
    TelegramBot& telegram() { return *telegram_; }
    TelegramAdmin& telegramAdmin() { return *telegramAdmin_; }  // F7
    BookingManager& bookings() { return *bookings_; }
    PaymentManager& payments() { return *payments_; }  // F4: ЮKassa владельцев
    // Pro-функция доступна пользователю: !SELFVOY_PLANS_ENFORCED или действующий Pro.
    bool proAllowed(long long userId);
    CalendarFeed& calendarFeed() { return *calendarFeed_; }
    ReferralManager& referrals() { return *referrals_; }

    // Один проход планировщика напоминаний (публично для тестов).
    // userId = 0 — все пользователи; иначе только напоминания этого пользователя.
    std::size_t runSchedulerTick(int limit = 100, long long userId = 0);

    // «Выбить» открытые соединения пользователя (событие session.revoked +
    // закрытие сокета). keepJwtId — не трогать соединения этой сессии;
    // onlyJwtId — только соединения этой сессии; exceptSessionId — не
    // трогать данное соединение (тот, кто сделал запрос). Пригодится и для
    // блокировки пользователя администратором.
    std::size_t kickUser(long long userId, const std::string& reason,
                         const std::string& keepJwtId = "", const std::string& onlyJwtId = "",
                         const std::string& exceptSessionId = "");

    // Обработка одного сообщения (публично: тестируется без сокетов).
    Json handleMessage(std::shared_ptr<Session> session, const Json& message);

    void registerHandler(const std::string& type, Handler handler);
    bool hasHandler(const std::string& type) const;
    std::vector<std::string> handlerTypes() const;

private:
    void handleConnection(std::shared_ptr<Session> session);
    void registerHandlers();
    bool authorize(std::shared_ptr<Session> session, const std::string& token, Json& payload);
    // Периодическая сверка авторизованного соединения с БД (отзыв/срок/блокировка).
    bool revalidate(const std::shared_ptr<Session>& session);

    Config config_;
    std::unique_ptr<DatabaseManager> database_;
    std::unique_ptr<AuthManager> auth_;
    std::unique_ptr<OAuthAuthManager> oauthAuth_;
    std::unique_ptr<ChatManager> chats_;
    std::unique_ptr<MemoryManager> memory_;
    std::unique_ptr<ToolManager> tools_;
    std::unique_ptr<AgentManager> agent_;
    std::unique_ptr<TaskManager> taskManager_;
    std::unique_ptr<IntegrationManager> integrations_;
    std::unique_ptr<NotificationsManager> notifications_;
    std::unique_ptr<AdminManager> admin_;
    std::unique_ptr<ScheduleLock> scheduleLock_;
    std::unique_ptr<InviteManager> invites_;
    std::unique_ptr<ReferralManager> referrals_;
    // F7: ИИ-администратор — раньше бота: бот (его поток опроса вызывает администратора)
    // разрушается первым.
    std::unique_ptr<TelegramAdmin> telegramAdmin_;
    // Бот объявлен раньше bookings_: разрушается позже — очередь извещений гостям
    // (поток BookingManager) до конца может ставить сообщения боту.
    std::unique_ptr<TelegramBot> telegram_;
    std::unique_ptr<PaymentManager> payments_;  // раньше bookings_: записи пользуются им до конца
    std::unique_ptr<BookingManager> bookings_;
    std::unique_ptr<CalendarFeed> calendarFeed_;  // ICS-подписка (F6)
    std::unique_ptr<Listener> listener_;
    ConnectionManager connections_;

    mutable std::mutex handlersMutex_;
    std::map<std::string, Handler> handlers_;

    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
    std::atomic<bool> stopping_{false};

    // Фоновый планировщик напоминаний.
    void schedulerLoop();
    std::unique_ptr<std::thread> schedulerThread_;
    std::mutex schedulerMutex_;
    std::condition_variable schedulerCv_;
};

}  // namespace selfvoy
