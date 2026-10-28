// selfvoy/calendarfeed.cpp
#include "selfvoy/calendarfeed.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

#include "selfvoy/config.h"
#include "selfvoy/crypto.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/log.h"
#include "selfvoy/memorymanager.h"  // isoUtc / parseIsoUtc
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace {

constexpr std::size_t kTokenBytes = 32;   // 256 бит случайности
constexpr std::size_t kTokenLength = 43;  // base64url без '=' от 32 байт
constexpr std::size_t kLineOctets = 75;   // RFC 5545 §3.1
constexpr int kMaxBookings = 2000;        // записей владельца в выборке (новые сверху)
constexpr int kMaxInvites = 500;

struct Event {
    std::string uid;
    std::time_t start = 0;
    std::time_t end = 0;
    std::string summary;
    std::string description;
    std::string stamp;  // ISO UTC последнего изменения ("" — момент выдачи)
};

}  // namespace

// ------------------------------------------------------------------ RFC 5545

namespace ics {

std::string escapeText(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case ';': out += "\\;"; break;
            case ',': out += "\\,"; break;
            case '\r':
                if (i + 1 < value.size() && value[i + 1] == '\n') ++i;
                out += "\\n";
                break;
            case '\n': out += "\\n"; break;
            default:
                if ((ch < 0x20 && ch != '\t') || ch == 0x7F) break;  // CTL в TEXT запрещены
                out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string foldLine(const std::string& line) {
    std::string out;
    out.reserve(line.size() + line.size() / kLineOctets * 3 + 2);
    std::size_t position = 0;
    bool first = true;
    while (position < line.size()) {
        // Продолжение начинается с пробела, он входит в 75 октетов строки.
        const std::size_t room = first ? kLineOctets : kLineOctets - 1;
        std::size_t cut = std::min(line.size(), position + room);
        // Не режем многобайтный символ: граница — перед ведущим байтом (не 10xxxxxx).
        if (cut < line.size()) {
            while (cut > position && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
            if (cut == position) cut = std::min(line.size(), position + room);  // битый UTF-8: режем как есть
        }
        if (!first) out.push_back(' ');
        out.append(line, position, cut - position);
        out += "\r\n";
        position = cut;
        first = false;
    }
    if (first) out += "\r\n";  // пустая строка
    return out;
}

std::string utcStamp(std::time_t moment) {
    std::tm parts{};
    gmtime_r(&moment, &parts);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02dZ", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return buffer;
}

}  // namespace ics

std::string httpResponse(int status, const std::string& reason, const std::string& contentType,
                         const std::string& body, const std::string& extraHeaders) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << extraHeaders << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

// ------------------------------------------------------------------ CalendarFeed

CalendarFeed::CalendarFeed(DatabaseManager& database, const Config& config) : database_(database), config_(config) {}

bool CalendarFeed::limited(const std::string& key, int perWindow, int windowSeconds) {
    // Тот же механизм, что у гостевых методов: в PostgreSQL — общая таблица rate_limits
    // (все экземпляры сервера), во встроенной БД — счётчик в памяти.
    if (database_.db().sharedRateLimits()) return database_.db().rateLimitHit(key, windowSeconds) > perWindow;
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(windowSeconds);
    std::lock_guard<std::mutex> lock(limitsMutex_);
    if (limits_.size() > 10000) {
        for (auto it = limits_.begin(); it != limits_.end();) {
            it = now - it->second.start > std::chrono::hours(1) ? limits_.erase(it) : std::next(it);
        }
    }
    auto& entry = limits_[key];
    if (entry.count == 0 || now - entry.start > window) {
        entry.start = now;
        entry.count = 0;
    }
    if (entry.count >= perWindow) return true;
    ++entry.count;
    return false;
}

std::string CalendarFeed::pathFor(const std::string& token) const {
    return std::string(kPathPrefix) + token + ".ics";
}

std::string CalendarFeed::tokenHashFromPath(const std::string& target) {
    std::string path = target.substr(0, target.find_first_of("?#"));
    const std::string prefix = kPathPrefix;
    const std::string suffix = ".ics";
    if (path.size() != prefix.size() + kTokenLength + suffix.size() || path.rfind(prefix, 0) != 0 ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return "";
    }
    const std::string token = path.substr(prefix.size(), kTokenLength);
    for (const unsigned char ch : token) {
        if (std::isalnum(ch) == 0 && ch != '-' && ch != '_') return "";
    }
    return crypto::toHex(crypto::sha256(token));
}

CalendarFeed::Result CalendarFeed::feed(long long userId) {
    Result result;
    result.ok = true;
    result.payload.set("enabled", Json(!database_.db().icsTokenHash(userId).empty()));
    result.payload.set("url", Json(std::string()));
    result.payload.set("path", Json(std::string()));
    return result;
}

CalendarFeed::Result CalendarFeed::reset(long long userId) {
    if (limited("calendar.feedReset:" + std::to_string(userId), kResetPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком часто — новую ссылку можно получить позже");
    }
    const std::string token = crypto::base64UrlEncode(crypto::randomBytes(kTokenBytes));
    const DatabaseError saved = database_.db().setIcsTokenHash(userId, crypto::toHex(crypto::sha256(token)));
    if (!saved.ok) {
        SELFVOY_LOG(log::Level::Warn, "calendar") << "ссылка подписки не сохранена: " << saved.message;
        return Result::failure(protocol::code::kInternal, "не удалось создать ссылку подписки");
    }
    const std::string path = pathFor(token);
    Result result;
    result.ok = true;
    result.payload.set("enabled", Json(true));
    result.payload.set("url", Json(config_.serverPublicUrl.empty() ? std::string() : config_.serverPublicUrl + path));
    result.payload.set("path", Json(path));
    return result;
}

CalendarFeed::Result CalendarFeed::disable(long long userId) {
    const DatabaseError saved = database_.db().setIcsTokenHash(userId, "");
    if (!saved.ok) return Result::failure(protocol::code::kInternal, "не удалось выключить подписку");
    Result result;
    result.ok = true;
    result.payload.set("enabled", Json(false));
    return result;
}

std::string CalendarFeed::serveHttp(const std::string& method, const std::string& target, const std::string& ip) {
    if (target.rfind(kPathPrefix, 0) != 0) return "";
    const std::string noStore = "Cache-Control: no-store\r\n";
    if (method != "GET") {
        return httpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed\n",
                            noStore + "Allow: GET\r\n");
    }
    // Лимит — до поиска токена: перебор ссылок упирается в него же.
    if (limited("ics:" + ip, kFetchPerMinute, 60)) {
        return httpResponse(429, "Too Many Requests", "text/plain; charset=utf-8", "too many requests\n",
                            noStore + "Retry-After: 60\r\n");
    }
    const std::string hash = tokenHashFromPath(target);
    const long long userId = hash.empty() ? 0 : database_.db().findUserByIcsToken(hash);
    const auto user = userId > 0 ? database_.db().findUserById(userId) : std::nullopt;
    if (!user || !user->active) {
        return httpResponse(404, "Not Found", "text/plain; charset=utf-8", "not found\n", noStore);
    }
    return httpResponse(200, "OK", "text/calendar; charset=utf-8", render(userId, std::time(nullptr)),
                        noStore + "Content-Disposition: inline; filename=\"selfvoy.ics\"\r\n");
}

std::string CalendarFeed::render(long long userId, std::time_t now) {
    const std::time_t from = now - static_cast<std::time_t>(kPastDays) * 86400;
    const std::time_t to = now + static_cast<std::time_t>(kFutureDays) * 86400;
    std::vector<Event> events;

    std::unordered_map<long long, std::string> titles;
    for (const auto& booking : database_.db().listBookings(userId, 0, true, kMaxBookings)) {
        auto title = titles.find(booking.pageId);
        if (title == titles.end()) {
            const auto page = database_.db().findBookingPage(booking.pageId);
            title = titles.emplace(booking.pageId, page ? page->title : std::string("Запись")).first;
        }
        const Json occurrences = booking.occurrences;  // items() — ссылка внутрь: держим копию
        for (const auto& occurrence : occurrences.items()) {
            Event event;
            if (occurrence.getString("status") != "active" ||
                !parseIsoUtc(occurrence.getString("start"), event.start) ||
                !parseIsoUtc(occurrence.getString("end"), event.end) || event.end <= event.start ||
                event.start < from || event.start > to) {
                continue;
            }
            event.uid = "booking-" + std::to_string(booking.id) + "-" + std::to_string(event.start) + "@selfvoy";
            event.summary = title->second + ": " + booking.guestName;
            event.description = booking.guestComment;
            event.stamp = booking.updatedAt;
            events.push_back(std::move(event));
        }
    }

    for (const auto& invite : database_.db().listInvites(userId, kMaxInvites)) {
        if (invite.status != "accepted" || !invite.chosen.isObject()) continue;
        Event event;
        if (!parseIsoUtc(invite.chosen.getString("start"), event.start) ||
            !parseIsoUtc(invite.chosen.getString("end"), event.end) || event.end <= event.start ||
            event.start < from || event.start > to) {
            continue;
        }
        event.uid = "invite-" + std::to_string(invite.id) + "-" + std::to_string(event.start) + "@selfvoy";
        event.summary = invite.guestName.empty() ? invite.topic : invite.topic + ": " + invite.guestName;
        event.description = invite.guestComment;
        event.stamp = invite.respondedAt;
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.start != b.start ? a.start < b.start : a.uid < b.uid;
    });

    const std::string issued = ics::utcStamp(now);
    std::string out;
    const auto line = [&out](const std::string& content) { out += ics::foldLine(content); };
    line("BEGIN:VCALENDAR");
    line("VERSION:2.0");
    line("PRODID:-//Selfvoy//Selfvoy Calendar//RU");
    line("CALSCALE:GREGORIAN");
    line("METHOD:PUBLISH");
    line("X-WR-CALNAME:Selfvoy");
    line("REFRESH-INTERVAL;VALUE=DURATION:PT1H");
    line("X-PUBLISHED-TTL:PT1H");
    for (const auto& event : events) {
        std::time_t changed = 0;
        line("BEGIN:VEVENT");
        line("UID:" + event.uid);
        line("DTSTAMP:" + (parseIsoUtc(event.stamp, changed) ? ics::utcStamp(changed) : issued));
        line("DTSTART:" + ics::utcStamp(event.start));
        line("DTEND:" + ics::utcStamp(event.end));
        line("SUMMARY:" + ics::escapeText(event.summary));
        if (!event.description.empty()) line("DESCRIPTION:" + ics::escapeText(event.description));
        line("STATUS:CONFIRMED");
        line("TRANSP:OPAQUE");
        line("END:VEVENT");
    }
    line("END:VCALENDAR");
    return out;
}

}  // namespace selfvoy
