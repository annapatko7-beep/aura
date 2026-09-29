// aura/log.h — простой потокобезопасный логгер.
#pragma once

#include <sstream>
#include <string>

namespace aura::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void setLevel(Level level);
void setLevel(const std::string& name);
Level level();
bool enabled(Level candidate);

void write(Level level, const std::string& component, const std::string& message);

class Builder {
public:
    Builder(Level level, std::string component) : level_(level), component_(std::move(component)) {}
    ~Builder() {
        if (enabled(level_)) write(level_, component_, stream_.str());
    }
    template <typename T>
    Builder& operator<<(const T& value) {
        if (enabled(level_)) stream_ << value;
        return *this;
    }

private:
    Level level_;
    std::string component_;
    std::ostringstream stream_;
};

#define AURA_LOG(level, component) ::aura::log::Builder(level, component)

}  // namespace aura::log
