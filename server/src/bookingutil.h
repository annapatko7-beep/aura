// bookingutil.h — общие проверки ввода и мелкие помощники страниц записи
// (bookingmanager.cpp, packages.cpp). Внутренний заголовок сервера.
#pragma once

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>

#include "selfvoy/json.h"
#include "selfvoy/memorymanager.h"  // parseIsoUtc / isoUtc
#include "selfvoy/slotspost.h"

namespace selfvoy::bookingutil {

inline std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// Длина в символах UTF-8 (продолжающие байты 10xxxxxx не считаются).
inline std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (const unsigned char ch : value) {
        if ((ch & 0xC0) != 0x80) ++count;
    }
    return count;
}

inline bool validText(const std::string& value, std::size_t maxChars, bool multiline) {
    if (utf8Length(value) > maxChars) return false;
    for (const unsigned char ch : value) {
        if (ch == 0x7F) return false;
        if (ch < 0x20 && !(multiline && (ch == '\n' || ch == '\r' || ch == '\t'))) return false;
    }
    return true;
}

inline bool plausibleEmail(const std::string& email) {
    if (email.size() < 3 || email.size() > 254) return false;
    const std::size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= email.size()) return false;
    if (email.find('@', at + 1) != std::string::npos) return false;
    for (const unsigned char ch : email) {
        if (ch <= 0x20 || ch == 0x7F || ch == '<' || ch == '>' || ch == '"' || ch == ',' || ch == ';') return false;
    }
    return true;
}

inline std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Числовое поле payload: отсутствует → fallback; не целое или вне [min, max] → false.
inline bool intField(const Json& payload, const std::string& key, long long min, long long max, long long& out) {
    if (!payload.contains(key)) return true;
    const Json value = payload.get(key);
    if (!value.isNumber()) return false;
    const double number = value.asDouble();
    if (!(number >= -9.0e15 && number <= 9.0e15)) return false;
    if (number != static_cast<double>(static_cast<long long>(number))) return false;
    out = static_cast<long long>(number);
    return out >= min && out <= max;
}

// Первые maxChars символов UTF-8 (без разрыва многобайтового символа).
inline std::string utf8Prefix(const std::string& value, std::size_t maxChars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) return value.substr(0, i);
            ++chars;
        }
    }
    return value;
}

inline std::string moneyOf(const Json& payment) {
    return slotspost::moneyLabel(payment.getInt("amount"), payment.getString("currency", "RUB"));
}

inline Json withStatus(const Json& payment, const std::string& status) {
    Json updated = payment;
    updated.set("status", Json(status));
    return updated;
}

inline bool validDay(const std::string& day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
    for (std::size_t i = 0; i < day.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (std::isdigit(static_cast<unsigned char>(day[i])) == 0) return false;
    }
    std::time_t probe = 0;
    return parseIsoUtc(day + "T00:00:00Z", probe);
}

// Местное время YYYY-MM-DDTHH:MM.
inline bool validLocal(const std::string& local) {
    return local.size() == 16 && local[10] == 'T' && local[13] == ':' && validDay(local.substr(0, 10)) &&
           std::isdigit(static_cast<unsigned char>(local[11])) != 0 &&
           std::isdigit(static_cast<unsigned char>(local[12])) != 0 &&
           std::isdigit(static_cast<unsigned char>(local[14])) != 0 &&
           std::isdigit(static_cast<unsigned char>(local[15])) != 0;
}

// «чт, 15 октября 2026, 10:30 (Europe/Moscow)» — время встречи в письме: местное
// время владельца из AI (local), иначе UTC.
inline std::string whenText(const Json& occurrence, const std::string& timezone) {
    static const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                          "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
    static const char* const kDays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
    std::string local = occurrence.getString("local");
    std::string zone = timezone.empty() ? "UTC" : timezone;
    if (!validLocal(local)) {
        local = occurrence.getString("start").substr(0, 16);
        zone = "UTC";
        if (!validLocal(local)) return occurrence.getString("start");
    }
    std::time_t date = 0;
    parseIsoUtc(local.substr(0, 10) + "T00:00:00Z", date);
    std::tm parts{};
    gmtime_r(&date, &parts);
    return std::string(kDays[parts.tm_wday]) + ", " + std::to_string(parts.tm_mday) + " " + kMonths[parts.tm_mon] +
           " " + std::to_string(parts.tm_year + 1900) + ", " + local.substr(11, 5) + " (" + zone + ")";
}

// Ссылка https:// без пробелов и управляющих символов, ≤ 500 (оплата F4, чек F10).
inline bool validHttpsUrl(const std::string& url) {
    if (url.size() > 500 || url.size() <= 8 || url.rfind("https://", 0) != 0) return false;
    return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
        return ch <= 0x20 || ch == 0x7F || ch == '"' || ch == '<' || ch == '>' || ch == '\\';
    });
}

}  // namespace selfvoy::bookingutil
