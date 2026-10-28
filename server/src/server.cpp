// selfvoy/server.cpp
#include "selfvoy/server.h"
#include "selfvoy/net.h"
#include "selfvoy/plan.h"
#include "selfvoy/smtp.h"
#include "selfvoy/tls.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "selfvoy/log.h"
#include "selfvoy/oauthauthmanager.h"
#include "integrationmanager.h"
#include "notificationsmanager.h"

namespace selfvoy {

namespace {

std::string tokenFromSession(const std::shared_ptr<Session>& session) {
    const std::string header = session->authorizationHeader();
    if (!header.empty()) return header;
    return "";
}

long long payloadChatId(const Json& payload) { return payload.getInt("chat_id"); }

// Клиент не должен задавать произвольный размер выдачи: иначе один запрос
// выгружает всю таблицу и раздувает ответ.
int boundedLimit(const Json& payload, const char* key, int fallback, int ceiling) {
    const long long requested = payload.getInt(key, fallback);
    if (requested <= 0) return fallback;
    return static_cast<int>(std::min<long long>(requested, ceiling));
}

// Длина строки в символах UTF-8 (а не в байтах): лимиты профиля — для людей.
std::size_t utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char ch : text) {
        if ((ch & 0xC0) != 0x80) ++count;
    }
    return count;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Аватар: пусто (сброс), https-ссылка или небольшая картинка data:image/*;base64.
// Клиенты ужимают фото до ~256 px, поэтому 150 КБ хватает с запасом, а участники
// чатов, получающие профиль в members[], не скачивают мегабайты.
bool validAvatar(const std::string& value, std::string& error) {
    if (value.empty()) return true;
    if (value.rfind("https://", 0) == 0) {
        if (value.size() > 2048) { error = "ссылка на аватар слишком длинная"; return false; }
        return true;
    }
    static const char* kPrefixes[] = {"data:image/png;base64,", "data:image/jpeg;base64,", "data:image/webp;base64,"};
    for (const char* prefix : kPrefixes) {
        if (value.rfind(prefix, 0) == 0) {
            if (value.size() > 150000) { error = "аватар слишком большой (до ~100 КБ)"; return false; }
            return true;
        }
    }
    error = "аватар — https-ссылка или картинка PNG/JPEG/WebP";
    return false;
}

// Админ-панель: роль admin для адреса из SELFVOY_ADMIN_EMAILS выдаётся при
// успешном входе / подтверждении email. Если роль изменилась, в ответе
// (payload.user) отдаётся уже обновлённая запись.
Json withBootstrappedRole(AdminManager& admin, IDatabase& db, long long userId, Json payload,
                          const std::string& remoteAddr) {
    if (userId <= 0) return payload;
    const auto user = db.findUserById(userId);
    if (!user) return payload;
    admin.bootstrap(*user, remoteAddr);
    if (const auto updated = db.findUserById(userId)) {
        if (updated->role != user->role && payload.isObject() && payload.contains("user")) {
            payload.set("user", updated->toJson());
        }
    }
    return payload;
}

// Закрыть все открытые соединения пользователей (блокировка, отзыв сессий
// администратором). Код 4001, причина — машинный ключ для клиента.
void kickUsers(ConnectionManager& connections, const std::vector<long long>& userIds, const std::string& reason) {
    for (const long long userId : userIds) {
        for (const auto& session : connections.forUser(userId)) {
            session->close(4001, reason);
            connections.remove(session->id());
        }
    }
}

// Сколько разных пользователей сейчас онлайн (авторизованные живые соединения).
std::size_t onlineUsers(const ConnectionManager& connections) {
    std::vector<long long> ids;
    for (const auto& item : connections.describe()) {
        const long long userId = item.getInt("user_id");
        if (userId > 0 && item.getBool("alive")) ids.push_back(userId);
    }
    std::sort(ids.begin(), ids.end());
    return static_cast<std::size_t>(std::unique(ids.begin(), ids.end()) - ids.begin());
}

}  // namespace

Server::Server(Config config) : config_(std::move(config)) {
    log::setLevel(config_.logLevel);
}

Server::~Server() { stop(); }

bool Server::start(std::string& error) {
    // Небезопасная конфигурация (dev-почта, слабый JWT-секрет, нет ключа 2FA)
    // вне SELFVOY_DEV_MODE=1 — отказ стартовать с понятным сообщением.
    std::vector<std::string> warnings;
    if (!config_.validate(error, warnings)) return false;
    for (const auto& warning : warnings) SELFVOY_LOG(log::Level::Warn, "server") << warning;
    plan::setEnforced(config_.plansEnforced);
    net::setCaFile(config_.caFile);
    // С OpenSSL https-адреса Google работают напрямую. Без него (сборка без TLS)
    // — только через TLS-прокси (egress), которому сервер шлёт обычный http.
    if (!config_.googleClientId.empty() && !tls::available()) {
        for (const std::string* url : {&config_.googleAuthUrl, &config_.googleTokenUrl, &config_.googleRevokeUrl,
                                       &config_.googleCalendarUrl, &config_.googleGmailUrl}) {
            if (url->rfind("https://", 0) == 0 && url != &config_.googleAuthUrl) {
                SELFVOY_LOG(log::Level::Warn, "server")
                    << "интеграции Google: " << *url
                    << " — https, а сервер собран без OpenSSL (без TLS). "
                       "Задайте SELFVOY_GOOGLE_TOKEN_URL/REVOKE_URL/CALENDAR_URL/GMAIL_URL на http-адрес "
                       "TLS-прокси (egress), иначе обмен кода и синхронизация не заработают";
                break;
            }
        }
    }
    if (config_.mailDriver == "smtp") {
        if (config_.smtpHost.empty() || smtp::addressOf(config_.smtpFrom).find('@') == std::string::npos) {
            error = "SELFVOY_MAIL_DRIVER=smtp: задайте SELFVOY_SMTP_HOST и SELFVOY_SMTP_FROM (например «Selfvoy <no-reply@домен>»)";
            return false;
        }
        if (config_.smtpTls != "none" && !smtp::tlsAvailable()) {
            error = "сервер собран без OpenSSL — для SMTP с TLS пересоберите с libssl-dev";
            return false;
        }
    }
    database_ = DatabaseManager::fromConfig(config_, error);
    if (!database_) return false;

    auth_ = std::make_unique<AuthManager>(*database_, config_);
    oauthAuth_ = std::make_unique<OAuthAuthManager>(*database_, *auth_, config_);
    chats_ = std::make_unique<ChatManager>(*database_, connections_);
    memory_ = std::make_unique<MemoryManager>(*database_, config_);
    integrations_ = std::make_unique<IntegrationManager>(*database_, config_);
    notifications_ = std::make_unique<NotificationsManager>(*database_, connections_, config_);
    tools_ = std::make_unique<ToolManager>(config_, *database_, *chats_, integrations_.get());
    agent_ = std::make_unique<AgentManager>(config_, *database_, *memory_, *tools_, *chats_);
    taskManager_ = std::make_unique<TaskManager>(*database_, connections_);
    admin_ = std::make_unique<AdminManager>(*database_, config_);
    referrals_ = std::make_unique<ReferralManager>(*database_, notifications_.get(), config_);
    scheduleLock_ = std::make_unique<ScheduleLock>(*database_);
    invites_ = std::make_unique<InviteManager>(*database_, *memory_, notifications_.get(), config_, referrals_.get(),
                                               scheduleLock_.get());
    bookings_ = std::make_unique<BookingManager>(*database_, *memory_, notifications_.get(), *referrals_,
                                                 *scheduleLock_, config_);
    // Telegram-бот (F2): без токена и имени — выключен (ссылки пустые, отправка — no-op).
    telegram_ = std::make_unique<TelegramBot>(*database_, config_);
    telegram_->setBookings(bookings_.get());
    bookings_->setTelegram(telegram_.get());
    // Оплата записи (F4): ЮKassa владельца (SELFVOY_YOOKASSA_API_URL) или оплата по ссылке.
    payments_ = std::make_unique<PaymentManager>(*database_, config_);
    bookings_->setPayments(payments_.get());
    if (config_.yookassaApiUrl.rfind("https://", 0) == 0 && !tls::available()) {
        SELFVOY_LOG(log::Level::Warn, "server") << "ЮKassa: SELFVOY_YOOKASSA_API_URL — https, а сервер собран без "
                                                   "OpenSSL — онлайн-оплата работать не будет";
    }
    notifications_->setTelegram(telegram_.get());
    // F7: ИИ-администратор страниц записи в Telegram (бот + путь booking.book + уведомления).
    telegramAdmin_ = std::make_unique<TelegramAdmin>(*database_, config_, *telegram_, *bookings_, notifications_.get());
    telegram_->setAdmin(telegramAdmin_.get());
    if (config_.telegramEnabled() && config_.telegramApiUrl.rfind("https://", 0) == 0 && !tls::available()) {
        SELFVOY_LOG(log::Level::Warn, "server") << "Telegram-бот: SELFVOY_TELEGRAM_API_URL — https, а сервер собран "
                                                   "без OpenSSL — бот работать не будет";
    }
    calendarFeed_ = std::make_unique<CalendarFeed>(*database_, config_);
    admin_->bootstrapAll();

    registerHandlers();

    listener_ = std::make_unique<Listener>(
        config_, [this](std::shared_ptr<Session> session) { handleConnection(std::move(session)); });
    if (!listener_->start(error)) return false;

    SELFVOY_LOG(log::Level::Info, "server") << "сервер Selfvoy запущен: " << config_.describe();
    std::string aiError;
    if (agent_->available(aiError)) {
        SELFVOY_LOG(log::Level::Info, "server") << "AI-сервис доступен: " << config_.aiServiceUrl;
    } else {
        SELFVOY_LOG(log::Level::Warn, "server")
            << "AI-сервис не отвечает (" << config_.aiServiceUrl << "): агент вернёт ошибку upstream_error";
    }

    // Фоновый планировщик напоминаний (0 = выключен, например в тестах).
    if (config_.schedulerIntervalMs > 0 && !schedulerThread_) {
        schedulerThread_ = std::make_unique<std::thread>(&Server::schedulerLoop, this);
    }
    telegram_->start();  // опрос getUpdates (если бот включён и SELFVOY_TELEGRAM_POLL != 0)
    return true;
}

bool Server::proAllowed(long long userId) { return database_ && plan::proAllowed(database_->db(), userId); }

void Server::schedulerLoop() {
    while (!stopping_.load()) {
        {
            std::unique_lock<std::mutex> lock(schedulerMutex_);
            schedulerCv_.wait_for(lock, std::chrono::milliseconds(config_.schedulerIntervalMs),
                                  [this] { return stopping_.load(); });
        }
        if (stopping_.load()) break;
        // Разбираем накопившиеся напоминания до конца, а не по 100 за интервал:
        // утренний пик в тысячи напоминаний иначе отставал бы на десятки минут.
        for (int batch = 0; batch < 100 && !stopping_.load(); ++batch) {
            if (runSchedulerTick(100) < 100) break;
        }
        // F1: напоминания гостям о встречах (атомарный захват — один экземпляр на строку).
        if (bookings_) {
            for (int batch = 0; batch < 100 && !stopping_.load(); ++batch) {
                if (bookings_->reminderTick(100, std::time(nullptr)) < 100) break;
            }
            // F4: ожидающие оплаты — опрос ЮKassa (не чаще раза в 30 с на платёж) и отмена
            // неоплаченных к сроку записей.
            if (!stopping_.load()) bookings_->paymentTick(500, std::time(nullptr));
            // F5: оплата пакетов (опрос ЮKassa, истечение неоплаченных) и срок действия.
            if (!stopping_.load()) bookings_->packageTick(500, std::time(nullptr));
            // F8: истёкшие предложения листа ожидания (время — следующему) и записи листа
            // старше 30 дней; «пора записаться снова» (атомарный захват — один экземпляр на запись).
            if (!stopping_.load()) bookings_->waitlistTick(500, std::time(nullptr));
            for (int batch = 0; batch < 20 && !stopping_.load(); ++batch) {
                if (bookings_->rebookTick(100, std::time(nullptr)) < 100) break;
            }
        }
    }
}

std::size_t Server::runSchedulerTick(int limit, long long userId) {
    if (!taskManager_) return 0;
    const auto due = taskManager_->dueTick(limit, userId);
    // Этап 13: каждое сработавшее напоминание — ещё и уведомление (in-app + push).
    if (notifications_) {
        for (const auto& task : due) {
            Json payload = Json::object();
            payload.set("task_id", Json(task.id));
            notifications_->create(task.userId, "task.due", "Напоминание", task.title, payload);
        }
    }
    return due.size();
}

void Server::stop() {
    const bool already = stopping_.exchange(true);
    if (listener_) listener_->stop();
    connections_.closeAll(1001, "server shutdown");
    // Рабочие потоки соединений вызывают обработчики Server — дожидаемся их,
    // иначе они переживут Server (ASan: use-after-free в Listener::releaseIpSlot).
    if (listener_ && !listener_->drain(std::chrono::seconds(10))) {
        SELFVOY_LOG(log::Level::Warn, "server") << "не все соединения завершились за 10 с";
    }
    // Останавливаем фоновый планировщик и дожидаемся его.
    schedulerCv_.notify_all();
    if (schedulerThread_ && schedulerThread_->joinable()) schedulerThread_->join();
    schedulerThread_.reset();
    // Извещения гостям из очереди ставят сообщения боту — сначала они, потом бот
    // (опрос прерывается сразу, очередь отправки дописывается не дольше ~5 с).
    if (bookings_) bookings_->flushMail();
    if (telegram_) telegram_->stop();
    if (!already) waitCondition_.notify_all();
}

void Server::requestStop() {
    stopping_.store(true);
    waitCondition_.notify_all();
}

void Server::wait() {
    std::unique_lock<std::mutex> lock(waitMutex_);
    waitCondition_.wait(lock, [this] { return stopping_.load(); });
}

Json Server::health() const {
    Json json = Json::object();
    json.set("status", Json("ok"));
    json.set("service", Json("selfvoy-server"));
    json.set("database", Json(database_ ? database_->backend() : std::string("none")));
    json.set("database_healthy", Json(database_ ? database_->healthy() : false));
    json.set("connections", connections_.stats());
    json.set("port", Json(static_cast<long long>(port())));
    json.set("ai_service", Json(config_.aiServiceUrl));
    if (database_) {
        const auto stats = database_->stats();
        Json dbStats = Json::object();
        dbStats.set("queries", Json(static_cast<long long>(stats.queries)));
        dbStats.set("failures", Json(static_cast<long long>(stats.failures)));
        json.set("database_stats", dbStats);
    }
    return json;
}

bool Server::authorize(std::shared_ptr<Session> session, const std::string& token, Json& payload) {
    if (session->authenticated()) return true;
    if (token.empty()) return false;

    const AuthManager::Result result = auth_->verifyToken(token);
    if (!result.ok) return false;
    // Заблокированный пользователь (admin.setActive) не входит и старым токеном.
    if (!result.payload.get("user").getBool("is_active", true)) return false;
    session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
    connections_.add(session);
    payload = result.payload;
    return true;
}

std::size_t Server::kickUser(long long userId, const std::string& reason, const std::string& keepJwtId,
                             const std::string& onlyJwtId, const std::string& exceptSessionId) {
    return connections_.kickUser(userId, reason, [&](const Session& candidate) {
        if (!exceptSessionId.empty() && candidate.id() == exceptSessionId) return false;
        const std::string jwt = candidate.jwtId();
        if (!keepJwtId.empty() && jwt == keepJwtId) return false;
        if (!onlyJwtId.empty() && jwt != onlyJwtId) return false;
        return true;
    });
}

bool Server::revalidate(const std::shared_ptr<Session>& session) {
    if (!session->authenticated()) return true;
    const auto age = std::chrono::steady_clock::now() - session->lastAuthCheck();
    if (config_.authRecheckSec > 0 && age < std::chrono::seconds(config_.authRecheckSec)) return true;
    const AuthManager::Result check = auth_->checkSession(session->userId(), session->jwtId());
    if (check.ok) {
        session->markAuthChecked();
        return true;
    }
    SELFVOY_LOG(log::Level::Info, "server") << "сессия " << session->id() << " больше не действительна: "
                                         << check.message;
    session->resetAuth();
    connections_.remove(session->id());
    return false;
}

void Server::handleConnection(std::shared_ptr<Session> session) {
    // Обычные HTTP-запросы на порт WebSocket: проверка живости и ICS-подписка (F6).
    session->setHttpHandler([this](const std::string& method, const std::string& target, const std::string& ip) {
        if (target == "/healthz" || target.rfind("/healthz?", 0) == 0) {
            if (method != "GET") {
                return httpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed\n",
                                    "Cache-Control: no-store\r\nAllow: GET\r\n");
            }
            // Сводная проверка: порт жив сам по себе, но для k8s/мониторинга важно
            // знать, отвечают ли БД и AI-сервис — без деталей (адреса, статистика
            // и бэкенды посторонним не нужны), только факт и короткая причина сбоя.
            const bool dbOk = database_ && database_->healthy();
            std::string aiError;
            const bool aiOk = agent_ && agent_->available(aiError);
            Json health = Json::object();
            health.set("status", dbOk && aiOk ? "ok" : "degraded");
            health.set("db", dbOk);
            health.set("ai", aiOk);
            if (!aiOk) health.set("ai_error", aiError);
            const int code = dbOk && aiOk ? 200 : 503;
            const std::string reason = code == 200 ? "OK" : "Service Unavailable";
            return httpResponse(code, reason, "application/json", health.dump(),
                                "Cache-Control: no-store\r\n");
        }
        if (target.rfind("/auth/oauth/callback", 0) == 0) {
            static const std::string kHtml =
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Selfvoy Auth</title></head>"
                "<body style=\"font-family:sans-serif;text-align:center;padding:50px;background:#09080e;color:#fff;\">"
                "<h3>Авторизация завершена</h3><p>Окно закрывается, возвращаем вас в Selfvoy...</p>"
                "<script>"
                "const p = new URLSearchParams(window.location.search);"
                "const d = { type: 'selfvoy:oauth', provider: p.get('provider')||'', code: p.get('code')||'', state: p.get('state')||'' };"
                "if (window.opener) { window.opener.postMessage(d, '*'); setTimeout(() => window.close(), 500); }"
                "else { window.location.href = '/#/oauth/callback?' + p.toString(); }"
                "</script></body></html>";
            return httpResponse(200, "OK", "text/html; charset=utf-8", kHtml, "Cache-Control: no-store\r\n");
        }
        return calendarFeed_ ? calendarFeed_->serveHttp(method, target, ip) : std::string();
    });
    std::string error;
    if (!session->performHandshake(error)) {
        if (session->servedHttp()) {
            SELFVOY_LOG(log::Level::Debug, "server") << "HTTP-запрос обслужен: " << error;
        } else {
            SELFVOY_LOG(log::Level::Warn, "server") << "рукопожатие не удалось: " << error;
        }
        return;
    }

    // Токен можно передать сразу в заголовке рукопожатия.
    Json authPayload;
    if (authorize(session, tokenFromSession(session), authPayload)) {
        SELFVOY_LOG(log::Level::Info, "server")
            << "сессия " << session->id() << " авторизована как " << session->email();
        Json welcome = protocol::event("session.ready", session->describe());
        welcome.set("auth", authPayload);
        session->send(welcome);
    }

    session->readLoop([this](std::shared_ptr<Session> active, const Json& message) {
        Json response = handleMessage(active, message);
        if (response.isNull()) return;
        // id возвращается в том же виде, в каком пришёл: число остаётся числом
        // (docs/PROTOCOL.md: «id»: int), строка — строкой.
        if (message.isObject() && message.get("id").isNumber() && response.isObject()) {
            response.set("id", message.get("id"));
        }
        active->send(response);
    });

    connections_.remove(session->id());
    if (session->authenticated()) database_->db().touchUser(session->userId());
    SELFVOY_LOG(log::Level::Info, "server") << "сессия " << session->id() << " завершена";
}

Json Server::handleMessage(std::shared_ptr<Session> session, const Json& message) {
    protocol::Request request;
    std::string parseError;
    if (!protocol::parse(message, request, parseError)) {
        return protocol::error("", protocol::code::kBadRequest, parseError);
    }

    Handler handler;
    {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        const auto it = handlers_.find(request.type);
        if (it != handlers_.end()) handler = it->second;
    }
    if (!handler) {
        return protocol::error(request.id, protocol::code::kBadRequest,
                               "неизвестный тип сообщения: " + request.type);
    }

    const bool guestCall = !session->authenticated() &&
                           (request.type.rfind("invite.", 0) == 0 || request.type.rfind("booking.", 0) == 0 ||
                            request.type.rfind("package.", 0) == 0) &&
                           protocol::isPublicType(request.type);
    if (!protocol::isPublicType(request.type)) {
        // Открытое соединение не живёт дольше своей сессии: отзыв, истечение
        // срока или блокировка аккаунта видны не позже чем через SELFVOY_AUTH_RECHECK.
        const bool expired = session->authenticated() && !revalidate(session);
        if (!session->authenticated()) {
            // Новый токен в payload (например, auth.token после refresh) — пробуем им.
            Json payload;
            const std::string token = request.payload.getString("token", tokenFromSession(session));
            if (!authorize(session, token, payload)) {
                return protocol::error(request.id, protocol::code::kUnauthorized,
                                       expired ? "сессия истекла или отозвана — войдите снова" : "требуется вход");
            }
        }
    }

    try {
        Json response = handler(session, request);
        if (guestCall && response.getString("type") == "ok") session->markGuest();
        return response;
    } catch (const std::exception& exception) {
        // Текст исключения (SQL, пути, внутренности) — только в лог.
        SELFVOY_LOG(log::Level::Error, "server")
            << "исключение в обработчике " << request.type << ": " << exception.what();
        return protocol::error(request.id, protocol::code::kInternal, "внутренняя ошибка сервера");
    } catch (...) {
        SELFVOY_LOG(log::Level::Error, "server") << "неизвестное исключение в обработчике " << request.type;
        return protocol::error(request.id, protocol::code::kInternal, "внутренняя ошибка сервера");
    }
}

void Server::registerHandler(const std::string& type, Handler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    handlers_[type] = std::move(handler);
}

bool Server::hasHandler(const std::string& type) const {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    return handlers_.find(type) != handlers_.end();
}

std::vector<std::string> Server::handlerTypes() const {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    std::vector<std::string> types;
    types.reserve(handlers_.size());
    for (const auto& entry : handlers_) types.push_back(entry.first);
    return types;
}

void Server::registerHandlers() {
    // ------------------------------------------------------------- сервис
    registerHandler("ping", [](std::shared_ptr<Session>, const protocol::Request& request) {
        Json payload = Json::object();
        payload.set("pong", Json(true));
        payload.set("server_time", Json(isoNow()));
        return protocol::ok(request.id, payload);
    });

    registerHandler("server.info", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        // До авторизации — только факт работы сервиса: адрес AI-сервиса, список
        // обработчиков и статистика БД посторонним не нужны.
        if (!session->authenticated()) {
            Json payload = Json::object();
            payload.set("status", Json("ok"));
            payload.set("service", Json("selfvoy-server"));
            payload.set("version", Json("0.1"));
            payload.set("telegram_bot", Json(telegram_ ? telegram_->username() : std::string()));
            return protocol::ok(request.id, payload);
        }
        Json payload = health();
        payload.set("telegram_bot", Json(telegram_ ? telegram_->username() : std::string()));
        Json types = Json::array();
        for (const auto& type : handlerTypes()) types.push(Json(type));
        payload.set("handlers", types);
        return protocol::ok(request.id, payload);
    });

    // --------------------------------------------------------------- auth
    // Регистрация больше не выдаёт сессию: сначала подтверждение email
    // (auth.verifyEmail), затем вход (auth.login).
    registerHandler("auth.register",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->registerUser(request.payload.getString("email"),
                                                                request.payload.getString("password"),
                                                                request.payload.getString("display_name"),
                                                                request.payload.getString("device", "qt-client"),
                                                                session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        // Регистрация по реферальной ссылке: неверный код молча игнорируется.
                        if (!request.payload.getString("ref").empty()) {
                            referrals_->attach(result.payload.get("user").getInt("id"),
                                               request.payload.getString("ref"),
                                               request.payload.getString("ref_source"));
                        }
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.oauth.providers",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        (void)session;
                        Json res = Json::object();
                        res.set("providers", oauthAuth_->providerCatalog());
                        return protocol::ok(request.id, res);
                    });

    registerHandler("auth.oauth.begin",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        (void)session;
                        const std::string provider = request.payload.getString("provider");
                        const std::string redirectUri = request.payload.getString("redirect_uri");
                        const auto result = oauthAuth_->begin(provider, redirectUri);
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.oauth.callback",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const std::string provider = request.payload.getString("provider");
                        const std::string code = request.payload.getString("code");
                        const std::string state = request.payload.getString("state");
                        const std::string device = request.payload.getString("device", "web");
                        const std::string deviceId = request.payload.getString("device_id");
                        const auto result = oauthAuth_->callback(provider, code, state, device, deviceId, session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        if (result.payload.contains("user")) {
                            const long long userId = result.payload.get("user").getInt("id");
                            referrals_->verified(userId);
                            const auto boosted = withBootstrappedRole(*admin_, database_->db(), userId, result.payload, session->remoteAddr());
                            session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
                            connections_.add(session);
                            return protocol::ok(request.id, boosted);
                        }
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.verifyEmail",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->verifyEmail(request.payload.getString("email"),
                                                               request.payload.getString("code"),
                                                               session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        // Приглашённый засчитывается пригласившему после подтверждения email.
                        referrals_->verified(result.payload.get("user").getInt("id"));
                        return protocol::ok(request.id,
                                            withBootstrappedRole(*admin_, database_->db(),
                                                                 result.payload.get("user").getInt("id"),
                                                                 result.payload, session->remoteAddr()));
                    });

    registerHandler("auth.resendCode",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->resendVerification(request.payload.getString("email"),
                                                                      session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.forgotPassword",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->forgotPassword(request.payload.getString("email"),
                                                                  session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.resetPassword",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->resetPassword(request.payload.getString("email"),
                                                                 request.payload.getString("code"),
                                                                 request.payload.getString("new_password"),
                                                                 session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        // Все сессии отозваны: открытые соединения этого пользователя
                        // закрываются (запросившее — просто разлогинивается).
                        if (session->userId() == result.userId) {
                            session->resetAuth();
                            connections_.remove(session->id());
                        }
                        kickUser(result.userId, "password_reset", "", "", session->id());
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.refresh",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->refresh(request.payload.getString("refresh_token"),
                                                           request.payload.getString("device", "qt-client"),
                                                           session->remoteAddr());
                        if (!result.ok) {
                            // Повторное использование refresh-токена: все сессии отозваны —
                            // закрываем и открытые соединения пользователя.
                            if (result.sessionsRevoked) {
                                if (session->userId() == result.userId) {
                                    session->resetAuth();
                                    connections_.remove(session->id());
                                }
                                kickUser(result.userId, "refresh_reuse", "", "", session->id());
                            }
                            return protocol::error(request.id, result.code, result.message);
                        }
                        // Как после auth.login: соединение работает под новой сессией.
                        // Иначе клиент, продливший истёкший токен, оставался неавторизованным
                        // до переподключения, и повторные prefs.get/chat.list получали отказ.
                        session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
                        connections_.add(session);
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.changePassword",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->changePassword(session->userId(),
                                                                  session->jwtId(),
                                                                  request.payload.getString("old_password"),
                                                                  request.payload.getString("new_password"),
                                                                  session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        // Текущее соединение продолжает работать под новой сессией (новая
                        // пара токенов в ответе), все прочие соединения закрываются.
                        session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
                        connections_.add(session);
                        kickUser(result.userId, "password_changed", result.jwtId, "", session->id());
                        return protocol::ok(request.id, result.payload);
                    });

    // Управление активными сессиями.
    registerHandler("sessions.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->listSessions(session->userId(), session->jwtId());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("sessions.revoke", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const std::string targetId = request.payload.getString("session_id");
        // Запоминаем id текущей БД-сессии до отзыва: findSessionByJwt
        // возвращает только живые сессии.
        const auto current = database_->db().findSessionByJwt(session->jwtId());
        const long long userId = session->userId();
        const auto result = auth_->revokeSession(userId, targetId);
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        // Если отозвали собственную сессию — разлогиниваем соединение.
        if (current && current->id == targetId) {
            session->resetAuth();
            connections_.remove(session->id());
        }
        // Открытые соединения отозванной сессии закрываются сразу.
        if (!result.jwtId.empty()) kickUser(userId, "session_revoked", "", result.jwtId, session->id());
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("sessions.revokeAll", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->revokeAllSessions(session->userId(), session->jwtId());
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        kickUser(session->userId(), "sessions_revoked", session->jwtId(), "", session->id());
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("auth.login",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->login(request.payload.getString("email"),
                                                         request.payload.getString("password"),
                                                         request.payload.getString("device", "qt-client"),
                                                         request.payload.getString("device_id"),
                                                         session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
                        connections_.add(session);
                        // Этап 13: уведомление о новом входе (безопасность).
                        if (notifications_) {
                            Json payload = Json::object();
                            payload.set("device", Json(request.payload.getString("device", "qt-client")));
                            notifications_->create(result.userId, "login.new", "Новый вход в Selfvoy",
                                                   session->remoteAddr(), payload);
                        }
                        return protocol::ok(request.id,
                                            withBootstrappedRole(*admin_, database_->db(), result.userId,
                                                                 result.payload, session->remoteAddr()));
                    });

    // Завершение входа с 2FA: пароль + TOTP-код (или резервный код).
    registerHandler("auth.login2fa",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->login2fa(
                            request.payload.getString("email"),
                            request.payload.getString("password"),
                            request.payload.getString("code"),
                            request.payload.getBool("trust_device", false),
                            request.payload.getString("device_id"),
                            request.payload.getString("device", "qt-client"),
                            session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
                        connections_.add(session);
                        // Этап 13: вход через 2FA — тоже уведомление о входе.
                        if (notifications_) {
                            Json payload = Json::object();
                            payload.set("device", Json(request.payload.getString("device", "qt-client")));
                            payload.set("two_factor", Json(true));
                            notifications_->create(result.userId, "login.new", "Новый вход в Selfvoy (2FA)",
                                                   session->remoteAddr(), payload);
                        }
                        return protocol::ok(request.id,
                                            withBootstrappedRole(*admin_, database_->db(), result.userId,
                                                                 result.payload, session->remoteAddr()));
                    });

    // Настройка 2FA: создать секрет → подтвердить кодом → (опц.) отключить.
    registerHandler("auth.setup2fa", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->setup2fa(session->userId(), session->remoteAddr());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("auth.confirm2fa", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->confirm2fa(session->userId(), request.payload.getString("code"),
                                              session->remoteAddr());
        // Этап 13: включение 2FA — уведомление безопасности.
        if (result.ok && notifications_) {
            notifications_->create(session->userId(), "twofactor.enabled",
                                   "Двухфакторная аутентификация включена",
                                   "Вход теперь требует код из приложения-аутентификатора");
        }
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("auth.disable2fa", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->disable2fa(session->userId(), request.payload.getString("password"),
                                              session->remoteAddr());
        // Этап 13: отключение 2FA — обязательно уведомляем владельца.
        if (result.ok && notifications_) {
            notifications_->create(session->userId(), "twofactor.disabled",
                                   "Двухфакторная аутентификация отключена",
                                   "Если это были не вы — смените пароль");
        }
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("auth.status2fa", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->twoFactorStatus(session->userId());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // Доверенные устройства.
    registerHandler("devices.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->listTrustedDevices(session->userId());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("devices.revoke", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->revokeTrustedDevice(session->userId(), request.payload.getString("id"),
                                                       session->remoteAddr());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("devices.revokeAll", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->revokeAllTrustedDevices(session->userId(), session->remoteAddr());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("auth.token",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        Json payload;
                        const std::string token = request.payload.getString("token");
                        if (!authorize(session, token, payload)) {
                            return protocol::error(request.id, protocol::code::kUnauthorized, "токен недействителен");
                        }
                        // Сессию уже авторизовал этим же токеном общий обработчик (auth.token
                        // не публичный метод) — authorize() тогда отдаёт пустой payload.
                        // По протоколу ответ содержит user: дополняем.
                        if (!payload.contains("user")) {
                            if (const auto user = database_->db().findUserById(session->userId())) {
                                if (!payload.isObject()) payload = Json::object();
                                payload.set("user", user->toJson());
                            }
                        }
                        return protocol::ok(request.id, payload);
                    });

    registerHandler("auth.me", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto user = database_->db().findUserById(session->userId());
        if (!user) return protocol::error(request.id, protocol::code::kNotFound, "пользователь не найден");
        Json payload = user->toJson();
        payload.set("online", Json(true));
        payload.set("preferences", memory_->preferences(session->userId()));
        return protocol::ok(request.id, payload);
    });

    // Профиль: имя, «о себе», аватар. Меняются только переданные поля.
    registerHandler("profile.update", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        Json fields = Json::object();
        if (request.payload.contains("display_name")) {
            const std::string name = trimmed(request.payload.getString("display_name"));
            if (name.empty() || utf8Length(name) > 80) {
                return protocol::error(request.id, protocol::code::kBadRequest, "имя — от 1 до 80 символов");
            }
            fields.set("display_name", Json(name));
        }
        if (request.payload.contains("bio")) {
            const std::string bio = trimmed(request.payload.getString("bio"));
            if (utf8Length(bio) > 300) {
                return protocol::error(request.id, protocol::code::kBadRequest, "«о себе» — до 300 символов");
            }
            fields.set("bio", Json(bio));
        }
        if (request.payload.contains("avatar_url")) {
            const std::string avatar = request.payload.getString("avatar_url");
            std::string error;
            if (!validAvatar(avatar, error)) return protocol::error(request.id, protocol::code::kBadRequest, error);
            fields.set("avatar_url", Json(avatar));
        }
        if (fields.members().empty()) {
            return protocol::error(request.id, protocol::code::kBadRequest, "нет полей профиля для изменения");
        }
        const DatabaseError stored = database_->db().updateProfile(session->userId(), fields);
        if (!stored.ok) return protocol::error(request.id, protocol::code::kInternal, stored.message);
        const auto user = database_->db().findUserById(session->userId());
        if (!user) return protocol::error(request.id, protocol::code::kNotFound, "пользователь не найден");
        Json payload = Json::object();
        payload.set("user", user->toJson());
        return protocol::ok(request.id, payload);
    });

    registerHandler("auth.logout", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const long long userId = session->userId();
        const std::string jwtId = session->jwtId();
        const auto result = auth_->logout(jwtId);
        session->resetAuth();
        connections_.remove(session->id());
        // Тот же токен мог быть предъявлен на других сокетах — закрываем и их.
        kickUser(userId, "logout", "", jwtId, session->id());
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("auth.deleteAccount", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const long long userId = session->userId();
        if (userId <= 0) {
            return protocol::error(request.id, "unauthorized", "Требуется авторизация");
        }
        const std::string jwtId = session->jwtId();
        auth_->logout(jwtId);
        const auto err = database_->db().deleteUser(userId);
        if (!err.ok) {
            return protocol::error(request.id, "delete_failed", err.message);
        }
        session->resetAuth();
        connections_.remove(session->id());
        kickUser(userId, "account_deleted", "Аккаунт был удален пользователем", "", session->id());
        Json payload = Json::object();
        payload.set("deleted", Json(true));
        return protocol::ok(request.id, payload);
    });

    // --------------------------------------------------------------- chat
    registerHandler("chat.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = chats_->list(session->userId(), boundedLimit(request.payload, "limit", 50, 200));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("chat.open", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = chats_->open(session->userId(), request.payload.getString("contact"),
                                         request.payload.getString("title"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("chat.history", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = chats_->history(session->userId(), payloadChatId(request.payload),
                                            request.payload.getInt("before_id"),
                                            boundedLimit(request.payload, "limit", 50, 200));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("chat.members", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = chats_->members(session->userId(), payloadChatId(request.payload));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("chat.read", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = chats_->markRead(session->userId(), payloadChatId(request.payload));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("chat.send", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        // Клиент шлёт только обычный текст: kind agent_*/system ставит сервер
        // (Selfvoy, подтверждённые действия), иначе их можно подделать.
        const std::string kind = request.payload.getString("kind", "text");
        if (kind != "text") {
            return protocol::error(request.id, protocol::code::kBadRequest,
                                   "клиент может отправлять только сообщения kind=text");
        }
        const std::string body = request.payload.getString("body");
        if (utf8Length(body) > 8000) {
            return protocol::error(request.id, protocol::code::kBadRequest, "сообщение длиннее 8000 символов");
        }
        Json extra = request.payload.get("payload");
        if (!extra.isNull() && !extra.isObject()) {
            return protocol::error(request.id, protocol::code::kBadRequest, "payload должен быть объектом");
        }
        if (extra.isObject()) {
            // Служебные поля Selfvoy (origin, proposal, confirmed, tool) — только от сервера.
            for (const char* reserved : {"origin", "proposal", "confirmed", "tool"}) {
                if (extra.contains(reserved)) {
                    return protocol::error(request.id, protocol::code::kBadRequest,
                                           std::string("поле payload.") + reserved + " зарезервировано сервером");
                }
            }
            if (extra.dump().size() > 4096) {
                return protocol::error(request.id, protocol::code::kBadRequest, "payload больше 4 КБ");
            }
        }
        const auto result = chats_->send(session->userId(), payloadChatId(request.payload), body, "text", extra);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("users.search", [this](std::shared_ptr<Session>, const protocol::Request& request) {
        const auto result = chats_->searchUsers(request.payload.getString("query"),
                                                boundedLimit(request.payload, "limit", 20, 50));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // -------------------------------------------------------------- agent
    registerHandler("agent.ask", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = agent_->ask(session->userId(), payloadChatId(request.payload),
                                        request.payload.getString("message"), request.payload.get("peers"),
                                        request.payload.getBool("execute", true));
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        // Этап 13: действие, ожидающее подтверждения, — ещё и уведомление
        // (in-app + push): пользователь увидит его, даже если закрыл клиент.
        if (notifications_) {
            const Json results = result.payload.get("results");
            for (const auto& item : results.items()) {
                if (!item.getBool("requires_confirmation", false)) continue;
                Json notifyPayload = Json::object();
                notifyPayload.set("confirmation_id", Json(item.getInt("confirmation_id")));
                notifyPayload.set("tool", Json(item.getString("tool")));
                notifications_->create(session->userId(), "confirmation.requested",
                                       "Нужно подтверждение",
                                       item.getString("summary", "Опасная операция ожидает решения"),
                                       notifyPayload);
                break;  // одно уведомление на запрос
            }
        }
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("agent.negotiate", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const std::string target = request.payload.getString("target");
        std::optional<UserRecord> peer;
        if (target.find('@') != std::string::npos && target.find(':') == std::string::npos) {
            peer = database_->db().findUserByEmail(target);
        } else if (target.find(':') == std::string::npos) {
            const auto candidates = database_->db().searchUsers(target, 5);
            for (const auto& candidate : candidates) {
                if (candidate.id != session->userId()) {
                    peer = candidate;
                    break;
                }
            }
        }
        if (!peer) {
            // Если указан федеративный адрес Aura (@username:domain.org):
            if (target.find(':') != std::string::npos || (!target.empty() && target.front() == '@')) {
                const auto fedResult = agent_->negotiateFederated(session->userId(), target,
                                                                  request.payload.getString("topic", "встреча"),
                                                                  static_cast<int>(request.payload.getInt("duration_minutes", 60)),
                                                                  static_cast<int>(request.payload.getInt("window_hours", 96)));
                if (!fedResult.ok) return protocol::error(request.id, fedResult.code, fedResult.message);
                return protocol::ok(request.id, fedResult.payload);
            }
            return protocol::error(request.id, protocol::code::kNotFound, "собеседник не найден");
        }

        const auto result = agent_->negotiate(session->userId(), peer->id,
                                              request.payload.getString("topic", "встреча"),
                                              static_cast<int>(request.payload.getInt("duration_minutes", 60)),
                                              static_cast<int>(request.payload.getInt("window_hours", 96)));
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        // Этап 13: собеседник узнаёт о переговорах даже офлайн.
        if (notifications_) {
            Json notifyPayload = Json::object();
            notifyPayload.set("initiator_id", Json(session->userId()));
            notifyPayload.set("topic", Json(request.payload.getString("topic", "встреча")));
            notifications_->create(peer->id, "a2a.proposal",
                                   "Selfvoy начал переговоры",
                                   "Тема: " + request.payload.getString("topic", "встреча"),
                                   notifyPayload);
        }
        return protocol::ok(request.id, result.payload);
    });

    // ------------------------------------------------------------ invite
    // Приглашение по ссылке: владелец (вход нужен) и гость без аккаунта
    // (invite.get / invite.respond — публичные, доступ по токену ссылки).
    registerHandler("invite.create", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = invites_->create(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("invite.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = invites_->list(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("invite.cancel", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = invites_->cancel(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("invite.get", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = invites_->get(request.payload, session->remoteAddr());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("invite.respond", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = invites_->respond(request.payload, session->remoteAddr());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // ------------------------------------------------------------ booking
    // Страницы записи: владелец (вход нужен) и гость без аккаунта (booking.get /
    // book / manage / reschedule / cancel — публичные, доступ по slug или токену).
    const auto bookingReply = [](const protocol::Request& request, const BookingManager::Result& result) {
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    };
    registerHandler("booking.pageCreate", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->pageCreate(session->userId(), request.payload));
    });
    registerHandler("booking.pageList", [this, bookingReply](std::shared_ptr<Session> session,
                                                             const protocol::Request& request) {
        return bookingReply(request, bookings_->pageList(session->userId()));
    });
    registerHandler("booking.pageUpdate", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->pageUpdate(session->userId(), request.payload));
    });
    registerHandler("booking.pageDelete", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->pageDelete(session->userId(), request.payload));
    });
    registerHandler("booking.list", [this, bookingReply](std::shared_ptr<Session> session,
                                                         const protocol::Request& request) {
        return bookingReply(request, bookings_->list(session->userId(), request.payload));
    });
    registerHandler("booking.ownerCancel", [this, bookingReply](std::shared_ptr<Session> session,
                                                                const protocol::Request& request) {
        return bookingReply(request, bookings_->ownerCancel(session->userId(), request.payload));
    });
    registerHandler("booking.get", [this, bookingReply](std::shared_ptr<Session> session,
                                                        const protocol::Request& request) {
        return bookingReply(request, bookings_->get(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.book", [this, bookingReply](std::shared_ptr<Session> session,
                                                         const protocol::Request& request) {
        return bookingReply(request, bookings_->book(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.manage", [this, bookingReply](std::shared_ptr<Session> session,
                                                           const protocol::Request& request) {
        return bookingReply(request, bookings_->manage(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.reschedule", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->reschedule(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.cancel", [this, bookingReply](std::shared_ptr<Session> session,
                                                           const protocol::Request& request) {
        return bookingReply(request, bookings_->cancel(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.resendLink", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->resendLink(request.payload, session->remoteAddr()));
    });
    // F4: «Я оплатил(а)» (гость) и отметка оплаты владельцем.
    registerHandler("booking.paymentClaim", [this, bookingReply](std::shared_ptr<Session> session,
                                                                 const protocol::Request& request) {
        return bookingReply(request, bookings_->paymentClaim(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.paymentSet", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->paymentSet(session->userId(), request.payload));
    });
    // F4: магазин ЮKassa владельца.
    const auto paymentsReply = [](const protocol::Request& request, const PaymentManager::Result& result) {
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    };
    registerHandler("payments.status", [this, paymentsReply](std::shared_ptr<Session> session,
                                                             const protocol::Request& request) {
        return paymentsReply(request, payments_->status(session->userId()));
    });
    registerHandler("payments.connect", [this, paymentsReply](std::shared_ptr<Session> session,
                                                              const protocol::Request& request) {
        return paymentsReply(request, payments_->connect(session->userId(), request.payload));
    });
    registerHandler("payments.disconnect", [this, paymentsReply](std::shared_ptr<Session> session,
                                                                 const protocol::Request& request) {
        return paymentsReply(request, payments_->disconnect(session->userId()));
    });
    // F5: пакеты (абонементы): гость — по ссылке пакета #/p/<токен>, владелец — продажа
    // вне сайта, корректировка остатка, отмена, отметка оплаты.
    registerHandler("package.get", [this, bookingReply](std::shared_ptr<Session> session,
                                                        const protocol::Request& request) {
        return bookingReply(request, bookings_->packageGet(request.payload, session->remoteAddr()));
    });
    registerHandler("package.paymentClaim", [this, bookingReply](std::shared_ptr<Session> session,
                                                                 const protocol::Request& request) {
        return bookingReply(request, bookings_->packagePaymentClaim(request.payload, session->remoteAddr()));
    });
    registerHandler("package.list", [this, bookingReply](std::shared_ptr<Session> session,
                                                         const protocol::Request& request) {
        return bookingReply(request, bookings_->packageList(session->userId(), request.payload));
    });
    registerHandler("package.issue", [this, bookingReply](std::shared_ptr<Session> session,
                                                          const protocol::Request& request) {
        return bookingReply(request, bookings_->packageIssue(session->userId(), request.payload));
    });
    registerHandler("package.adjust", [this, bookingReply](std::shared_ptr<Session> session,
                                                           const protocol::Request& request) {
        return bookingReply(request, bookings_->packageAdjust(session->userId(), request.payload));
    });
    registerHandler("package.cancel", [this, bookingReply](std::shared_ptr<Session> session,
                                                           const protocol::Request& request) {
        return bookingReply(request, bookings_->packageCancel(session->userId(), request.payload));
    });
    registerHandler("package.paymentSet", [this, bookingReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return bookingReply(request, bookings_->packagePaymentSet(session->userId(), request.payload));
    });
    // F1: «Приду» по ссылке из напоминания (#/m/<токен>?confirm=<start>).
    registerHandler("booking.confirm", [this, bookingReply](std::shared_ptr<Session> session,
                                                            const protocol::Request& request) {
        return bookingReply(request, bookings_->confirm(request.payload, session->remoteAddr()));
    });

    // ------------------------------------- F8: лист ожидания (гость — по slug/токену)
    registerHandler("booking.waitlistJoin", [this, bookingReply](std::shared_ptr<Session> session,
                                                                 const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistJoin(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.waitlistGet", [this, bookingReply](std::shared_ptr<Session> session,
                                                                const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistGet(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.waitlistClaim", [this, bookingReply](std::shared_ptr<Session> session,
                                                                  const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistClaim(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.waitlistLeave", [this, bookingReply](std::shared_ptr<Session> session,
                                                                  const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistLeave(request.payload, session->remoteAddr()));
    });
    registerHandler("booking.waitlist", [this, bookingReply](std::shared_ptr<Session> session,
                                                             const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistList(session->userId(), request.payload));
    });
    registerHandler("booking.waitlistRemove", [this, bookingReply](std::shared_ptr<Session> session,
                                                                   const protocol::Request& request) {
        return bookingReply(request, bookings_->waitlistRemove(session->userId(), request.payload));
    });
    // ------------------------------------------------ F10: чеки «Мой налог»
    registerHandler("tax.pending", [this, bookingReply](std::shared_ptr<Session> session,
                                                        const protocol::Request& request) {
        return bookingReply(request, bookings_->taxPending(session->userId(), request.payload));
    });
    registerHandler("tax.receiptDone", [this, bookingReply](std::shared_ptr<Session> session,
                                                            const protocol::Request& request) {
        return bookingReply(request, bookings_->taxReceiptDone(session->userId(), request.payload));
    });

    // ---------------------------------------------------- F6: окна и ICS
    registerHandler("booking.slotsPost", [this, bookingReply](std::shared_ptr<Session> session,
                                                              const protocol::Request& request) {
        return bookingReply(request, bookings_->slotsPost(session->userId(), request.payload));
    });
    const auto feedReply = [](const protocol::Request& request, const CalendarFeed::Result& result) {
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    };
    registerHandler("calendar.feed", [this, feedReply](std::shared_ptr<Session> session,
                                                       const protocol::Request& request) {
        return feedReply(request, calendarFeed_->feed(session->userId()));
    });
    registerHandler("calendar.feedReset", [this, feedReply](std::shared_ptr<Session> session,
                                                            const protocol::Request& request) {
        return feedReply(request, calendarFeed_->reset(session->userId()));
    });
    registerHandler("calendar.feedDisable", [this, feedReply](std::shared_ptr<Session> session,
                                                              const protocol::Request& request) {
        return feedReply(request, calendarFeed_->disable(session->userId()));
    });

    // ----------------------------------------------------------- referral
    registerHandler("referral.get", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = referrals_->get(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("speech.transcribe", [this](std::shared_ptr<Session>, const protocol::Request& request) {
        const std::string audio = request.payload.getString("audio");
        if (audio.empty()) {
            return protocol::error(request.id, protocol::code::kBadRequest, "нужно audio (base64)");
        }
        const auto result = agent_->transcribe(audio,
                                               request.payload.getString("language", "auto"),
                                               request.payload.getString("format", "webm"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("agent.status", [this](std::shared_ptr<Session>, const protocol::Request& request) {
        std::string error;
        const bool available = agent_->available(error);
        Json payload = Json::object();
        payload.set("ai_service", Json(config_.aiServiceUrl));
        payload.set("available", Json(available));
        if (!available) payload.set("error", Json(error));
        return protocol::ok(request.id, payload);
    });

    // ------------------------------------------------------------- memory
    registerHandler("memory.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id, memory_->load(session->userId(), request.payload.getString("query"),
                                                      boundedLimit(request.payload, "limit", 20, 200)));
    });

    registerHandler("memory.add", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        Json entries = Json::array();
        const std::string text = request.payload.getString("text");
        if (!text.empty()) {
            Json entry = Json::object();
            entry.set("kind", Json(request.payload.getString("kind", "fact")));
            entry.set("text", Json(text));
            entries.push(entry);
        }
        const Json extra = request.payload.get("entries");
        if (extra.isArray()) {
            for (const auto& entry : extra.items()) entries.push(entry);
        }
        const std::size_t requested = entries.size();
        const std::size_t saved = memory_->remember(session->userId(), entries);
        Json payload = Json::object();
        payload.set("saved", Json(static_cast<long long>(saved)));
        // Раньше отброшенные записи (пустой текст или kind вне CHECK схемы)
        // пропадали молча — клиент не узнавал, что память не пополнилась.
        payload.set("requested", Json(static_cast<long long>(requested)));
        payload.set("skipped", Json(static_cast<long long>(requested - saved)));
        return protocol::ok(request.id, payload);
    });

    registerHandler("memory.extract", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        std::string error;
        const Json result = memory_->extract(session->userId(), request.payload.getString("message"),
                                             request.payload.getBool("commit", true), error);
        return protocol::ok(request.id, result);
    });

    // -------------------------------------------------------- preferences
    registerHandler("prefs.get", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id, memory_->preferences(session->userId()));
    });

    registerHandler("prefs.set", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        // F10: npd_receipts («я самозанятый, напоминать выдать чек») — bool, включение — Pro.
        if (request.payload.contains("npd_receipts")) {
            const Json value = request.payload.get("npd_receipts");
            if (!value.isBool()) {
                return protocol::error(request.id, protocol::code::kBadRequest, "npd_receipts — true или false");
            }
            if (value.asBool() && !memory_->preferences(session->userId()).getBool("npd_receipts") &&
                !proAllowed(session->userId())) {
                return protocol::error(request.id, protocol::code::kProRequired,
                                       "напоминания о чеках «Мой налог» доступны на тарифе Pro");
            }
        }
        std::string error;
        bool invalid = false;
        if (!memory_->setPreferences(session->userId(), request.payload, error, &invalid)) {
            return protocol::error(request.id, invalid ? protocol::code::kBadRequest : protocol::code::kInternal, error);
        }
        return protocol::ok(request.id, memory_->preferences(session->userId()));
    });

    // -------------------------------------------------------------- tools
    registerHandler("tool.list", [this](std::shared_ptr<Session>, const protocol::Request& request) {
        return protocol::ok(request.id, tools_->list());
    });

    registerHandler("tool.run", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        // Прямой вызов инструмента проходит тот же барьер tool_permissions, что и
        // действия Selfvoy: deny — отказ, ask — отложенное действие с подтверждением.
        const std::string tool = request.payload.getString("tool");
        if (!ToolManager::isKnownTool(tool)) {
            return protocol::error(request.id, protocol::code::kBadRequest, "неизвестный инструмент: " + tool);
        }
        const Json item = agent_->executeOne(session->userId(), payloadChatId(request.payload), tool,
                                             request.payload.get("args"));
        if (item.getBool("denied")) {
            return protocol::error(request.id, protocol::code::kForbidden, item.getString("error"));
        }
        if (item.getBool("requires_confirmation")) {
            Json payload = Json::object();
            payload.set("requires_confirmation", Json(true));
            payload.set("confirmation_id", item.get("confirmation_id"));
            payload.set("summary", item.get("summary"));
            payload.set("tool", Json(tool));
            payload.set("status", Json("pending"));
            if (notifications_) {
                Json notifyPayload = Json::object();
                notifyPayload.set("confirmation_id", item.get("confirmation_id"));
                notifyPayload.set("tool", Json(tool));
                notifications_->create(session->userId(), "confirmation.requested", "Нужно подтверждение",
                                       item.getString("summary", "Опасная операция ожидает решения"), notifyPayload);
            }
            return protocol::ok(request.id, payload);
        }
        if (!item.getBool("ok")) return protocol::error(request.id, protocol::code::kBadRequest, item.getString("error"));
        return protocol::ok(request.id, item.get("data"));
    });

    // -------------------------------------- разрешения и подтверждения (этап 8)
    registerHandler("permissions.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id, agent_->listPermissions(session->userId()));
    });

    registerHandler("permissions.set", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = agent_->setPermission(session->userId(),
                                                  request.payload.getString("tool"),
                                                  request.payload.getString("mode"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("confirmation.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id,
                            agent_->listConfirmations(session->userId(),
                                                      request.payload.getString("status", "pending"),
                                                      boundedLimit(request.payload, "limit", 20, 100)));
    });

    registerHandler("confirmation.approve", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = agent_->resolveConfirmation(session->userId(), request.payload.getInt("id"), true);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("confirmation.deny", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = agent_->resolveConfirmation(session->userId(), request.payload.getInt("id"), false);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // ------------------------------------------------------- задачи (этап 8)
    registerHandler("tasks.list", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->list(session->userId(),
                                               request.payload.getString("status"),
                                               boundedLimit(request.payload, "limit", 50, 200));
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("tasks.create", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->create(session->userId(),
                                                 payloadChatId(request.payload),
                                                 request.payload.getString("title"),
                                                 request.payload.getString("notes"),
                                                 request.payload.getString("due_at"),
                                                 request.payload.getString("remind_at"),
                                                 static_cast<int>(request.payload.getInt("priority", 0)));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("tasks.complete", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->setStatus(session->userId(), request.payload.getInt("id"), "done");
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("tasks.cancel", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->setStatus(session->userId(), request.payload.getInt("id"), "cancelled");
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("tasks.reopen", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->setStatus(session->userId(), request.payload.getInt("id"), "pending");
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("tasks.delete", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = taskManager_->remove(session->userId(), request.payload.getInt("id"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // Ручной проход планировщика: отправить наступившие напоминания сейчас —
    // только напоминания вызывающего пользователя (не глобальный проход).
    registerHandler("tasks.due", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        Json payload = Json::object();
        payload.set("sent", Json(static_cast<long long>(runSchedulerTick(100, session->userId()))));
        return protocol::ok(request.id, payload);
    });

    // Интеграции (этап 9): Google OAuth 2.0 + PKCE, токены зашифрованы.
    registerHandler("integrations.list", [this](std::shared_ptr<Session> session,
                                                const protocol::Request& request) {
        const auto result = integrations_->list(session->userId());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("integrations.begin", [this](std::shared_ptr<Session> session,
                                                 const protocol::Request& request) {
        const auto result = integrations_->begin(session->userId(),
                                                 request.payload.getString("provider"),
                                                 request.payload.getString("redirect_uri"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("integrations.callback", [this](std::shared_ptr<Session> session,
                                                    const protocol::Request& request) {
        const auto result = integrations_->callback(session->userId(),
                                                    request.payload.getString("provider"),
                                                    request.payload.getString("code"),
                                                    request.payload.getString("state"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("integrations.revoke", [this](std::shared_ptr<Session> session,
                                                  const protocol::Request& request) {
        const auto result = integrations_->revoke(session->userId(), request.payload.getInt("id"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("integrations.sync", [this](std::shared_ptr<Session> session,
                                                const protocol::Request& request) {
        const auto result = integrations_->sync(session->userId(), request.payload.getInt("id"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("integrations.connect_caldav", [this](std::shared_ptr<Session> session,
                                                          const protocol::Request& request) {
        const auto result = integrations_->connectCalDav(session->userId(),
                                                         request.payload.getString("provider", "apple_caldav"),
                                                         request.payload.getString("caldav_url"),
                                                         request.payload.getString("username"),
                                                         request.payload.getString("password"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });


    // Уведомления (этап 13): in-app «входящая» + устройства push-доставки.
    registerHandler("notifications.list", [this](std::shared_ptr<Session> session,
                                                 const protocol::Request& request) {
        const auto result = notifications_->list(
            session->userId(),
            request.payload.getBool("unread", false),
            boundedLimit(request.payload, "limit", 50, 200));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // id отсутствует или 0 — отметить все прочитанными.
    registerHandler("notifications.read", [this](std::shared_ptr<Session> session,
                                                 const protocol::Request& request) {
        const auto result = notifications_->markRead(session->userId(),
                                                     request.payload.getInt("id", 0));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("devices.push.register", [this](std::shared_ptr<Session> session,
                                                    const protocol::Request& request) {
        const auto result = notifications_->registerDevice(
            session->userId(),
            request.payload.getString("platform", "apns"),
            request.payload.getString("token"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("devices.push.list", [this](std::shared_ptr<Session> session,
                                                const protocol::Request& request) {
        const auto result = notifications_->listDevices(session->userId());
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("devices.push.revoke", [this](std::shared_ptr<Session> session,
                                                  const protocol::Request& request) {
        const auto result = notifications_->revokeDevice(session->userId(),
                                                         request.payload.getInt("id"));
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    // ---------------------------------------------------- админ-панель
    // Права (moderator / admin) проверяет AdminManager по роли из БД на каждый
    // запрос: понижение роли действует сразу, без перевхода.
    const auto adminReply = [this](const protocol::Request& request, const AdminManager::Result& result,
                                   const char* kickReason) {
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        kickUsers(connections_, result.kick, kickReason);
        return protocol::ok(request.id, result.payload);
    };

    registerHandler("admin.stats", [this, adminReply](std::shared_ptr<Session> session,
                                                      const protocol::Request& request) {
        return adminReply(request, admin_->stats(session->userId(), onlineUsers(connections_)), "");
    });

    registerHandler("admin.users", [this, adminReply](std::shared_ptr<Session> session,
                                                      const protocol::Request& request) {
        return adminReply(request, admin_->users(session->userId(), request.payload), "");
    });

    registerHandler("admin.user", [this, adminReply](std::shared_ptr<Session> session,
                                                     const protocol::Request& request) {
        return adminReply(request, admin_->user(session->userId(), request.payload), "");
    });

    registerHandler("admin.setActive", [this, adminReply](std::shared_ptr<Session> session,
                                                          const protocol::Request& request) {
        return adminReply(request, admin_->setActive(session->userId(), request.payload, session->remoteAddr()),
                          "account_disabled");
    });

    registerHandler("admin.setRole", [this, adminReply](std::shared_ptr<Session> session,
                                                        const protocol::Request& request) {
        return adminReply(request, admin_->setRole(session->userId(), request.payload, session->remoteAddr()), "");
    });

    registerHandler("admin.revokeSessions", [this, adminReply](std::shared_ptr<Session> session,
                                                               const protocol::Request& request) {
        return adminReply(request,
                          admin_->revokeSessions(session->userId(), request.payload, session->remoteAddr()),
                          "sessions_revoked");
    });

    registerHandler("admin.audit", [this, adminReply](std::shared_ptr<Session> session,
                                                      const protocol::Request& request) {
        return adminReply(request, admin_->audit(session->userId(), request.payload), "");
    });

    // ------------------------------------------- рост (S1a): тариф и Telegram
    registerHandler("admin.setPlan", [this, adminReply](std::shared_ptr<Session> session,
                                                        const protocol::Request& request) {
        return adminReply(request, admin_->setPlan(session->userId(), request.payload, session->remoteAddr()), "");
    });

    registerHandler("telegram.status", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id, telegram_->status(session->userId()));
    });

    registerHandler("telegram.link", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        Json payload;
        std::string error;
        if (!telegram_->link(session->userId(), payload, error)) {
            return protocol::error(request.id, protocol::code::kBadRequest, error);
        }
        return protocol::ok(request.id, payload);
    });

    registerHandler("telegram.unlink", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        return protocol::ok(request.id, telegram_->unlink(session->userId()));
    });

    // F7: вопросы гостей ИИ-администратору, переданные специалисту, и ответ из приложения.
    registerHandler("telegram.questions", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = telegramAdmin_->questions(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("telegram.reply", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = telegramAdmin_->reply(session->userId(), request.payload);
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });
}

}  // namespace selfvoy
