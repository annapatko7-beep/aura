// aura/json.cpp — реализация JSON-парсера.
#include "aura/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace aura {

namespace {

class Parser {
public:
    // Ограничение глубины защищает стек: без него кадр с глубоко вложенным JSON
    // роняет процесс (рекурсия parseValue -> parseArray -> parseValue ...).
    static constexpr int kMaxDepth = 128;

    // Счётчик глубины живёт ровно столько, сколько разбор вложенного значения.
    struct DepthGuard {
        int& counter;
        explicit DepthGuard(int& value) : counter(value) { ++counter; }
        ~DepthGuard() { --counter; }
    };

    Parser(const std::string& text, std::string* error) : text_(text), error_(error) {}

    bool run(Json& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        if (pos_ != text_.size()) return fail("лишние данные после JSON");
        return true;
    }

private:
    bool fail(const std::string& message) {
        if (error_) {
            std::ostringstream out;
            out << message << " (позиция " << pos_ << ")";
            *error_ = out.str();
        }
        return false;
    }

    void skipWs() {
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const std::size_t len = std::strlen(word);
        if (text_.compare(pos_, len, word) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    bool parseValue(Json& out) {
        if (pos_ >= text_.size()) return fail("неожиданный конец строки");
        if (depth_ > kMaxDepth) return fail("слишком глубокая вложенность JSON");
        const char ch = text_[pos_];
        if (ch == '{') return parseObject(out);
        if (ch == '[') return parseArray(out);
        if (ch == '"') {
            std::string value;
            if (!parseString(value)) return false;
            out = Json(std::move(value));
            return true;
        }
        if (literal("true")) {
            out = Json(true);
            return true;
        }
        if (literal("false")) {
            out = Json(false);
            return true;
        }
        if (literal("null")) {
            out = Json(nullptr);
            return true;
        }
        return parseNumber(out);
    }

    bool parseNumber(Json& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '-' || ch == '+') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) return fail("ожидалось значение");
        const std::string raw = text_.substr(start, pos_ - start);
        // Проверяем грамматику JSON целиком: strtod молча отбрасывал хвост,
        // из-за чего «1.2.3» разбиралось как 1.2, а «+5» принималось.
        if (!isValidNumberLiteral(raw)) return fail("некорректное число: " + raw);
        char* end = nullptr;
        const double value = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() + raw.size()) return fail("некорректное число: " + raw);
        out = Json(value);
        return true;
    }

    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    static bool isValidNumberLiteral(const std::string& raw) {
        std::size_t i = 0;
        if (i < raw.size() && raw[i] == '-') ++i;
        if (i >= raw.size()) return false;
        if (raw[i] == '0') {
            ++i;
        } else if (raw[i] >= '1' && raw[i] <= '9') {
            while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') ++i;
        } else {
            return false;
        }
        if (i < raw.size() && raw[i] == '.') {
            ++i;
            const std::size_t digits = i;
            while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') ++i;
            if (i == digits) return false;
        }
        if (i < raw.size() && (raw[i] == 'e' || raw[i] == 'E')) {
            ++i;
            if (i < raw.size() && (raw[i] == '+' || raw[i] == '-')) ++i;
            const std::size_t digits = i;
            while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') ++i;
            if (i == digits) return false;
        }
        return i == raw.size();
    }

    bool parseString(std::string& out) {
        if (text_[pos_] != '"') return fail("ожидалась строка");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == '"') {
                ++pos_;
                return true;
            }
            if (ch == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) return fail("обрыв экранирования");
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int code = 0;
                        if (!parseHex4(code)) return false;
                        // Суррогатная пара
                        if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            const std::size_t save = pos_;
                            pos_ += 2;
                            unsigned int low = 0;
                            if (parseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save;
                            }
                        }
                        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;  // оборванная пара
                        appendUtf8(out, code);
                        break;
                    }
                    default:
                        return fail("неизвестное экранирование");
                }
                continue;
            }
            out.push_back(ch);
            ++pos_;
        }
        return fail("незакрытая строка");
    }

    bool parseHex4(unsigned int& out) {
        if (pos_ + 4 > text_.size()) return fail("ожидалось 4 hex-символа");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[pos_++];
            unsigned int digit = 0;
            if (ch >= '0' && ch <= '9') digit = static_cast<unsigned int>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = static_cast<unsigned int>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') digit = static_cast<unsigned int>(ch - 'A' + 10);
            else return fail("некорректный hex в \\u");
            out = (out << 4) | digit;
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parseArray(Json& out) {
        const DepthGuard guard(depth_);
        out = Json::array();
        ++pos_;  // '['
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            Json item;
            if (!parseValue(item)) return false;
            out.push(std::move(item));
            skipWs();
            if (pos_ >= text_.size()) return fail("незакрытый массив");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("ожидалась ',' или ']'");
        }
    }

    bool parseObject(Json& out) {
        const DepthGuard guard(depth_);
        out = Json::object();
        ++pos_;  // '{'
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("ожидалось ':'");
            ++pos_;
            skipWs();
            Json value;
            if (!parseValue(value)) return false;
            out.set(key, std::move(value));
            skipWs();
            if (pos_ >= text_.size()) return fail("незакрытый объект");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("ожидалась ',' или '}'");
        }
    }

    const std::string& text_;
    std::string* error_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

void dumpString(std::string& out, const std::string& value) {
    out.push_back('"');
    out += jsonEscape(value);
    out.push_back('"');
}

void dumpNumber(std::string& out, double value) {
    if (std::isnan(value) || std::isinf(value)) {
        out += "null";
        return;
    }
    const long long asInt = static_cast<long long>(value);
    if (static_cast<double>(asInt) == value && std::fabs(value) < 1e15) {
        out += std::to_string(asInt);
        return;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out += buffer;
}

}  // namespace

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
                } else {
                    out.push_back(ch);  // UTF-8 байты проходят как есть
                }
        }
    }
    return out;
}

Json Json::parse(const std::string& text, std::string* error) {
    Json result;
    Parser parser(text, error);
    if (!parser.run(result)) return Json();
    return result;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad = indent > 0 ? std::string(static_cast<std::size_t>(indent) * depth, ' ') : "";
    const std::string padInner =
        indent > 0 ? std::string(static_cast<std::size_t>(indent) * (depth + 1), ' ') : "";
    const char* newline = indent > 0 ? "\n" : "";

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: dumpNumber(out, number_); break;
        case Type::String: dumpString(out, string_); break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            out += newline;
            for (std::size_t i = 0; i < array_.size(); ++i) {
                out += padInner;
                array_[i].dumpTo(out, indent, depth + 1);
                if (i + 1 < array_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "]";
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            out += newline;
            for (std::size_t i = 0; i < object_.size(); ++i) {
                out += padInner;
                dumpString(out, object_[i].first);
                out += indent > 0 ? ": " : ":";
                object_[i].second.dumpTo(out, indent, depth + 1);
                if (i + 1 < object_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "}";
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& member : object_) {
        if (member.first == key) return member.second;
    }
    object_.emplace_back(key, Json());
    return object_.back().second;
}

void Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(key, std::move(value));
}

const Json* Json::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& member : object_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

bool Json::contains(const std::string& key) const { return find(key) != nullptr; }

void Json::erase(const std::string& key) {
    if (type_ != Type::Object) return;
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return;
        }
    }
}

std::vector<std::string> Json::keys() const {
    std::vector<std::string> result;
    if (type_ == Type::Object) {
        result.reserve(object_.size());
        for (const auto& member : object_) result.push_back(member.first);
    }
    return result;
}

std::string Json::getString(const std::string& key, const std::string& fallback) const {
    const Json* value = find(key);
    return value && value->isString() ? value->asString() : fallback;
}

long long Json::getInt(const std::string& key, long long fallback) const {
    const Json* value = find(key);
    return value && value->isNumber() ? value->asInt() : fallback;
}

double Json::getDouble(const std::string& key, double fallback) const {
    const Json* value = find(key);
    return value && value->isNumber() ? value->asDouble() : fallback;
}

bool Json::getBool(const std::string& key, bool fallback) const {
    const Json* value = find(key);
    return value && value->isBool() ? value->asBool() : fallback;
}

Json Json::get(const std::string& key) const {
    const Json* value = find(key);
    return value ? *value : Json();
}

void Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
}

std::size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const Json& Json::at(std::size_t index) const {
    static const Json nullJson;
    if (type_ != Type::Array || index >= array_.size()) return nullJson;
    return array_[index];
}

}  // namespace aura
