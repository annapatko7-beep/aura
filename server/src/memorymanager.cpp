// aura/memorymanager.cpp
#include "aura/memorymanager.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

#include "aura/log.h"
#include "aura/net.h"

namespace aura {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Простая релевантность: совпадение подстрок запроса в тексте записи + вес.
double relevance(const std::string& text, const std::vector<std::string>& tokens) {
    if (tokens.empty()) return 0.0;
    const std::string haystack = lower(text);
    double score = 0.0;
    for (const auto& token : tokens) {
        if (token.size() < 4) continue;
        if (haystack.find(lower(token)) != std::string::npos) score += 1.0;
        else if (haystack.find(lower(token).substr(0, 4)) != std::string::npos) score += 0.6;
    }
    return score;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (const char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        // UTF-8: буквы кириллицы/латиницы и цифры
        if (std::isalnum(byte) != 0 || byte >= 0x80) {
            current.push_back(ch);
        } else if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

}  // namespace

MemoryManager::MemoryManager(DatabaseManager& database, const Config& config)
    : database_(database), config_(config) {}

Json MemoryManager::load(long long userId, const std::string& query, int limit) {
    std::vector<MemoryRecord> records = database_.db().listMemory(userId, limit > 0 ? limit * 2 : 40);
    const std::vector<std::string> tokens = words(query);
    if (!tokens.empty()) {
        std::stable_sort(records.begin(), records.end(),
                         [&tokens](const MemoryRecord& a, const MemoryRecord& b) {
                             return relevance(a.text, tokens) + a.weight >
                                    relevance(b.text, tokens) + b.weight;
                         });
    }
    if (limit > 0 && records.size() > static_cast<std::size_t>(limit)) {
        records.resize(static_cast<std::size_t>(limit));
    }

    Json entries = Json::array();
    for (const auto& record : records) entries.push(record.toJson());
    Json result = Json::object();
    result.set("user_id", Json(userId));
    result.set("entries", entries);
    return result;
}

Json MemoryManager::context(long long userId, const std::string& focusMessage) {
    Json context = Json::object();
    context.set("user_id", Json(userId));
    if (const auto user = database_.db().findUserById(userId)) {
        context.set("email", Json(user->email));
        context.set("display_name", Json(user->displayName));
        context.set("timezone", Json(user->timezone));
    }
    context.set("now", Json(isoNow()));
    context.set("message", Json(focusMessage));
    context.set("preferences", preferences(userId));
    context.set("memory", load(userId, focusMessage, 20).get("entries"));

    Json calendar = Json::array();
    for (const auto& record : schedule(userId)) {
        Json event = Json::object();
        event.set("title", Json(record.text));
        event.set("source", Json("user_memory"));
        event.set("tags", [&record] {
            Json tags = Json::array();
            for (const auto& tag : record.tags) tags.push(Json(tag));
            return tags;
        }());
        calendar.push(event);
    }
    context.set("calendar", calendar);
    return context;
}

std::size_t MemoryManager::remember(long long userId, const Json& updates) {
    if (!updates.isArray()) return 0;
    std::size_t saved = 0;
    for (const auto& entry : updates.items()) {
        const std::string text = entry.getString("text");
        if (text.empty()) continue;
        std::vector<std::string> tags;
        const Json tagArray = entry.get("tags");
        for (const auto& tag : tagArray.items()) {
            if (!tag.asString().empty()) tags.push_back(tag.asString());
        }
        const DatabaseError result = database_.db().upsertMemory(
            userId, entry.getString("kind", "fact"), text, entry.getDouble("weight", 1.0), tags);
        if (result.ok) {
            ++saved;
        } else {
            AURA_LOG(log::Level::Warn, "memory") << "не удалось сохранить факт: " << result.message;
        }
    }
    AURA_LOG(log::Level::Debug, "memory") << "user=" << userId << " сохранено фактов: " << saved;
    return saved;
}

Json MemoryManager::extractLocally(const std::string& message) {
    static const std::vector<std::pair<const char*, const char*>> patterns = {
        {R"(я\s+(люблю|не люблю|предпочитаю|ем|не ем|живу|работаю)\s+([^.;!?]{2,60}))", "preference"},
        {R"(у меня\s+(аллергия на|есть|нет)\s+([^.;!?]{2,60}))", "fact"},
        {R"(мо(?:й|я|ё)\s+(друг|подруга|коллега|начальник)\s+([А-ЯЁA-Z][а-яёa-zA-Z]{1,30}))", "contact"},
    };

    Json entries = Json::array();
    for (const auto& pattern : patterns) {
        try {
            const std::regex expression(pattern.first, std::regex::icase);
            auto begin = std::sregex_iterator(message.begin(), message.end(), expression);
            const auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                const std::smatch& match = *it;
                std::string text;
                for (std::size_t group = 1; group < match.size(); ++group) {
                    if (match[group].matched) {
                        if (!text.empty()) text += " ";
                        text += match[group].str();
                    }
                }
                if (text.empty()) continue;
                Json entry = Json::object();
                entry.set("kind", Json(pattern.second));
                entry.set("text", Json(text));
                Json tags = Json::array();
                tags.push(Json("extracted"));
                entry.set("tags", tags);
                entries.push(entry);
            }
        } catch (const std::regex_error& error) {
            AURA_LOG(log::Level::Error, "memory") << "regex: " << error.what();
        }
    }
    return entries;
}

Json MemoryManager::extract(long long userId, const std::string& message, bool commit, std::string& error) {
    Json request = Json::object();
    request.set("user_key", Json(userId != 0 ? std::to_string(userId) : std::string("anonymous")));
    request.set("message", Json(message));
    request.set("commit", Json(commit));
    Json context = Json::object();
    context.set("user_id", Json(userId));
    context.set("message", Json(message));
    request.set("context", context);

    std::map<std::string, std::string> headers;
    if (!config_.aiServiceToken.empty()) headers["X-Aura-Token"] = config_.aiServiceToken;

    std::string url = config_.aiServiceUrl;
    if (!url.empty() && url.back() == '/') url.pop_back();
    const net::HttpResponse response =
        net::httpRequest("POST", url + "/v1/memory/extract", headers, request.dump(), config_.aiTimeoutMs, error);

    Json result = Json::object();
    if (response.ok()) {
        const Json parsed = Json::parse(response.body, &error);
        if (!parsed.isNull()) {
            const Json entries = parsed.get("entries");
            if (commit) remember(userId, entries);
            result.set("entries", entries);
            result.set("source", Json("ai"));
            return result;
        }
    }

    AURA_LOG(log::Level::Warn, "memory")
        << "AI-сервис недоступен (" << error << "), извлекаю факты локально";
    const Json local = extractLocally(message);
    if (commit) remember(userId, local);
    result.set("entries", local);
    result.set("source", Json("local"));
    error.clear();
    return result;
}

Json MemoryManager::preferences(long long userId) {
    Json preferences = database_.db().getPreferences(userId);
    if (!preferences.isObject()) preferences = Json::object();
    preferences.set("user_id", Json(userId));
    return preferences;
}

bool MemoryManager::setPreferences(long long userId, const Json& preferences, std::string& error) {
    Json toSave = preferences;
    // Онбординг-опрос после регистрации: как только пользователь прислал
    // хотя бы одно поле анкеты (день рождения, аллергии, диета, город, …),
    // помечаем опрос пройденным — клиенты по этому флагу показывают анкету.
    static const char* kOnboardingKeys[] = {"birthday", "allergies", "diet",   "transport",
                                            "city",     "budget_limit", "preferred_hours",
                                            "work_hours"};
    if (!toSave.contains("onboarded")) {
        for (const char* key : kOnboardingKeys) {
            if (toSave.contains(key)) {
                toSave.set("onboarded", Json(true));
                break;
            }
        }
    }
    const DatabaseError result = database_.db().setPreferences(userId, toSave);
    if (!result.ok) {
        error = result.message;
        return false;
    }
    return true;
}

std::vector<MemoryRecord> MemoryManager::schedule(long long userId) {
    std::vector<MemoryRecord> result;
    for (const auto& record : database_.db().listMemory(userId, 200)) {
        const bool isSchedule = record.kind == "schedule" ||
                                std::find(record.tags.begin(), record.tags.end(), "reminder") != record.tags.end();
        if (isSchedule) result.push_back(record);
    }
    return result;
}

}  // namespace aura
