// aura/protocol.cpp
#include "aura/protocol.h"

namespace aura::protocol {

Json ok(const std::string& id, const Json& payload) {
    Json message = Json::object();
    message.set("id", Json(id));
    message.set("type", Json("ok"));
    message.set("payload", payload.isObject() ? payload : Json::object());
    return message;
}

Json error(const std::string& id, const std::string& code, const std::string& message) {
    Json response = Json::object();
    response.set("id", Json(id));
    response.set("type", Json("error"));
    response.set("code", Json(code));
    response.set("message", Json(message));
    return response;
}

Json event(const std::string& name, const Json& payload) {
    Json message = Json::object();
    message.set("type", Json("event"));
    message.set("event", Json(name));
    message.set("payload", payload.isObject() ? payload : Json::object());
    return message;
}

bool parse(const Json& message, Request& out, std::string& error) {
    if (!message.isObject()) {
        error = "ожидался JSON-объект";
        return false;
    }
    out.id = message.getString("id");
    out.type = message.getString("type");
    if (out.type.empty()) {
        error = "не указан тип сообщения";
        return false;
    }
    out.payload = message.get("payload").isObject() ? message.get("payload") : Json::object();
    return true;
}

bool isPublicType(const std::string& type) {
    return type == "auth.register" || type == "auth.login" || type == "auth.login2fa" ||
           type == "auth.verifyEmail" || type == "auth.resendCode" ||
           type == "auth.forgotPassword" || type == "auth.resetPassword" ||
           type == "auth.refresh" ||
           type == "ping" || type == "server.info";
}

}  // namespace aura::protocol
