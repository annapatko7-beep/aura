// aura/databasemanager.cpp
#include "aura/databasemanager.h"

#include "aura/config.h"
#include "aura/log.h"

namespace aura {

DatabaseManager::DatabaseManager(std::unique_ptr<IDatabase> database)
    : database_(std::move(database)) {}

std::unique_ptr<DatabaseManager> DatabaseManager::fromConfig(const Config& config, std::string& error) {
    if (!config.databaseUrl.empty()) {
        auto postgres = makePostgresDatabase(config.databaseUrl);
        if (postgres && postgres->connect(error)) {
            AURA_LOG(log::Level::Info, "db") << "PostgreSQL подключён";
            return std::make_unique<DatabaseManager>(std::move(postgres));
        }
        AURA_LOG(log::Level::Warn, "db")
            << "PostgreSQL недоступен (" << error << ") — переключаюсь на встроенное хранилище";
        error.clear();
    }

    auto embedded = makeEmbeddedDatabase(config.embeddedDbPath);
    if (!embedded->connect(error)) {
        AURA_LOG(log::Level::Error, "db") << "не удалось инициализировать хранилище: " << error;
        return nullptr;
    }
    AURA_LOG(log::Level::Info, "db") << "встроенное хранилище: " << config.embeddedDbPath;
    return std::make_unique<DatabaseManager>(std::move(embedded));
}

DatabaseManager::Stats DatabaseManager::stats() const {
    Stats snapshot;
    snapshot.queries = queries_.load(std::memory_order_relaxed);
    snapshot.failures = failures_.load(std::memory_order_relaxed);
    return snapshot;
}

void DatabaseManager::logResult(const char* operation, bool ok) {
    if (!ok) failures_.fetch_add(1, std::memory_order_relaxed);
    if (!log::enabled(log::Level::Debug)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.push_back(std::string(operation) + (ok ? "" : " (FAILED)"));
    if (recent_.size() > 32) recent_.erase(recent_.begin());
}

std::vector<std::string> DatabaseManager::recentOperations(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recent_.size() <= limit) return recent_;
    return std::vector<std::string>(recent_.end() - static_cast<std::ptrdiff_t>(limit), recent_.end());
}

}  // namespace aura
