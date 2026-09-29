// aura/jwt.h — JWT HS256: подпись и проверка токенов сессии.
#pragma once

#include <string>

#include "aura/json.h"

namespace aura::jwt {

struct Token {
    bool valid = false;
    Json claims;        // payload токена
    std::string error;  // причина отказа (для логов)
};

// Подписывает claims (добавляет iat, если его нет).
std::string sign(const Json& claims, const std::string& secret, long long ttlSeconds);

// Проверяет подпись и срок действия. nowEpochSec = 0 → взять текущее время.
Token verify(const std::string& token, const std::string& secret, long long nowEpochSec = 0);

long long nowEpoch();

}  // namespace aura::jwt
