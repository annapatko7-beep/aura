// selfvoy/slotspost.h — текст поста «свободные окна» (booking.slotsPost, F6).
//
// Сборка текста отделена от BookingManager: её проверяют юнит-тесты без AI и БД.
// Окна — местное время владельца «YYYY-MM-DDTHH:MM» (как поле local у слотов AI).
#pragma once

#include <string>
#include <vector>

namespace selfvoy::slotspost {

struct Input {
    std::string title;
    int durationMinutes = 0;
    long long price = 0;            // минимальные единицы; 0 — цену не показывать
    std::string currency = "RUB";
    std::vector<std::string> locals;  // местное время окон по возрастанию
    bool utc = false;               // местное время неизвестно — окна в UTC (пометка в заголовке)
    int days = 7;
    std::string url;                // ссылка страницы; "" — строки со ссылкой нет
    std::string format = "telegram";  // telegram | vk | plain
};

constexpr int kMaxPerDay = 8;  // времён в строке дня, остальные — «и ещё N»

// «Пн, 21 окт» для даты YYYY-MM-DD; "" — неверная дата.
std::string dayLabel(const std::string& day);
// «50 мин», «1 ч», «1 ч 30 мин».
std::string durationLabel(int minutes);
// «2 500 ₽», «99,50 $» — сумма в минимальных единицах.
std::string moneyLabel(long long minor, const std::string& currency);

std::string render(const Input& input);

}  // namespace selfvoy::slotspost
