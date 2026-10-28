// selfvoy/agentmanager.cpp
#include "selfvoy/agentmanager.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "selfvoy/chatmanager.h"
#include "selfvoy/log.h"
#include "selfvoy/net.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    std::string url = base;
    if (!url.empty() && url.back() == '/') url.pop_back();
    return url + path;
}

}  // namespace

AgentManager::AgentManager(const Config& config,
                           DatabaseManager& database,
                           MemoryManager& memory,
                           ToolManager& tools,
                           ChatManager& chats)
    : config_(config), database_(database), memory_(memory), tools_(tools), chats_(chats) {}

bool AgentManager::available(std::string& error) const {
    return net::pingAiService(config_.aiServiceUrl, std::min(config_.aiTimeoutMs, 3000), error);
}

Json AgentManager::buildContext(long long userId, const std::string& message, const Json& history) {
    Json context = memory_.context(userId, message);
    if (history.isArray()) context.set("history", history);
    return context;
}

std::optional<UserRecord> AgentManager::resolvePeer(long long userId, const std::string& target) {
    if (target.empty()) return std::nullopt;
    if (target.find('@') != std::string::npos) return database_.db().findUserByEmail(target);

    const std::string needle = lower(target);
    for (const auto& user : database_.db().searchUsers(target, 20)) {
        if (user.id == userId) continue;
        const std::string name = lower(user.displayName);
        if (name == needle || name.find(needle) != std::string::npos || needle.find(name.substr(0, 3)) == 0) {
            return user;
        }
    }
    return std::nullopt;
}

std::string AgentManager::effectiveMode(long long userId, const std::string& tool) {
    const std::string explicitMode = database_.db().getToolPermission(userId, tool);
    if (!explicitMode.empty()) return explicitMode;
    return ToolManager::defaultMode(tool);
}

std::string AgentManager::summarizeAction(const std::string& tool, const Json& args) {
    if (tool == "send_message") {
        const std::string to = args.getString("to");
        return "Отправить сообщение " + (to.empty() ? std::string("контакту") : to);
    }
    if (tool == "send_email") return "Отправить письмо на " + args.getString("to", "?");
    if (tool == "book_table") {
        return "Забронировать столик: " + args.getString("place", "?") +
               " на " + std::to_string(args.getInt("people", 2)) + " чел.";
    }
    if (tool == "create_note") return "Создать заметку";
    if (tool == "create_reminder") return "Создать напоминание: " + args.getString("text", "");
    if (tool == "find_cafe") return "Подобрать место";
    if (tool == "check_calendar") return "Проверить календарь";
    if (tool == "suggest_time") return "Предложить время";
    return "Выполнить " + tool;
}

Json AgentManager::executeOne(long long userId, long long chatId, const std::string& tool, const Json& args) {
    Json item = Json::object();
    item.set("tool", Json(tool));
    const Json safeArgs = args.isObject() ? args : Json::object();

    // Барьер безопасности (этап 8): сервер, а не LLM и не клиент, решает, исполнять ли.
    const std::string mode = effectiveMode(userId, tool);
    item.set("mode", Json(mode));
    if (mode == "deny") {
        item.set("ok", Json(false));
        item.set("denied", Json(true));
        item.set("error", Json("действие запрещено вашими настройками разрешений"));
        return item;
    }
    if (mode == "ask") {
        PendingActionRecord pending;
        pending.userId = userId;
        pending.chatId = chatId;
        pending.tool = tool;
        pending.args = safeArgs;
        pending.summary = summarizeAction(tool, safeArgs);
        const long long pendingId = database_.db().createPendingAction(pending);
        item.set("ok", Json(false));
        item.set("requires_confirmation", Json(true));
        item.set("confirmation_id", Json(pendingId));
        item.set("summary", Json(pending.summary));
        return item;
    }

    // mode == "allow": исполняем сразу.
    const ToolManager::Result outcome = tools_.run(userId, tool, safeArgs);
    item.set("ok", Json(outcome.ok));
    if (outcome.ok) {
        item.set("data", outcome.data);
    } else {
        item.set("error", Json(outcome.error));
    }
    return item;
}

Json AgentManager::executeActions(long long userId, long long chatId, const Json& actions, bool execute) {
    Json results = Json::array();
    if (!actions.isArray()) return results;

    for (const auto& action : actions.items()) {
        const std::string tool = action.getString("tool");
        const Json args = action.get("args").isObject() ? action.get("args") : Json::object();
        if (!execute) {
            Json item = Json::object();
            item.set("id", Json(action.getString("id")));
            item.set("tool", Json(tool));
            item.set("ok", Json(false));
            item.set("skipped", Json(true));
            results.push(item);
            continue;
        }
        Json item = executeOne(userId, chatId, tool, args);
        item.set("id", Json(action.getString("id")));
        results.push(item);
    }
    return results;
}

bool AgentManager::shareChat(long long first, long long second) {
    if (first == 0 || second == 0 || first == second) return false;
    if (database_.db().findDirectChat(first, second)) return true;
    for (const auto& chat : database_.db().listChats(first, 500)) {
        if (database_.db().isMember(chat.id, second)) return true;
    }
    return false;
}

Json AgentManager::peerContext(long long responderId, const std::string& topic) {
    // Приватность респондента (prefs.privacy): AI-сервис получает только то,
    // что человек разрешил раскрывать чужому Selfvoy.
    Json responder = memory_.context(responderId, topic);
    const Json responderPrivacy = memory_.privacy(responderId);
    if (responderPrivacy.getBool("hide_event_titles", true)) {
        Json calendar = Json::array();
        for (const auto& event : responder.get("calendar").items()) {
            Json redacted = event;
            redacted.set("title", Json("занято"));
            redacted.erase("tags");
            calendar.push(redacted);
        }
        responder.set("calendar", calendar);
    }
    if (responderPrivacy.getBool("share_free_slots_only", true)) {
        // Только занятость и то, что нужно для времени и места; прочие факты памяти,
        // день рождения и аллергии чужому Selfvoy не уходят.
        responder.set("memory", Json::array());
        Json preferences = responder.get("preferences");
        if (preferences.isObject()) {
            preferences.erase("birthday");
            preferences.erase("allergies");
            responder.set("preferences", preferences);
        }
    }
    return responder;
}

AgentManager::Result AgentManager::ask(long long userId,
                                       long long chatId,
                                       const std::string& message,
                                       const Json& peers,
                                       bool execute) {
    if (message.empty()) return Result::failure(protocol::code::kBadRequest, "пустой запрос к Selfvoy");
    // Те же пределы, что у AI-сервиса: длиннее — отказ здесь, а не 422 от AI.
    const auto symbols = std::count_if(message.begin(), message.end(),
                                       [](unsigned char ch) { return (ch & 0xC0) != 0x80; });
    if (symbols > 8000) return Result::failure(protocol::code::kBadRequest, "запрос к Selfvoy — до 8000 символов");
    if (peers.isArray() && peers.items().size() > 20) {
        return Result::failure(protocol::code::kBadRequest, "собеседников в запросе — не больше 20");
    }
    // История чата уходит в AI только участнику этого чата.
    if (chatId > 0 && !database_.db().isMember(chatId, userId)) {
        return Result::failure(protocol::code::kForbidden, "вы не участник этого чата");
    }

    Json history = Json::array();
    if (chatId > 0) {
        for (const auto& record : chats_.recent(chatId, 20)) {
            Json item = Json::object();
            item.set("sender", Json(record.senderName));
            item.set("body", Json(record.body));
            item.set("created_at", Json(record.createdAt));
            history.push(item);
        }
    }

    Json request = Json::object();
    request.set("context", buildContext(userId, message, history));
    // AI-сервис только планирует: действия выполняет C++ ToolManager.
    request.set("execute", Json(false));
    request.set("dry_run", Json(!execute));

    Json peerContexts = Json::array();
    if (peers.isArray()) {
        for (const auto& peer : peers.items()) {
            const std::string email = peer.getString("email");
            const auto resolved = resolvePeer(userId, email.empty() ? peer.getString("name") : email);
            // Контекст другого человека — только если у вас есть общий чат, и
            // только в объёме, который он разрешил (prefs.privacy).
            if (!resolved || !shareChat(userId, resolved->id)) continue;
            peerContexts.push(peerContext(resolved->id, message));
        }
    }
    request.set("peers", peerContexts);

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response = net::httpRequest(
        "POST", joinUrl(config_.aiServiceUrl, "/v1/agent/run"), headers, request.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Error, "agent") << "AI-сервис ответил " << response.status << ": " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен, попробуйте позже");
    }

    const Json answer = Json::parse(response.body, &error);
    if (!answer.isObject()) {
        SELFVOY_LOG(log::Level::Error, "agent") << "AI-сервис вернул не-JSON: " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис вернул некорректный ответ");
    }

    // 1. Выполняем действия (с барьером подтверждений для опасных операций)
    const Json actions = answer.get("actions");
    const Json results = executeActions(userId, chatId, actions, execute);

    // 2. Обновляем долговременную память
    std::size_t savedMemory = 0;
    // Предложения LLM о памяти — не больше 5 записей по 300 символов (как в AI-сервисе).
    if (execute) savedMemory = memory_.remember(userId, answer.get("memory_updates"), 5, 300);

    Result result;
    result.ok = true;
    result.payload.set("reply", Json(answer.getString("reply")));
    result.payload.set("intent", Json(answer.getString("intent", "chat")));
    result.payload.set("confidence", Json(answer.getDouble("confidence", 0.5)));
    result.payload.set("plan", answer.get("plan"));
    result.payload.set("actions", actions.isArray() ? actions : Json::array());
    result.payload.set("results", results);
    result.payload.set("memory_saved", Json(static_cast<long long>(savedMemory)));
    result.payload.set("llm", Json(answer.getString("llm")));

    // 3. Agent-to-Agent: договариваемся с Selfvoy собеседника
    const Json proposal = answer.get("a2a");
    if (proposal.isObject() && !proposal.getString("target").empty()) {
        const auto peer = resolvePeer(userId, proposal.getString("target"));
        if (peer) {
            const Result negotiation = negotiate(userId,
                                                 peer->id,
                                                 proposal.getString("topic", "встреча"),
                                                 60,
                                                 96);
            result.payload.set("a2a", negotiation.payload);
            if (negotiation.ok && chatId > 0 && execute) {
                const std::string text = negotiation.payload.getString("message", "Selfvoy договорился о встрече");
                Json payload = Json::object();
                payload.set("origin", Json("selfvoy-a2a"));
                payload.set("proposal", negotiation.payload);
                chats_.send(userId, chatId, text, "agent_reply", payload);
            }
        } else {
            const std::string target = proposal.getString("target");
            const bool isRemote = (target.find(':') != std::string::npos || (target.find('@') != std::string::npos && target.find('@') > 0));
            if (isRemote) {
                Json fedReq = Json::object();
                fedReq.set("recipient_handle", Json(target));
                const auto userRec = database_.db().findUserById(userId);
                const std::string senderHandle = userRec ? userRec->email : "user@" + config_.host;
                fedReq.set("sender_handle", Json(senderHandle));
                fedReq.set("intent", Json("schedule_meeting"));
                Json fedPayload = Json::object();
                fedPayload.set("topic", Json(proposal.getString("topic", "встреча")));
                fedPayload.set("proposal", proposal);
                fedReq.set("payload", fedPayload);

                std::map<std::string, std::string> fedHeaders{{"Content-Type", "application/json"}};
                if (!config_.aiServiceToken.empty()) fedHeaders["X-Selfvoy-Token"] = config_.aiServiceToken;

                std::string fedErr;
                const net::HttpResponse fedResp = net::httpRequest(
                    "POST", joinUrl(config_.aiServiceUrl, "/v1/federation/send"),
                    fedHeaders, fedReq.dump(), config_.aiTimeoutMs, fedErr);

                result.payload.set("a2a", proposal);
                result.payload.set("a2a_remote", Json(true));
                if (fedResp.ok()) {
                    result.payload.set("a2a_note", Json("предложение отправлено по протоколу федерации Aura (AFP)"));
                } else {
                    result.payload.set("a2a_note", Json("удалённый агент Aura: " + (fedErr.empty() ? std::to_string(fedResp.status) : fedErr)));
                }
            } else {
                result.payload.set("a2a", proposal);
                result.payload.set("a2a_note", Json("собеседник не найден среди пользователей Selfvoy"));
            }
        }
    }


    SELFVOY_LOG(log::Level::Info, "agent")
        << "user=" << userId << " intent=" << result.payload.getString("intent")
        << " действий=" << (actions.isArray() ? actions.size() : 0);
    return result;
}

AgentManager::Result AgentManager::negotiate(long long initiatorId,
                                             long long responderId,
                                             const std::string& topic,
                                             int durationMinutes,
                                             int windowHours) {
    Json request = Json::object();
    request.set("initiator", memory_.context(initiatorId, topic));

    Json responder = peerContext(responderId, topic);
    const Json responderPrivacy = memory_.privacy(responderId);
    request.set("responder", responder);
    // Место предлагается, только если обе стороны это разрешили.
    const bool choosePlace = memory_.privacy(initiatorId).getBool("allow_place_choice", true) &&
                             responderPrivacy.getBool("allow_place_choice", true);
    request.set("choose_place", Json(choosePlace));
    request.set("intent", Json("schedule_meeting"));
    request.set("topic", Json(topic));
    request.set("duration_minutes", Json(durationMinutes));
    request.set("window_hours", Json(windowHours));
    request.set("multi_turn", Json(true));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", joinUrl(config_.aiServiceUrl, "/v1/agent/negotiate"), headers,
                         request.dump(), config_.aiTimeoutMs, error);

    Result result;
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Warn, "agent") << "A2A-переговоры не удались: " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен для A2A");
    }
    const Json answer = Json::parse(response.body, &error);
    if (answer.isNull()) return Result::failure(protocol::code::kUpstream, "некорректный ответ A2A");

    result.ok = true;
    result.payload = answer;
    result.payload.set("initiator_id", Json(initiatorId));
    result.payload.set("responder_id", Json(responderId));
    return result;
}

AgentManager::Result AgentManager::negotiateFederated(long long initiatorId,
                                                     const std::string& remoteHandle,
                                                     const std::string& topic,
                                                     int durationMinutes,
                                                     int windowHours) {
    const auto initiator = database_.db().findUserById(initiatorId);
    if (!initiator) return Result::failure(protocol::code::kNotFound, "инициатор не найден");

    std::string senderHandle = initiator->email;
    if (senderHandle.empty() || senderHandle.front() != '@') {
        senderHandle = "@" + (senderHandle.empty() ? initiator->displayName : senderHandle);
    }
    if (senderHandle.find(':') == std::string::npos) {
        senderHandle += ":localhost";
    }

    Json initiatorContext = memory_.context(initiatorId, topic);

    Json payload = Json::object();
    payload.set("initiator", initiatorContext);
    payload.set("topic", Json(topic));
    payload.set("duration_minutes", Json(durationMinutes));
    payload.set("window_hours", Json(windowHours));
    payload.set("multi_turn", Json(true));

    Json sendReq = Json::object();
    sendReq.set("sender_handle", Json(senderHandle));
    sendReq.set("recipient_handle", Json(remoteHandle));
    sendReq.set("intent", Json("schedule_meeting"));
    sendReq.set("payload", payload);

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", joinUrl(config_.aiServiceUrl, "/v1/federation/send"), headers,
                         sendReq.dump(), config_.aiTimeoutMs, error);

    Result result;
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Warn, "agent") << "Федеративный A2A-запрос не удался: " << error;
        return Result::failure(protocol::code::kUpstream, "Удалённый узел Aura недоступен через федерацию");
    }
    const Json answer = Json::parse(response.body, &error);
    if (answer.isNull()) return Result::failure(protocol::code::kUpstream, "некорректный ответ федерации");

    result.ok = true;
    result.payload = answer;
    result.payload.set("initiator_id", Json(initiatorId));
    result.payload.set("federated_target", Json(remoteHandle));
    result.payload.set("federated", Json(true));
    return result;
}

AgentManager::Result AgentManager::transcribe(const std::string& audioBase64,
                                              const std::string& language,
                                              const std::string& format) {
    Json request = Json::object();
    request.set("audio", Json(audioBase64));
    request.set("language", Json(language.empty() ? std::string("auto") : language));
    if (!format.empty()) request.set("format", Json(format));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Selfvoy-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", joinUrl(config_.aiServiceUrl, "/v1/speech/transcribe"), headers,
                         request.dump(), config_.aiTimeoutMs, error);

    Result result;
    if (!response.ok()) {
        SELFVOY_LOG(log::Level::Warn, "agent") << "STT не удался: " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен для распознавания речи");
    }
    const Json answer = Json::parse(response.body, &error);
    if (answer.isNull()) return Result::failure(protocol::code::kUpstream, "некорректный ответ STT");

    result.ok = true;
    result.payload = answer;  // {text, language, provider}
    return result;
}

// ---------------------------------------------------------------------------
//  Разрешения и барьер подтверждения (этап 8)
// ---------------------------------------------------------------------------

Json AgentManager::listPermissions(long long userId) {
    const Json catalog = tools_.list();
    const Json catalogTools = catalog.get("tools");
    Json out = Json::array();
    for (const auto& tool : catalogTools.items()) {
        const std::string name = tool.getString("name");
        Json item = Json::object();
        item.set("tool", Json(name));
        item.set("description", tool.get("description"));
        item.set("dangerous", tool.get("dangerous"));
        item.set("default_mode", tool.get("default_mode"));
        item.set("mode", Json(effectiveMode(userId, name)));
        out.push(item);
    }
    Json result = Json::object();
    result.set("tools", out);
    return result;
}

AgentManager::Result AgentManager::setPermission(long long userId,
                                                 const std::string& tool,
                                                 const std::string& mode) {
    if (!ToolManager::isKnownTool(tool)) {
        return Result::failure(protocol::code::kBadRequest, "неизвестный инструмент: " + tool);
    }
    if (mode != "allow" && mode != "ask" && mode != "deny") {
        return Result::failure(protocol::code::kBadRequest, "режим должен быть allow, ask или deny");
    }
    const DatabaseError saved = database_.db().setToolPermission(userId, tool, mode);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, saved.message);

    Json detail = Json::object();
    detail.set("tool", Json(tool));
    detail.set("mode", Json(mode));
    database_.db().insertAudit(userId, "tool_permission", detail, "");

    Result result;
    result.ok = true;
    result.payload.set("tool", Json(tool));
    result.payload.set("mode", Json(mode));
    return result;
}

Json AgentManager::listConfirmations(long long userId, const std::string& status, int limit) {
    Json out = Json::array();
    for (const auto& record : database_.db().listPendingActions(userId, status, limit)) {
        out.push(record.toJson());
    }
    Json result = Json::object();
    result.set("actions", out);
    return result;
}

AgentManager::Result AgentManager::resolveConfirmation(long long userId, long long actionId, bool approve) {
    const auto pending = database_.db().findPendingAction(actionId);
    if (!pending || pending->userId != userId) {
        return Result::failure(protocol::code::kNotFound, "отложенное действие не найдено");
    }
    if (pending->status != "pending") {
        return Result::failure(protocol::code::kBadRequest, "действие уже обработано (" + pending->status + ")");
    }

    Json detail = Json::object();
    detail.set("action_id", Json(actionId));
    detail.set("tool", Json(pending->tool));

    Result result;
    result.ok = true;
    result.payload.set("id", Json(actionId));

    // Атомарный переход pending → approved/denied: из двух одновременных
    // confirmation.approve инструмент исполнит только один.
    if (!database_.db().claimPendingAction(actionId, userId, approve ? "approved" : "denied")) {
        return Result::failure(protocol::code::kBadRequest, "действие уже обработано");
    }

    if (!approve) {
        database_.db().resolvePendingAction(actionId, "denied", Json::object());
        database_.db().insertAudit(userId, "action_denied", detail, "");
        result.payload.set("status", Json("denied"));
        return result;
    }

    // Пользователь подтвердил — теперь исполняем инструмент.
    const ToolManager::Result outcome = tools_.run(userId, pending->tool, pending->args);
    Json outcomeJson = Json::object();
    outcomeJson.set("ok", Json(outcome.ok));
    if (outcome.ok) {
        outcomeJson.set("data", outcome.data);
    } else {
        outcomeJson.set("error", Json(outcome.error));
    }
    const std::string status = outcome.ok ? "executed" : "failed";
    database_.db().resolvePendingAction(actionId, status, outcomeJson);
    detail.set("status", Json(status));
    database_.db().insertAudit(userId, "action_approved", detail, "");

    if (outcome.ok && pending->chatId > 0) {
        Json payload = Json::object();
        payload.set("origin", Json("selfvoy-agent"));
        payload.set("confirmed", Json(true));
        payload.set("tool", Json(pending->tool));
        chats_.send(userId, pending->chatId, "Selfvoy выполнил: " + pending->summary, "agent_action", payload);
    }

    result.payload.set("status", Json(status));
    result.payload.set("result", outcomeJson);
    return result;
}

}  // namespace selfvoy
