// aura/protocol.h — конверт сообщений WebSocket.
//
// Клиент → сервер:   {"id": "req-1", "type": "chat.send", "payload": {...}}
// Сервер → клиент:   {"id": "req-1", "type": "ok", "payload": {...}}
//                    {"id": "req-1", "type": "error", "code": "...", "message": "..."}
// Сервер → клиент:   {"type": "event", "event": "chat.message", "payload": {...}}
#pragma once

#include <string>

#include "aura/json.h"

namespace aura::protocol {

struct Request {
    std::string id;
    std::string type;
    Json payload = Json::object();
};

Json ok(const std::string& id, const Json& payload = Json::object());
Json error(const std::string& id, const std::string& code, const std::string& message);
Json event(const std::string& name, const Json& payload = Json::object());

// Разбирает входящее сообщение. Возвращает false, если это не запрос.
bool parse(const Json& message, Request& out, std::string& error);

// Типы запросов, не требующие авторизации.
bool isPublicType(const std::string& type);

// Коды ошибок (единый словарь для клиента).
namespace code {
constexpr const char* kBadRequest = "bad_request";
constexpr const char* kUnauthorized = "unauthorized";
constexpr const char* kForbidden = "forbidden";
constexpr const char* kNotFound = "not_found";
constexpr const char* kConflict = "conflict";
constexpr const char* kUpstream = "upstream_error";
constexpr const char* kInternal = "internal_error";
}  // namespace code

}  // namespace aura::protocol
