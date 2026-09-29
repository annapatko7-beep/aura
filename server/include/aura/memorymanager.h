// aura/memorymanager.h — загрузка и обновление долговременной памяти пользователя.
#pragma once

#include <string>
#include <vector>

#include "aura/config.h"
#include "aura/databasemanager.h"
#include "aura/json.h"

namespace aura {

class MemoryManager {
public:
    MemoryManager(DatabaseManager& database, const Config& config);

    // Записи памяти, отсортированные по релевантности запросу.
    Json load(long long userId, const std::string& query, int limit = 20);

    // Готовый контекст для AI-сервиса: память + настройки + расписание.
    Json context(long long userId, const std::string& focusMessage);

    // Сохраняет записи, которые вернул AI-сервис в memory_updates.
    std::size_t remember(long long userId, const Json& updates);

    // Просит AI-сервис извлечь факты из фразы (POST /v1/memory/extract).
    // При недоступном сервисе работает локальное правило (см. extractLocally).
    Json extract(long long userId, const std::string& message, bool commit, std::string& error);

    // Локальное извлечение фактов (тот же набор правил, что в Python memory.py).
    static Json extractLocally(const std::string& message);

    Json preferences(long long userId);
    bool setPreferences(long long userId, const Json& preferences, std::string& error);

    std::vector<MemoryRecord> schedule(long long userId);

private:
    DatabaseManager& database_;
    const Config& config_;
};

}  // namespace aura
