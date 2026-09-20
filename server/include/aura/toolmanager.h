// aura/toolmanager.h — выполнение действий Ауры через API.
//
// Инструменты повторяют контракт Python tools.py, чтобы действие можно было
// выполнить и на C++-сервере (основной режим), и на стороне AI-сервиса.
// В режиме sandbox (по умолчанию) внешние вызовы заменяются детерминированными
// ответами — удобно для разработки без сторонних ключей.
#pragma once

#include <string>

#include "aura/chatmanager.h"
#include "aura/config.h"
#include "aura/databasemanager.h"
#include "aura/json.h"

namespace aura {

class ToolManager {
public:
    struct Result {
        bool ok = false;
        Json data = Json::object();
        std::string error;
    };

    ToolManager(const Config& config, DatabaseManager& database, ChatManager& chats);

    Json list() const;
    Result run(long long userId, const std::string& tool, const Json& args);

    // Отдельные инструменты (открыты для тестов).
    Result sendMessage(long long userId, const Json& args);
    Result createNote(long long userId, const Json& args);
    Result createReminder(long long userId, const Json& args);
    Result sendEmail(long long userId, const Json& args);
    Result findCafe(long long userId, const Json& args);
    Result bookTable(long long userId, const Json& args);
    Result checkCalendar(long long userId, const Json& args);
    Result suggestTime(long long userId, const Json& args);

    std::size_t executed() const { return executed_; }

private:
    Result httpTool(const std::string& endpoint,
                    const Json& payload,
                    const Json& sandboxFallback) const;

    const Config& config_;
    DatabaseManager& database_;
    ChatManager& chats_;
    mutable std::size_t executed_ = 0;
};

}  // namespace aura
