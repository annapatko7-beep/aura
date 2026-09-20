// aura/agentmanager.h — мост между C++-сервером и Python AI Service.
//
// Схема работы (по скелету проекта):
//   Session → AgentManager --HTTP--> Python AI Service (agent.py/planner.py/memory.py)
//           ← JSON с действиями
//           → ToolManager выполняет действия
//           → MemoryManager сохраняет новые факты
//           → ChatManager публикует результат в чат
#pragma once

#include <optional>
#include <string>

#include "aura/config.h"
#include "aura/databasemanager.h"
#include "aura/json.h"
#include "aura/memorymanager.h"
#include "aura/toolmanager.h"

namespace aura {

class ChatManager;

class AgentManager {
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

    AgentManager(const Config& config,
                 DatabaseManager& database,
                 MemoryManager& memory,
                 ToolManager& tools,
                 ChatManager& chats);

    // Главный запрос: «Аура, сделай X».
    Result ask(long long userId,
               long long chatId,
               const std::string& message,
               const Json& peers,
               bool execute);

    // Agent-to-Agent: Аура пользователя A договаривается с Аурой пользователя B.
    Result negotiate(long long initiatorId,
                     long long responderId,
                     const std::string& topic,
                     int durationMinutes,
                     int windowHours);

    // Распознавание речи (STT): форвард аудио (base64) в AI-сервис
    // /v1/speech/transcribe (Whisper-совместимый). language: ru|en|auto.
    Result transcribe(const std::string& audioBase64,
                      const std::string& language,
                      const std::string& format);

    // Контекст, который сервер отправляет в AI-сервис (вынесено для тестов).
    Json buildContext(long long userId, const std::string& message, const Json& history);

    bool available(std::string& error) const;

private:
    // Выполняет действия, предложенные моделью, через ToolManager.
    Json executeActions(long long userId, const Json& actions, bool execute);

    // Ищет пользователя-собеседника по email или имени.
    std::optional<UserRecord> resolvePeer(long long userId, const std::string& target);

    const Config& config_;
    DatabaseManager& database_;
    MemoryManager& memory_;
    ToolManager& tools_;
    ChatManager& chats_;
};

}  // namespace aura
