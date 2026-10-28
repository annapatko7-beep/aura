// selfvoy/slotspost.cpp — пост «свободные окна» (booking.slotsPost, F6).
#include "selfvoy/slotspost.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "selfvoy/bookingmanager.h"
#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/memorymanager.h"  // isoUtc / parseIsoUtc
#include "selfvoy/protocol.h"

namespace selfvoy {

namespace slotspost {

namespace {

// Разряды через пробел: 2500 -> «2 500».
std::string groupDigits(long long value) {
    const std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(' ');
        out.push_back(digits[i]);
    }
    return out;
}

std::string daysPhrase(int days) {
    if (days == 1) return "ближайший день";
    const int tail = days % 100;
    const int last = days % 10;
    const char* word = (tail >= 11 && tail <= 14) ? "дней" : last == 1 ? "день" : (last >= 2 && last <= 4) ? "дня" : "дней";
    return "ближайшие " + std::to_string(days) + " " + word;
}

}  // namespace

std::string dayLabel(const std::string& day) {
    static const char* const kDays[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
    static const char* const kMonths[] = {"янв", "фев", "мар", "апр", "мая", "июн",
                                          "июл", "авг", "сен", "окт", "ноя", "дек"};
    std::time_t date = 0;
    if (day.size() != 10 || !parseIsoUtc(day + "T00:00:00Z", date)) return "";
    std::tm parts{};
    gmtime_r(&date, &parts);
    return std::string(kDays[parts.tm_wday]) + ", " + std::to_string(parts.tm_mday) + " " + kMonths[parts.tm_mon];
}

std::string durationLabel(int minutes) {
    if (minutes <= 0) return "";
    if (minutes < 60) return std::to_string(minutes) + " мин";
    std::string out = std::to_string(minutes / 60) + " ч";
    if (minutes % 60 != 0) out += " " + std::to_string(minutes % 60) + " мин";
    return out;
}

std::string moneyLabel(long long minor, const std::string& currency) {
    if (minor <= 0) return "";
    std::string symbol = currency;
    if (currency == "RUB") symbol = "₽";
    else if (currency == "USD") symbol = "$";
    else if (currency == "EUR") symbol = "€";
    else if (currency == "KZT") symbol = "₸";
    else if (currency == "BYN") symbol = "Br";
    std::string out = groupDigits(minor / 100);
    if (minor % 100 != 0) {
        char cents[8];
        std::snprintf(cents, sizeof(cents), ",%02lld", minor % 100);
        out += cents;
    }
    return out + " " + symbol;
}

std::string render(const Input& input) {
    const bool plain = input.format == "plain";
    std::string details = durationLabel(input.durationMinutes);
    const std::string money = moneyLabel(input.price, input.currency);
    if (!money.empty()) details += (details.empty() ? "" : ", ") + money;
    std::string out = "Свободные окна: " + input.title;
    if (!details.empty()) out += " (" + details + ")";
    if (input.utc && !input.locals.empty()) out += ", время UTC";
    out += "\n";
    if (!plain) out += "\n";

    if (input.locals.empty()) {
        out += "На " + daysPhrase(input.days) + " свободных окон нет\n";
    } else {
        std::size_t i = 0;
        while (i < input.locals.size()) {
            const std::string day = input.locals[i].substr(0, 10);
            std::vector<std::string> times;
            while (i < input.locals.size() && input.locals[i].compare(0, 10, day) == 0) {
                times.push_back(input.locals[i].size() >= 16 ? input.locals[i].substr(11, 5) : input.locals[i]);
                ++i;
            }
            const std::string label = dayLabel(day);
            out += (label.empty() ? day : label) + ": ";
            const std::size_t shown = std::min<std::size_t>(times.size(), kMaxPerDay);
            for (std::size_t k = 0; k < shown; ++k) out += (k == 0 ? "" : ", ") + times[k];
            if (times.size() > shown) out += " и ещё " + std::to_string(times.size() - shown);
            out += "\n";
        }
    }
    if (!input.url.empty()) {
        if (!plain) out += "\n";
        out += (input.format == "vk" ? "Запись онлайн: " : "Записаться: ") + input.url + "\n";
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace slotspost

BookingManager::Result BookingManager::slotsPost(long long ownerId, const Json& payload) {
    long long days = 7;
    if (payload.contains("days")) {
        const Json value = payload.get("days");
        if (!value.isNumber() || value.asDouble() != static_cast<double>(static_cast<long long>(value.asDouble()))) {
            return Result::failure(protocol::code::kBadRequest, "days — целое число от 1 до 14");
        }
        days = static_cast<long long>(value.asDouble());
    }
    if (days < 1 || days > 14) return Result::failure(protocol::code::kBadRequest, "days — целое число от 1 до 14");
    const std::string format = payload.getString("format", "telegram");
    if (format != "telegram" && format != "vk" && format != "plain") {
        return Result::failure(protocol::code::kBadRequest, "format — telegram, vk или plain");
    }
    const long long pageId = payload.getInt("page_id");
    const auto page = pageId > 0 ? database_.db().findBookingPage(pageId) : std::nullopt;
    if (!page || page->ownerId != ownerId || page->status == "archived") {
        return Result::failure(protocol::code::kNotFound, "страница записи не найдена");
    }
    if (page->status != "active") {
        return Result::failure(protocol::code::kConflict, "запись на странице на паузе — включите её, чтобы опубликовать окна");
    }
    if (!ownerAccepts(ownerId)) {
        return Result::failure(protocol::code::kForbidden, "подтвердите email — до этого гости не видят страницу");
    }
    if (limited("booking.slotsPost:" + std::to_string(ownerId), kSlotsPostPerHour, 3600)) {
        return Result::failure(protocol::code::kForbidden, "слишком много запросов, попробуйте позже");
    }

    // Те же окна, что гость видит в booking.get: кандидаты AI + запас, занятость, лимит в день.
    std::vector<Offer> candidates;
    if (!offers(*page, candidates)) {
        return Result::failure(protocol::code::kUpstream, "AI-сервис недоступен — не удалось подобрать свободное время");
    }
    const std::time_t until = std::time(nullptr) + static_cast<std::time_t>(days) * 86400;
    slotspost::Input input;
    input.title = page->title;
    input.durationMinutes = page->durationMinutes;
    input.price = page->price;  // цена страницы (F4); 0 — в заголовке не показывается
    input.currency = page->currency.empty() ? std::string("RUB") : page->currency;
    input.days = static_cast<int>(days);
    input.format = format;
    input.url = config_.publicUrl.empty() ? std::string() : config_.publicUrl + "/#/b/" + page->slug;
    std::vector<Offer> windows;
    for (const auto& offer : available(*page, candidates)) {
        if (offer.start <= until) windows.push_back(offer);
    }
    // Местное время владельца (local от AI, проверено в offers); если его нет хоть у
    // одного окна — все окна в UTC, чтобы не смешивать пояса в одном посте.
    for (const auto& offer : windows) {
        if (offer.local.size() != 16) input.utc = true;
    }
    for (const auto& offer : windows) {
        input.locals.push_back(input.utc ? isoUtc(offer.start).substr(0, 16) : offer.local);
    }

    Result result;
    result.ok = true;
    result.payload.set("text", Json(slotspost::render(input)));
    result.payload.set("slots", Json(static_cast<long long>(input.locals.size())));
    result.payload.set("url", Json(input.url));
    return result;
}

}  // namespace selfvoy
