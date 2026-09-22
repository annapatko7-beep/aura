// aura/log.cpp
#include "aura/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace aura::log {

namespace {
Level g_level = Level::Info;
std::mutex g_mutex;

const char* name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Off: return "OFF  ";
    }
    return "?    ";
}
}  // namespace

void setLevel(Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

void setLevel(const std::string& text) {
    if (text == "debug") setLevel(Level::Debug);
    else if (text == "info") setLevel(Level::Info);
    else if (text == "warn") setLevel(Level::Warn);
    else if (text == "error") setLevel(Level::Error);
    else if (text == "off") setLevel(Level::Off);
}

Level level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

bool enabled(Level candidate) { return candidate >= level(); }

void write(Level level, const std::string& component, const std::string& message) {
    if (!enabled(level)) return;
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const int millis =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
                         1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%s.%03dZ %-5s [%s] %s\n", stamp, millis, name(level), component.c_str(),
                 message.c_str());
    std::fflush(stderr);
}

}  // namespace aura::log
