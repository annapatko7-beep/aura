// aura/taskmanager.h — задачи и напоминания + планировщик уведомлений (этап 8).
//
// Задачи хранятся в таблице tasks. Планировщик (dueTick) находит напоминания,
// у которых наступил срок (remind_at <= now), отмечает их отправленными и
// рассылает владельцам событие task.due. Сервер вызывает dueTick по таймеру.
#pragma once

#include <string>
#include <vector>

#include "aura/connectionmanager.h"
#include "aura/databasemanager.h"
#include "aura/idatabase.h"
#include "aura/json.h"

namespace aura {

class TaskManager {
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

    TaskManager(DatabaseManager& database, ConnectionManager& connections);

    Result create(long long userId,
                  long long chatId,
                  const std::string& title,
                  const std::string& notes,
                  const std::string& dueAt,
                  const std::string& remindAt,
                  int priority);
    Result list(long long userId, const std::string& status, int limit);
    // status: done | cancelled | pending (переоткрыть).
    Result setStatus(long long userId, long long taskId, const std::string& status);
    Result remove(long long userId, long long taskId);

    // Планировщик: находит просроченные напоминания, помечает отправленными и
    // рассылает task.due. Возвращает обработанные задачи (для тестов/логов).
    std::vector<TaskRecord> dueTick(int limit);

private:
    DatabaseManager& database_;
    ConnectionManager& connections_;
};

}  // namespace aura
