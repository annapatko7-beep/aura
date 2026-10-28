// selfvoy/schedulelock.h — блокировка расписания владельца на время «проверить, что
// время свободно → записать встречу» (invite.respond, booking.*).
//
// Внутри процесса — mutex на владельца (приглашения и записи одного владельца не
// займут одно время одновременно; разные владельцы друг друга не ждут). Между экземплярами сервера на общем PostgreSQL —
// advisory lock по id владельца (IDatabase::tryLockSchedule); встроенное
// хранилище работает в одном процессе, ему хватает mutex.
#pragma once

#include <map>
#include <memory>
#include <mutex>

namespace selfvoy {

class DatabaseManager;

class ScheduleLock {
public:
    explicit ScheduleLock(DatabaseManager& database) : database_(database) {}

    class Guard {
    public:
        Guard(Guard&& other) noexcept;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;
        ~Guard();
        // false — другой экземпляр сервера не отпустил расписание за отведённое время.
        bool ok() const { return ok_; }
        // Отпустить раньше деструктора: запись сохранена, дальше — уведомления и
        // письма, которые не должны держать расписание владельца.
        void release();

    private:
        friend class ScheduleLock;
        Guard(ScheduleLock* owner, long long ownerId);
        ScheduleLock* owner_;
        long long ownerId_;
        bool ok_ = false;
        bool held_ = false;
    };

    // Ждёт блокировку не дольше ~3 с.
    Guard acquire(long long ownerId);

private:
    struct Slot {
        std::mutex mutex;
        int users = 0;  // под mapMutex_: сколько Guard держат или ждут этот слот
    };
    std::shared_ptr<Slot> slot(long long ownerId);
    void unlock(long long ownerId, bool advisory);

    DatabaseManager& database_;
    std::mutex mapMutex_;
    std::map<long long, std::shared_ptr<Slot>> slots_;
};

}  // namespace selfvoy
