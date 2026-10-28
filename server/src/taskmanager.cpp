// selfvoy/taskmanager.cpp
#include "selfvoy/taskmanager.h"

#include "selfvoy/log.h"
#include "selfvoy/protocol.h"

namespace selfvoy {

TaskManager::TaskManager(DatabaseManager& database, ConnectionManager& connections)
    : database_(database), connections_(connections) {}

TaskManager::Result TaskManager::create(long long userId,
                                        long long chatId,
                                        const std::string& title,
                                        const std::string& notes,
                                        const std::string& dueAt,
                                        const std::string& remindAt,
                                        int priority) {
    if (title.empty()) return Result::failure(protocol::code::kBadRequest, "пустой заголовок задачи");

    TaskRecord record;
    record.userId = userId;
    record.chatId = chatId;
    record.title = title;
    record.notes = notes;
    record.dueAt = dueAt;
    record.remindAt = remindAt;
    record.priority = priority;

    long long id = 0;
    const DatabaseError saved = database_.db().createTask(record, id);
    if (!saved.ok) return Result::failure(protocol::code::kInternal, saved.message);

    Result result;
    result.ok = true;
    if (const auto created = database_.db().findTask(id)) result.payload = created->toJson();
    return result;
}

TaskManager::Result TaskManager::list(long long userId, const std::string& status, int limit) {
    Json tasks = Json::array();
    for (const auto& task : database_.db().listTasks(userId, status, limit)) tasks.push(task.toJson());
    Result result;
    result.ok = true;
    result.payload.set("tasks", tasks);
    return result;
}

TaskManager::Result TaskManager::setStatus(long long userId, long long taskId, const std::string& status) {
    if (status != "pending" && status != "done" && status != "cancelled") {
        return Result::failure(protocol::code::kBadRequest, "статус должен быть pending, done или cancelled");
    }
    const auto task = database_.db().findTask(taskId);
    if (!task || task->userId != userId) return Result::failure(protocol::code::kNotFound, "задача не найдена");

    const DatabaseError updated = database_.db().setTaskStatus(taskId, status);
    if (!updated.ok) return Result::failure(protocol::code::kInternal, updated.message);

    Result result;
    result.ok = true;
    if (const auto fresh = database_.db().findTask(taskId)) result.payload = fresh->toJson();
    return result;
}

TaskManager::Result TaskManager::remove(long long userId, long long taskId) {
    const auto task = database_.db().findTask(taskId);
    if (!task || task->userId != userId) return Result::failure(protocol::code::kNotFound, "задача не найдена");

    const DatabaseError removed = database_.db().deleteTask(taskId);
    if (!removed.ok) return Result::failure(protocol::code::kInternal, removed.message);

    Result result;
    result.ok = true;
    result.payload.set("deleted", Json(taskId));
    return result;
}

std::vector<TaskRecord> TaskManager::dueTick(int limit, long long userId) {
    std::vector<TaskRecord> due;
    for (const auto& task : database_.db().listDueTasks(limit, userId)) {
        // Отметка атомарна: если задачу уже забрал другой проход (фоновый
        // планировщик и tasks.due одновременно), второй её пропускает.
        if (!database_.db().markTaskReminded(task.id).ok) continue;
        connections_.deliverToUser(task.userId, protocol::event("task.due", task.toJson()));
        due.push_back(task);
    }
    if (!due.empty()) {
        SELFVOY_LOG(log::Level::Info, "scheduler") << "напоминаний отправлено: " << due.size();
    }
    return due;
}

}  // namespace selfvoy
