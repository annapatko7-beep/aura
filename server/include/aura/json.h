// aura/json.h — компактный JSON-парсер/сериализатор без внешних зависимостей.
//
// Объекты хранят порядок ключей (std::vector<pair>), чтобы ответы сервера были
// детерминированными — это удобно для тестов и отладки протокола.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aura {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char* value) : type_(Type::String), string_(value ? value : "") {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Json array() {
        Json json;
        json.type_ = Type::Array;
        return json;
    }
    static Json object() {
        Json json;
        json.type_ = Type::Object;
        return json;
    }
    static Json parse(const std::string& text, std::string* error = nullptr);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double asDouble(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    long long asInt(long long fallback = 0) const {
        return isNumber() ? static_cast<long long>(number_) : fallback;
    }
    const std::string& asString() const {
        static const std::string empty;
        return isString() ? string_ : empty;
    }

    std::string dump(int indent = 0) const;

    // --- object -----------------------------------------------------------
    Json& operator[](const std::string& key);
    void set(const std::string& key, Json value);
    const Json* find(const std::string& key) const;
    bool contains(const std::string& key) const;
    void erase(const std::string& key);
    const Object& members() const { return object_; }
    std::vector<std::string> keys() const;

    std::string getString(const std::string& key, const std::string& fallback = "") const;
    long long getInt(const std::string& key, long long fallback = 0) const;
    double getDouble(const std::string& key, double fallback = 0.0) const;
    bool getBool(const std::string& key, bool fallback = false) const;
    Json get(const std::string& key) const;

    // --- array ------------------------------------------------------------
    void push(Json value);
    std::size_t size() const;
    bool empty() const { return size() == 0; }
    const Json& at(std::size_t index) const;  // вне диапазона — общий null; записи нет
    const Array& items() const { return array_; }
    Array& items() {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            array_.clear();
        }
        return array_;
    }

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

// Экранирование строки для JSON (используется и протоколом, и логами).
std::string jsonEscape(const std::string& value);

}  // namespace aura
