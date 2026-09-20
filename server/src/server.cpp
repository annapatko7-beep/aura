// aura/server.cpp
#include "aura/server.h"

#include <algorithm>
#include <utility>

#include "aura/log.h"

namespace aura {

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

}  // namespace

Server::Server(Config config) : config_(std::move(config)) {
    log::setLevel(config_.logLevel);
}

Server::~Server() { stop(); }

bool Server::start(std::string& error) {
    database_ = DatabaseManager::fromConfig(config_, error);
    if (!database_) return false;

    auth_ = std::make_unique<AuthManager>(*database_, config_);
    chats_ = std::make_unique<ChatManager>(*database_, connections_);
    memory_ = std::make_unique<MemoryManager>(*database_, config_);
    tools_ = std::make_unique<ToolManager>(config_, *database_, *chats_);
    agent_ = std::make_unique<AgentManager>(config_, *database_, *memory_, *tools_, *chats_);

    registerHandlers();

    listener_ = std::make_unique<Listener>(
        config_, [this](std::shared_ptr<Session> session) { handleConnection(std::move(session)); });
    if (!listener_->start(error)) return false;

    AURA_LOG(log::Level::Info, "server") << "Aura server запущен: " << config_.describe();
    std::string aiError;
    if (agent_->available(aiError)) {
        AURA_LOG(log::Level::Info, "server") << "AI-сервис доступен: " << config_.aiServiceUrl;
    } else {
        AURA_LOG(log::Level::Warn, "server")
            << "AI-сервис не отвечает (" << config_.aiServiceUrl << "): агент вернёт ошибку upstream_error";
    }
    return true;
}

void Server::stop() {
    const bool already = stopping_.exchange(true);
    if (listener_) listener_->stop();
    connections_.closeAll(1001, "server shutdown");
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
    json.set("service", Json("aura-server"));
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
    session->authenticate(result.userId, result.displayName, result.email, result.jwtId);
    connections_.add(session);
    payload = result.payload;
    return true;
}

void Server::handleConnection(std::shared_ptr<Session> session) {
    std::string error;
    if (!session->performHandshake(error)) {
        AURA_LOG(log::Level::Warn, "server") << "рукопожатие не удалось: " << error;
        return;
    }

    // Токен можно передать сразу в заголовке рукопожатия.
    Json authPayload;
    if (authorize(session, tokenFromSession(session), authPayload)) {
        AURA_LOG(log::Level::Info, "server")
            << "сессия " << session->id() << " авторизована как " << session->email();
        Json welcome = protocol::event("session.ready", session->describe());
        welcome.set("auth", authPayload);
        session->send(welcome);
    }

    session->readLoop([this](std::shared_ptr<Session> active, const Json& message) {
        const Json response = handleMessage(active, message);
        if (!response.isNull()) active->send(response);
    });

    connections_.remove(session->id());
    if (session->authenticated()) database_->db().touchUser(session->userId());
    AURA_LOG(log::Level::Info, "server") << "сессия " << session->id() << " завершена";
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

    if (!protocol::isPublicType(request.type) && !session->authenticated()) {
        Json payload;
        const std::string token = request.payload.getString("token", tokenFromSession(session));
        if (!authorize(session, token, payload)) {
            return protocol::error(request.id, protocol::code::kUnauthorized, "требуется вход");
        }
    }

    try {
        return handler(session, request);
    } catch (const std::exception& exception) {
        AURA_LOG(log::Level::Error, "server")
            << "исключение в обработчике " << request.type << ": " << exception.what();
        return protocol::error(request.id, protocol::code::kInternal, exception.what());
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
            payload.set("service", Json("aura-server"));
            payload.set("version", Json("0.1"));
            return protocol::ok(request.id, payload);
        }
        Json payload = health();
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
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.verifyEmail",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        (void)session;
                        const auto result = auth_->verifyEmail(request.payload.getString("email"),
                                                               request.payload.getString("code"));
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        return protocol::ok(request.id, result.payload);
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
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.refresh",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->refresh(request.payload.getString("refresh_token"),
                                                           request.payload.getString("device", "qt-client"),
                                                           session->remoteAddr());
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
                        return protocol::ok(request.id, result.payload);
                    });

    registerHandler("auth.changePassword",
                    [this](std::shared_ptr<Session> session, const protocol::Request& request) {
                        const auto result = auth_->changePassword(session->userId(),
                                                                  session->jwtId(),
                                                                  request.payload.getString("old_password"),
                                                                  request.payload.getString("new_password"));
                        if (!result.ok) return protocol::error(request.id, result.code, result.message);
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
        const auto result = auth_->revokeSession(session->userId(), targetId);
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
        // Если отозвали собственную сессию — разлогиниваем соединение.
        if (current && current->id == targetId) {
            session->resetAuth();
            connections_.remove(session->id());
        }
        return protocol::ok(request.id, result.payload);
    });

    registerHandler("sessions.revokeAll", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->revokeAllSessions(session->userId(), session->jwtId());
        if (!result.ok) return protocol::error(request.id, result.code, result.message);
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
                        return protocol::ok(request.id, result.payload);
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
                        return protocol::ok(request.id, result.payload);
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
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("auth.disable2fa", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->disable2fa(session->userId(), request.payload.getString("password"),
                                              session->remoteAddr());
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

    registerHandler("auth.logout", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const auto result = auth_->logout(session->jwtId());
        session->resetAuth();
        connections_.remove(session->id());
        return protocol::ok(request.id, result.payload);
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
        const auto result = chats_->send(session->userId(), payloadChatId(request.payload),
                                         request.payload.getString("body"), request.payload.getString("kind", "text"),
                                         request.payload.get("payload"));
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
        return result.ok ? protocol::ok(request.id, result.payload)
                         : protocol::error(request.id, result.code, result.message);
    });

    registerHandler("agent.negotiate", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const std::string target = request.payload.getString("target");
        std::optional<UserRecord> peer;
        if (target.find('@') != std::string::npos) {
            peer = database_->db().findUserByEmail(target);
        } else {
            const auto candidates = database_->db().searchUsers(target, 5);
            for (const auto& candidate : candidates) {
                if (candidate.id != session->userId()) {
                    peer = candidate;
                    break;
                }
            }
        }
        if (!peer) return protocol::error(request.id, protocol::code::kNotFound, "собеседник не найден");

        const auto result = agent_->negotiate(session->userId(), peer->id,
                                              request.payload.getString("topic", "встреча"),
                                              static_cast<int>(request.payload.getInt("duration_minutes", 60)),
                                              static_cast<int>(request.payload.getInt("window_hours", 96)));
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
        std::string error;
        if (!memory_->setPreferences(session->userId(), request.payload, error)) {
            return protocol::error(request.id, protocol::code::kInternal, error);
        }
        return protocol::ok(request.id, memory_->preferences(session->userId()));
    });

    // -------------------------------------------------------------- tools
    registerHandler("tool.list", [this](std::shared_ptr<Session>, const protocol::Request& request) {
        return protocol::ok(request.id, tools_->list());
    });

    registerHandler("tool.run", [this](std::shared_ptr<Session> session, const protocol::Request& request) {
        const ToolManager::Result result =
            tools_->run(session->userId(), request.payload.getString("tool"), request.payload.get("args"));
        if (!result.ok) return protocol::error(request.id, protocol::code::kBadRequest, result.error);
        return protocol::ok(request.id, result.data);
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
}

}  // namespace aura
