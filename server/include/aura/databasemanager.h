// aura/databasemanager.h — фасад над IDatabase: логи запросов, счётчики,
// единая точка входа для остальных менеджеров.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aura/idatabase.h"

namespace aura {

struct Config;

class DatabaseManager {
public:
    explicit DatabaseManager(std::unique_ptr<IDatabase> database);

    // Создаёт DatabaseManager по конфигурации: PostgreSQL, если задан URL,
    // иначе встроенное файловое хранилище (dev/demo).
    static std::unique_ptr<DatabaseManager> fromConfig(const Config& config, std::string& error);

    IDatabase& db() { return *database_; }
    const IDatabase& db() const { return *database_; }
    std::string backend() const { return database_ ? database_->name() : "none"; }
    bool healthy() const { return database_ && database_->healthy(); }

    // Служебная статистика для healthcheck'а и логов.
    struct Stats {
        unsigned long long queries = 0;
        unsigned long long failures = 0;
    };
    Stats stats() const;

    // Оборачивает вызов репозитория: считает запросы и логирует ошибки.
    template <typename Fn>
    auto track(const char* operation, Fn&& fn) -> decltype(fn()) {
        queries_.fetch_add(1, std::memory_order_relaxed);
        auto result = fn();
        logResult(operation, true);
        return result;
    }

    void logResult(const char* operation, bool ok);
    std::vector<std::string> recentOperations(std::size_t limit = 10) const;

private:
    std::unique_ptr<IDatabase> database_;
    mutable std::mutex mutex_;
    std::vector<std::string> recent_;
    std::atomic<unsigned long long> queries_{0};
    std::atomic<unsigned long long> failures_{0};
};

}  // namespace aura
