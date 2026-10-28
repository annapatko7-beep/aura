// selfvoy/schedulelock.cpp
#include "selfvoy/schedulelock.h"

#include <chrono>
#include <thread>

#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"

namespace selfvoy {

ScheduleLock::Guard::Guard(ScheduleLock* owner, long long ownerId) : owner_(owner), ownerId_(ownerId) {}

ScheduleLock::Guard::Guard(Guard&& other) noexcept
    : owner_(other.owner_), ownerId_(other.ownerId_), ok_(other.ok_), held_(other.held_) {
    other.ok_ = false;
    other.held_ = false;
    other.owner_ = nullptr;
}

ScheduleLock::Guard::~Guard() { release(); }

void ScheduleLock::Guard::release() {
    if (!held_ || !owner_) return;
    held_ = false;
    owner_->unlock(ownerId_, ok_);
}

std::shared_ptr<ScheduleLock::Slot> ScheduleLock::slot(long long ownerId) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto& entry = slots_[ownerId];
    if (!entry) entry = std::make_shared<Slot>();
    ++entry->users;
    return entry;
}

void ScheduleLock::unlock(long long ownerId, bool advisory) {
    if (advisory) database_.db().unlockSchedule(ownerId);
    std::lock_guard<std::mutex> lock(mapMutex_);
    const auto it = slots_.find(ownerId);
    if (it == slots_.end()) return;
    const std::shared_ptr<Slot> entry = it->second;
    entry->mutex.unlock();
    if (--entry->users == 0) slots_.erase(it);
}

ScheduleLock::Guard ScheduleLock::acquire(long long ownerId) {
    const std::shared_ptr<Slot> entry = slot(ownerId);
    entry->mutex.lock();
    Guard guard(this, ownerId);
    guard.held_ = true;
    // Короткие попытки вместо блокирующего ожидания: соединение с БД общее,
    // нельзя держать его, пока ждём другой экземпляр.
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (database_.db().tryLockSchedule(ownerId)) {
            guard.ok_ = true;
            return guard;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    SELFVOY_LOG(log::Level::Warn, "schedule") << "расписание владельца " << ownerId << " занято другим экземпляром";
    return guard;
}

}  // namespace selfvoy
