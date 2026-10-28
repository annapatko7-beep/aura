// selfvoy/protocol.cpp
#include "selfvoy/protocol.h"

namespace selfvoy::protocol {

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
    const Json id = message.get("id");
    out.id = id.isNumber() ? std::to_string(id.asInt()) : message.getString("id");
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
           type == "auth.oauth.providers" || type == "auth.oauth.begin" || type == "auth.oauth.callback" ||
           type == "invite.get" || type == "invite.respond" ||
           type == "booking.get" || type == "booking.book" || type == "booking.manage" ||
           type == "booking.reschedule" || type == "booking.cancel" || type == "booking.resendLink" ||
           type == "booking.confirm" || type == "booking.paymentClaim" ||
           type == "package.get" || type == "package.paymentClaim" ||
           type == "booking.waitlistJoin" || type == "booking.waitlistGet" || type == "booking.waitlistClaim" ||
           type == "booking.waitlistLeave" ||
           type == "ping" || type == "server.info";
}

}  // namespace selfvoy::protocol
