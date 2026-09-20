// aura/toolmanager.cpp
#include "aura/toolmanager.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "aura/crypto.h"
#include "aura/log.h"
#include "aura/net.h"

namespace aura {

namespace {

struct Cafe {
    const char* name;
    const char* city;
    std::vector<std::string> tags;
    double rating;
    int price;
    double lat;
    double lon;
};

// Небольшой справочник мест (в продакшене — запрос к картам через mapsApiUrl).
const std::vector<Cafe> kCafes = {
    {"Кофе на полпути", "Керкраде", {"coffee", "quiet", "vegan"}, 4.7, 2, 50.861, 6.064},
    {"Aurora Roasters", "Керкраде", {"coffee", "workspace", "gluten_free"}, 4.6, 3, 50.857, 6.071},
    {"Grey Garden", "Херлен", {"brunch", "vegan", "quiet"}, 4.5, 2, 50.888, 5.978},
    {"Steak House Nord", "Херлен", {"dinner", "meat"}, 4.4, 4, 50.885, 5.982},
    {"Matcha Point", "Маастрихт", {"coffee", "vegan", "workspace"}, 4.8, 3, 50.849, 5.691},
};

std::vector<std::string> toStringList(const Json& array) {
    std::vector<std::string> result;
    if (!array.isArray()) return result;
    for (const auto& item : array.items()) {
        if (!item.asString().empty()) result.push_back(item.asString());
    }
    return result;
}

bool hasTag(const std::vector<std::string>& tags, const std::string& tag) {
    for (const auto& value : tags) {
        if (value == tag) return true;
    }
    return false;
}

double scoreCafe(const Cafe& cafe, const std::vector<std::string>& diet, double budget, bool hasMidpoint,
                 double midLat, double midLon) {
    double score = cafe.rating / 5.0;
    if (!diet.empty()) {
        for (const auto& tag : diet) {
            if (hasTag(cafe.tags, tag)) score += 0.2;
        }
        const bool vegan = hasTag(diet, "vegan") || hasTag(diet, "vegetarian");
        if (vegan && hasTag(cafe.tags, "meat")) score -= 0.5;
    }
    if (budget > 0 && cafe.price > budget) score -= 0.3;
    if (hasMidpoint) {
        const double distance = std::hypot(cafe.lat - midLat, cafe.lon - midLon);
        score += std::max(0.0, 0.3 - distance);
    }
    return score;
}

}  // namespace

ToolManager::ToolManager(const Config& config, DatabaseManager& database, ChatManager& chats)
    : config_(config), database_(database), chats_(chats) {}

bool ToolManager::isKnownTool(const std::string& tool) {
    static const std::vector<std::string> kTools = {
        "send_message", "create_note", "create_reminder", "send_email",
        "find_cafe",    "book_table",  "check_calendar",  "suggest_time",
    };
    return std::find(kTools.begin(), kTools.end(), tool) != kTools.end();
}

bool ToolManager::isDangerous(const std::string& tool) {
    // Внешние побочные эффекты: отправка кому-либо, письмо, бронирование.
    return tool == "send_message" || tool == "send_email" || tool == "book_table";
}

std::string ToolManager::defaultMode(const std::string& tool) {
    return isDangerous(tool) ? "ask" : "allow";
}

Json ToolManager::list() const {
    Json tools = Json::array();
    const std::vector<std::pair<const char*, const char*>> catalog = {
        {"send_message", "Отправить сообщение контакту или в чат"},
        {"create_note", "Создать заметку (хранится в UserMemory)"},
        {"create_reminder", "Создать напоминание"},
        {"send_email", "Отправить письмо через почтовый API"},
        {"find_cafe", "Подобрать место под диеты, бюджет и середину пути"},
        {"book_table", "Забронировать столик"},
        {"check_calendar", "Показать занятость пользователя"},
        {"suggest_time", "Предложить свободные слоты в окне"},
    };
    for (const auto& item : catalog) {
        Json tool = Json::object();
        tool.set("name", Json(item.first));
        tool.set("description", Json(item.second));
        tool.set("dangerous", Json(isDangerous(item.first)));
        tool.set("default_mode", Json(defaultMode(item.first)));
        tools.push(tool);
    }
    Json result = Json::object();
    result.set("tools", tools);
    result.set("mode", Json(config_.toolsMode));
    return result;
}

ToolManager::Result ToolManager::run(long long userId, const std::string& tool, const Json& args) {
    Result result;
    if (tool == "send_message") result = sendMessage(userId, args);
    else if (tool == "create_note") result = createNote(userId, args);
    else if (tool == "create_reminder") result = createReminder(userId, args);
    else if (tool == "send_email") result = sendEmail(userId, args);
    else if (tool == "find_cafe") result = findCafe(userId, args);
    else if (tool == "book_table") result = bookTable(userId, args);
    else if (tool == "check_calendar") result = checkCalendar(userId, args);
    else if (tool == "suggest_time") result = suggestTime(userId, args);
    else {
        result.error = "неизвестный инструмент: " + tool;
        return result;
    }
    if (result.ok) ++executed_;
    AURA_LOG(result.ok ? log::Level::Debug : log::Level::Warn, "tools")
        << tool << (result.ok ? " выполнен" : " ошибка: " + result.error);
    return result;
}

ToolManager::Result ToolManager::sendMessage(long long userId, const Json& args) {
    Result result;
    const std::string text = args.getString("text");
    if (text.empty()) {
        result.error = "нужен текст сообщения";
        return result;
    }

    long long chatId = args.getInt("chat_id");
    if (chatId == 0) {
        const std::string to = args.getString("to");
        if (to.empty() || to == "self") {
            result.error = "не указан получатель (to или chat_id)";
            return result;
        }
        const auto opened = chats_.open(userId, to, "Аура: " + text.substr(0, 40));
        if (!opened.ok) {
            result.error = opened.message;
            return result;
        }
        chatId = opened.payload.get("chat").getInt("id");
    }

    Json payload = Json::object();
    payload.set("origin", Json("aura-agent"));
    const auto sent = chats_.send(userId, chatId, text, "agent_action", payload);
    if (!sent.ok) {
        result.error = sent.message;
        return result;
    }
    result.ok = true;
    result.data = sent.payload;
    return result;
}

ToolManager::Result ToolManager::createNote(long long userId, const Json& args) {
    Result result;
    const std::string text = args.getString("text");
    if (text.empty()) {
        result.error = "пустая заметка";
        return result;
    }
    const DatabaseError saved =
        database_.db().upsertMemory(userId, "fact", text, 1.0, {"note", "tool"});
    if (!saved.ok) {
        result.error = saved.message;
        return result;
    }
    result.ok = true;
    result.data.set("note_id", Json(crypto::toHex(crypto::sha256(text)).substr(0, 10)));
    result.data.set("text", Json(text));
    result.data.set("created_at", Json(isoNow()));
    return result;
}

ToolManager::Result ToolManager::createReminder(long long userId, const Json& args) {
    Result result;
    const std::string text = args.getString("text");
    if (text.empty()) {
        result.error = "пустой текст напоминания";
        return result;
    }
    const std::string fireAt = args.getString("at", "asap");
    const std::string stored = "Напоминание: " + text + " (" + fireAt + ")";
    const DatabaseError saved = database_.db().upsertMemory(userId, "schedule", stored, 1.5, {"reminder", "tool"});
    if (!saved.ok) {
        result.error = saved.message;
        return result;
    }
    result.ok = true;
    result.data.set("reminder_id", Json(crypto::toHex(crypto::sha256(stored)).substr(0, 10)));
    result.data.set("text", Json(text));
    result.data.set("fire_at", Json(fireAt));
    return result;
}

ToolManager::Result ToolManager::sendEmail(long long userId, const Json& args) {
    Result result;
    const std::string to = args.getString("to");
    if (to.find('@') == std::string::npos) {
        result.error = "нужен корректный адрес получателя";
        return result;
    }
    Json payload = Json::object();
    payload.set("to", Json(to));
    payload.set("subject", Json(args.getString("subject", "Без темы")));
    payload.set("body", Json(args.getString("body")));
    payload.set("from_user", Json(userId));

    Json sandbox = payload;
    sandbox.set("queued", Json(true));
    sandbox.set("message_id", Json(crypto::toHex(crypto::sha256(to + args.getString("body"))).substr(0, 12)));
    sandbox.set("sent_at", Json(isoNow()));
    result = httpTool(config_.emailApiUrl, payload, sandbox);
    return result;
}

ToolManager::Result ToolManager::findCafe(long long userId, const Json& args) {
    Result result;
    const Json preferences = database_.db().getPreferences(userId);
    std::vector<std::string> diet = toStringList(args.get("diet"));
    if (diet.empty()) diet = toStringList(preferences.get("diet"));
    const double budget = args.contains("budget_limit") ? args.getDouble("budget_limit")
                                                        : preferences.getDouble("budget_limit");
    const std::string city = args.getString("city", preferences.getString("city"));

    bool hasMidpoint = false;
    double midLat = 0.0;
    double midLon = 0.0;
    if (const Json* midpoint = args.find("midpoint"); midpoint && midpoint->isObject()) {
        hasMidpoint = midpoint->contains("lat") && midpoint->contains("lon");
        midLat = midpoint->getDouble("lat");
        midLon = midpoint->getDouble("lon");
    }

    std::vector<std::pair<double, std::size_t>> ranked;
    for (std::size_t i = 0; i < kCafes.size(); ++i) {
        if (!city.empty() && kCafes[i].city != city) continue;
        ranked.emplace_back(scoreCafe(kCafes[i], diet, budget, hasMidpoint, midLat, midLon), i);
    }
    if (ranked.empty()) {
        for (std::size_t i = 0; i < kCafes.size(); ++i) {
            ranked.emplace_back(scoreCafe(kCafes[i], diet, budget, hasMidpoint, midLat, midLon), i);
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& left, const auto& right) { return left.first > right.first; });

    Json results = Json::array();
    for (std::size_t i = 0; i < ranked.size() && i < 3; ++i) {
        const Cafe& cafe = kCafes[ranked[i].second];
        Json item = Json::object();
        item.set("name", Json(cafe.name));
        item.set("city", Json(cafe.city));
        item.set("rating", Json(cafe.rating));
        item.set("price", Json(cafe.price));
        item.set("lat", Json(cafe.lat));
        item.set("lon", Json(cafe.lon));
        Json tags = Json::array();
        for (const auto& tag : cafe.tags) tags.push(Json(tag));
        item.set("tags", tags);
        item.set("score", Json(std::round(ranked[i].first * 1000.0) / 1000.0));
        results.push(item);
    }

    Json sandbox = Json::object();
    sandbox.set("results", results);
    sandbox.set("query", args);
    result = httpTool(config_.mapsApiUrl, args, sandbox);
    return result;
}

ToolManager::Result ToolManager::bookTable(long long userId, const Json& args) {
    Result result;
    const std::string place = args.getString("place");
    if (place.empty()) {
        result.error = "нужно имя места (place)";
        return result;
    }
    const long long people = args.getInt("people", 2);
    if (people < 1) {
        result.error = "people должно быть >= 1";
        return result;
    }
    const std::string at = args.getString("at", isoNow());

    Json payload = Json::object();
    payload.set("place", Json(place));
    payload.set("at", Json(at));
    payload.set("people", Json(people));
    payload.set("user_id", Json(userId));

    Json sandbox = payload;
    sandbox.set("confirmation",
                Json("AURA-" + crypto::toHex(crypto::sha1(place + at + std::to_string(people))).substr(0, 10)));
    result = httpTool(config_.mapsApiUrl, payload, sandbox);
    return result;
}

ToolManager::Result ToolManager::checkCalendar(long long userId, const Json& args) {
    Result result;
    result.ok = true;
    Json busy = Json::array();
    for (const auto& record : database_.db().listMemory(userId, 100)) {
        if (record.kind != "schedule") continue;
        Json event = Json::object();
        event.set("title", Json(record.text));
        event.set("updated_at", Json(record.updatedAt));
        busy.push(event);
    }
    result.data.set("window", args.get("window"));
    result.data.set("busy", busy);
    return result;
}

ToolManager::Result ToolManager::suggestTime(long long userId, const Json& args) {
    // Упрощённый планировщик на стороне сервера: точная математика слотов
    // живёт в Python planner.py, здесь — быстрые предложения по настройкам.
    Result result;
    const Json preferences = database_.db().getPreferences(userId);
    std::vector<int> preferred;
    const Json preferredHours = preferences.get("preferred_hours");
    for (const auto& hour : preferredHours.items()) {
        preferred.push_back(static_cast<int>(hour.asInt()));
    }

    const Json window = args.get("window");
    const std::string start = window.getString("start", isoNow());
    const long long duration = args.getInt("duration_minutes", 60);
    const long long limit = args.getInt("limit", 3);

    // Часы, которые пользователь уже отметил как занятые (записи вида schedule)
    std::vector<std::string> busy;
    for (const auto& record : database_.db().listMemory(userId, 100)) {
        if (record.kind == "schedule") busy.push_back(record.text);
    }

    Json slots = Json::array();
    for (int hour = 8; hour <= 20 && static_cast<long long>(slots.size()) < limit; ++hour) {
        const bool isPreferred = std::find(preferred.begin(), preferred.end(), hour) != preferred.end();
        Json slot = Json::object();
        slot.set("start", Json(start.substr(0, 11) + (hour < 10 ? "0" : "") + std::to_string(hour) + ":00:00Z"));
        slot.set("duration_minutes", Json(duration));
        slot.set("score", Json(isPreferred ? 0.85 : 0.55));
        slot.set("reason", Json(isPreferred ? "удобное время" : "свободно"));
        slots.push(slot);
    }

    result.ok = true;
    result.data.set("duration_minutes", Json(duration));
    result.data.set("slots", slots);
    result.data.set("busy_count", Json(static_cast<long long>(busy.size())));
    return result;
}

ToolManager::Result ToolManager::httpTool(const std::string& endpoint,
                                          const Json& payload,
                                          const Json& sandboxFallback) const {
    Result result;
    if (config_.toolsMode != "http" || endpoint.empty()) {
        result.ok = true;
        result.data = sandboxFallback;
        return result;
    }

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!config_.aiServiceToken.empty()) headers["X-Aura-Token"] = config_.aiServiceToken;

    std::string error;
    const net::HttpResponse response =
        net::httpRequest("POST", endpoint, headers, payload.dump(), config_.aiTimeoutMs, error);
    if (!response.ok()) {
        AURA_LOG(log::Level::Warn, "tools") << "внешний API ответил " << response.status << ": " << error;
        result.ok = true;  // деградируем до sandbox, но не роняем сценарий
        result.data = sandboxFallback;
        result.data.set("degraded", Json(true));
        result.data.set("error", Json(error));
        return result;
    }

    const Json parsed = Json::parse(response.body, &error);
    result.ok = true;
    result.data = parsed.isNull() ? sandboxFallback : parsed;
    return result;
}

}  // namespace aura
