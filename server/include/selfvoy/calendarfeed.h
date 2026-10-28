// selfvoy/calendarfeed.h — ICS-подписка владельца (F6): calendar.feed / feedReset /
// feedDisable и HTTP GET /ics/<токен>.ics на порту WebSocket.
//
// Владелец получает постоянную ссылку-подписку (Google Календарь, Apple Календарь,
// Outlook — «добавить по URL»): активные встречи записей (booking.*) и принятые
// приглашения (invite.*) от −30 до +180 суток. Токен ссылки — 32 случайных байта
// (base64url, 43 символа); в БД только sha256 (users.ics_token_hash), сам токен
// виден один раз — в ответе calendar.feedReset. Новый reset — старая ссылка
// перестаёт работать; feedDisable — подписка выключена (404).
#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

#include "selfvoy/json.h"

namespace selfvoy {

class DatabaseManager;
struct Config;

// RFC 5545: экранирование TEXT и складывание строк содержимого.
namespace ics {

// TEXT (§3.3.11): «\» → «\\», «;» → «\;», «,» → «\,», перевод строки (LF, CRLF, CR) → «\n».
// Прочие управляющие символы (кроме табуляции) выбрасываются — в TEXT они запрещены.
std::string escapeText(const std::string& value);

// Строка содержимого (§3.1) без завершающего CRLF → строки не длиннее 75 октетов,
// продолжения начинаются с пробела; многобайтные символы UTF-8 не разрываются.
// Результат всегда заканчивается CRLF.
std::string foldLine(const std::string& line);

// Момент в формате DATE-TIME UTC: 20261021T100000Z.
std::string utcStamp(std::time_t moment);

}  // namespace ics

// Ответ HTTP/1.1 с Connection: close (тело и Content-Length; extraHeaders — строки с CRLF).
std::string httpResponse(int status, const std::string& reason, const std::string& contentType,
                         const std::string& body, const std::string& extraHeaders = "");

class CalendarFeed {
public:
    struct Result {
        bool ok = false;
        std::string code;
        std::string message;
        Json payload = Json::object();

        static Result failure(std::string code, std::string message) {
            Result result;
            result.code = std::move(code);
            result.message = std::move(message);
            return result;
        }
    };

    static constexpr int kFetchPerMinute = 60;  // GET /ics/… с одного IP
    static constexpr int kResetPerHour = 20;    // новых ссылок в час на пользователя
    static constexpr int kPastDays = 30;
    static constexpr int kFutureDays = 180;
    static constexpr const char* kPathPrefix = "/ics/";

    CalendarFeed(DatabaseManager& database, const Config& config);

    // calendar.feed → {enabled, url, path}: токен не хранится, поэтому url/path пусты
    // (новую ссылку выдаёт только feedReset).
    Result feed(long long userId);
    // calendar.feedReset → новый токен: {enabled: true, url, path}.
    Result reset(long long userId);
    // calendar.feedDisable → {enabled: false}.
    Result disable(long long userId);

    // HTTP-запрос не-WebSocket с целью /ics/…: полный ответ HTTP/1.1 (200 text/calendar,
    // 404, 405, 429); "" — цель не /ics/ (маршрут не наш).
    std::string serveHttp(const std::string& method, const std::string& target, const std::string& ip);

    // Текст календаря владельца на момент now (для HTTP и юнит-тестов).
    std::string render(long long userId, std::time_t now);

    // Токен из пути /ics/<токен>.ics → hex sha256; "" — заведомо неверный путь или токен.
    static std::string tokenHashFromPath(const std::string& target);

private:
    bool limited(const std::string& key, int perWindow, int windowSeconds);
    std::string pathFor(const std::string& token) const;

    DatabaseManager& database_;
    const Config& config_;

    struct Window {
        std::chrono::steady_clock::time_point start;
        int count = 0;
    };
    std::mutex limitsMutex_;
    std::unordered_map<std::string, Window> limits_;
};

}  // namespace selfvoy
