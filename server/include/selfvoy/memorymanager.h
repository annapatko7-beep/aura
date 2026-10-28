// selfvoy/memorymanager.h — загрузка и обновление долговременной памяти пользователя.
#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "selfvoy/config.h"
#include "selfvoy/databasemanager.h"
#include "selfvoy/json.h"

namespace selfvoy {

// «YYYY-MM-DDTHH:MM[:SS][.fff](Z|±HH:MM)» → секунды UTC; false, если это не ISO-метка.
bool parseIsoUtc(const std::string& value, std::time_t& out);
// Секунды UTC → «YYYY-MM-DDTHH:MM:SSZ».
std::string isoUtc(std::time_t t);

class MemoryManager {
public:
    MemoryManager(DatabaseManager& database, const Config& config);

    // Записи памяти, отсортированные по релевантности запросу.
    Json load(long long userId, const std::string& query, int limit = 20);

    // Готовый контекст для AI-сервиса: память + настройки + расписание.
    Json context(long long userId, const std::string& focusMessage);

    // Сохраняет записи, которые вернул AI-сервис в memory_updates.
    // Сохраняет записи памяти, проверяя каждую: kind — fact|preference|schedule|contact,
    // текст — до maxText символов, вес 0..5, до 10 тегов по 40 байт. maxEntries > 0 —
    // не больше стольких записей (предложения LLM: она не источник истины).
    std::size_t remember(long long userId, const Json& updates, std::size_t maxEntries = 0,
                         std::size_t maxText = 1000);

    // Просит AI-сервис извлечь факты из фразы (POST /v1/memory/extract).
    // При недоступном сервисе работает локальное правило (см. extractLocally).
    Json extract(long long userId, const std::string& message, bool commit, std::string& error);

    // Локальное извлечение фактов (тот же набор правил, что в Python memory.py).
    static Json extractLocally(const std::string& message);

    Json preferences(long long userId);
    // invalid=true — ошибка во входных данных (bad_request), иначе — сбой хранилища.
    bool setPreferences(long long userId, const Json& preferences, std::string& error, bool* invalid = nullptr);
    // Приватность Selfvoy: что он раскрывает чужому Selfvoy в переговорах.
    // Ключи share_free_slots_only, hide_event_titles, allow_place_choice (по умолчанию все true).
    Json privacy(long long userId);
    static Json defaultPrivacy();

    std::vector<MemoryRecord> schedule(long long userId);
    // Занятые интервалы [start, end) из расписания (те же, что уходят в AI как calendar).
    // Плюс будущие встречи записей гостей (booking.*) и время, которое держится за гостем
    // из листа ожидания (F8).
    std::vector<std::pair<std::time_t, std::time_t>> busy(long long userId);
    // Будущие активные встречи со страниц записи: событие календаря для AI и интервал.
    std::vector<std::pair<Json, std::pair<std::time_t, std::time_t>>> bookingEvents(long long userId);

private:
    DatabaseManager& database_;
    const Config& config_;
};

}  // namespace selfvoy
