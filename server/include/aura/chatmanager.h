// aura/chatmanager.h — чаты, участники, сообщения и история.
#pragma once

#include <string>

#include "aura/connectionmanager.h"
#include "aura/databasemanager.h"
#include "aura/json.h"

namespace aura {

class ChatManager {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();
        MessageRecord record;

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    ChatManager(DatabaseManager& database, ConnectionManager& connections);

    Result list(long long userId, int limit);
    Result open(long long userId, const std::string& contact, const std::string& title);
    Result history(long long userId, long long chatId, long long beforeId, int limit);
    Result members(long long userId, long long chatId);
    Result markRead(long long userId, long long chatId);
    Result searchUsers(const std::string& query, int limit);

    // Отправка сообщения: сохранение + рассылка события chat.message участникам.
    // senderId = 0 означает системное сообщение (например, результат работы Ауры).
    Result send(long long senderId,
                long long chatId,
                const std::string& body,
                const std::string& kind,
                const Json& payload);

    std::vector<MessageRecord> recent(long long chatId, int limit);

private:
    DatabaseManager& database_;
    ConnectionManager& connections_;
};

}  // namespace aura
