// aura/jwt.cpp — реализация HS256.
#include "aura/jwt.h"

#include <chrono>

#include "aura/crypto.h"

namespace aura::jwt {

long long nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string sign(const Json& claims, const std::string& secret, long long ttlSeconds) {
    Json header = Json::object();
    header.set("alg", Json("HS256"));
    header.set("typ", Json("JWT"));

    Json payload = claims.isObject() ? claims : Json::object();
    const long long issuedAt = nowEpoch();
    if (!payload.contains("iat")) payload.set("iat", Json(static_cast<long long>(issuedAt)));
    if (ttlSeconds > 0 && !payload.contains("exp")) {
        payload.set("exp", Json(static_cast<long long>(issuedAt + ttlSeconds)));
    }

    const std::string head = crypto::base64UrlEncode(header.dump());
    const std::string body = crypto::base64UrlEncode(payload.dump());
    const std::string signingInput = head + "." + body;
    const std::string signature = crypto::base64UrlEncode(crypto::hmacSha256(secret, signingInput));
    return signingInput + "." + signature;
}

Token verify(const std::string& token, const std::string& secret, long long nowEpochSec) {
    Token result;
    const std::size_t first = token.find('.');
    const std::size_t last = token.rfind('.');
    if (first == std::string::npos || last == std::string::npos || first == last) {
        result.error = "некорректный формат токена";
        return result;
    }
    // Ровно две точки: header.payload.signature — иначе разбор неоднозначен.
    if (token.find('.', first + 1) != last) {
        result.error = "некорректный формат токена";
        return result;
    }

    // Заголовок обязан объявлять HS256: иначе можно подсунуть другой алгоритм.
    std::string headerRaw;
    if (!crypto::base64Decode(token.substr(0, first), headerRaw)) {
        result.error = "header не декодируется";
        return result;
    }
    std::string headerError;
    const Json header = Json::parse(headerRaw, &headerError);
    if (header.getString("alg") != "HS256") {
        result.error = "неподдерживаемый алгоритм токена";
        return result;
    }

    const std::string signingInput = token.substr(0, last);
    const std::string signature = token.substr(last + 1);
    const std::string expected = crypto::base64UrlEncode(crypto::hmacSha256(secret, signingInput));
    if (!crypto::constantTimeEquals(signature, expected)) {
        result.error = "подпись не совпадает";
        return result;
    }

    std::string payloadRaw;
    if (!crypto::base64Decode(token.substr(first + 1, last - first - 1), payloadRaw)) {
        result.error = "payload не декодируется";
        return result;
    }
    std::string parseError;
    result.claims = Json::parse(payloadRaw, &parseError);
    if (result.claims.isNull()) {
        result.error = "payload не JSON: " + parseError;
        return result;
    }

    const long long now = nowEpochSec > 0 ? nowEpochSec : nowEpoch();
    // exp обязан быть числом: раньше строковое значение молча отменяло проверку
    // срока, и токен становился бессрочным.
    if (const Json* exp = result.claims.find("exp")) {
        if (!exp->isNumber()) {
            result.error = "некорректный exp";
            return result;
        }
        if (exp->asInt() < now) {
            result.error = "токен просрочен";
            return result;
        }
    }

    result.valid = true;
    return result;
}

}  // namespace aura::jwt
