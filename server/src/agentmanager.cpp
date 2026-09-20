// aura/agentmanager.cpp
#include "aura/agentmanager.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "aura/chatmanager.h"
#include "aura/log.h"
#include "aura/net.h"
#include "aura/protocol.h"

namespace aura {

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

Json AgentManager::executeActions(long long userId, const Json& actions, bool execute) {
    Json results = Json::array();
    if (!actions.isArray()) return results;

    for (const auto& action : actions.items()) {
        const std::string tool = action.getString("tool");
        const Json args = action.get("args").isObject() ? action.get("args") : Json::object();
        Json item = Json::object();
        item.set("id", Json(action.getString("id")));
        item.set("tool", Json(tool));
        if (!execute) {
            item.set("ok", Json(false));
            item.set("skipped", Json(true));
            results.push(item);
            continue;
        }
        const ToolManager::Result outcome = tools_.run(userId, tool, args);
        item.set("ok", Json(outcome.ok));
        if (outcome.ok) {
            item.set("data", outcome.data);
        } else {
            item.set("error", Json(outcome.error));
        }
        results.push(item);
    }
    return results;
}

AgentManager::Result AgentManager::ask(long long userId,
                                       long long chatId,
                                       const std::string& message,
                                       const Json& peers,
                                       bool execute) {
    if (message.empty()) return Result::failure(protocol::code::kBadRequest, "пустой запрос к Ауре");

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
            if (!resolved) continue;
            peerContexts.push(memory_.context(resolved->id, message));
        }
    }
    request.set("peers", peerContexts);

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Aura-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response = net::httpRequest(
        "POST", joinUrl(config_.aiServiceUrl, "/v1/agent/run"), headers, request.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        AURA_LOG(log::Level::Error, "agent") << "AI-сервис ответил " << response.status << ": " << error;
        return Result::failure(protocol::code::kUpstream,
                               "AI-сервис недоступен: " + (error.empty() ? std::to_string(response.status) : error));
    }

    const Json answer = Json::parse(response.body, &error);
    if (answer.isNull()) {
        return Result::failure(protocol::code::kUpstream, "AI-сервис вернул не-JSON: " + error);
    }

    // 1. Выполняем действия
    const Json actions = answer.get("actions");
    const Json results = executeActions(userId, actions, execute);

    // 2. Обновляем долговременную память
    std::size_t savedMemory = 0;
    if (execute) savedMemory = memory_.remember(userId, answer.get("memory_updates"));

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

    // 3. Agent-to-Agent: договариваемся с Аурой собеседника
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
                const std::string text = negotiation.payload.getString("message", "Аура договорилась о встрече");
                Json payload = Json::object();
                payload.set("origin", Json("aura-a2a"));
                payload.set("proposal", negotiation.payload);
                chats_.send(userId, chatId, text, "agent_reply", payload);
            }
        } else {
            result.payload.set("a2a", proposal);
            result.payload.set("a2a_note", Json("собеседник не найден среди пользователей Aura"));
        }
    }

    AURA_LOG(log::Level::Info, "agent")
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
    request.set("responder", memory_.context(responderId, topic));
    request.set("intent", Json("schedule_meeting"));
    request.set("topic", Json(topic));
    request.set("duration_minutes", Json(durationMinutes));
    request.set("window_hours", Json(windowHours));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Aura-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", joinUrl(config_.aiServiceUrl, "/v1/agent/negotiate"), headers,
                         request.dump(), config_.aiTimeoutMs, error);

    Result result;
    if (!response.ok()) {
        AURA_LOG(log::Level::Warn, "agent") << "A2A-переговоры не удались: " << error;
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

AgentManager::Result AgentManager::transcribe(const std::string& audioBase64,
                                              const std::string& language,
                                              const std::string& format) {
    Json request = Json::object();
    request.set("audio", Json(audioBase64));
    request.set("language", Json(language.empty() ? std::string("auto") : language));
    if (!format.empty()) request.set("format", Json(format));

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Aura-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", joinUrl(config_.aiServiceUrl, "/v1/speech/transcribe"), headers,
                         request.dump(), config_.aiTimeoutMs, error);

    Result result;
    if (!response.ok()) {
        AURA_LOG(log::Level::Warn, "agent") << "STT не удался: " << error;
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен для распознавания речи");
    }
    const Json answer = Json::parse(response.body, &error);
    if (answer.isNull()) return Result::failure(protocol::code::kUpstream, "некорректный ответ STT");

    result.ok = true;
    result.payload = answer;  // {text, language, provider}
    return result;
}

}  // namespace aura
