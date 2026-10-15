#pragma once
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace okuflow::mcp {
// Original bounded JSON implementation: no imported SDK/runtime; duplicate keys
// and malformed UTF-8 are rejected rather than silently reinterpreted.
class Json {
  public:
    using Object = std::map<std::string, Json, std::less<>>;
    using Array = std::vector<Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool v) : value_(v) {}
    Json(int v) : value_(static_cast<double>(v)) {}
    Json(double v) : value_(v) {}
    Json(const char *v) : value_(std::string(v)) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(std::string_view v) : value_(std::string(v)) {}
    Json(Array v) : value_(std::move(v)) {}
    Json(Object v) : value_(std::move(v)) {}
    bool IsObject() const { return std::holds_alternative<Object>(value_); }
    bool IsArray() const { return std::holds_alternative<Array>(value_); }
    bool IsString() const { return std::holds_alternative<std::string>(value_); }
    bool IsNumber() const { return std::holds_alternative<double>(value_); }
    bool IsBool() const { return std::holds_alternative<bool>(value_); }
    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
    const Object &Members() const { return std::get<Object>(value_); }
    Object &Members() { return std::get<Object>(value_); }
    const Array &Elements() const { return std::get<Array>(value_); }
    const std::string &String() const { return std::get<std::string>(value_); }
    double Number() const { return std::get<double>(value_); }
    bool Boolean() const { return std::get<bool>(value_); }
    const Json *Find(std::string_view name) const;
    std::string Dump() const;
    static Json Parse(std::string_view input); // throws invalid_argument
  private:
    Value value_;
};
std::string Base64(std::string_view bytes);
std::string SafeClientName(std::string_view untrusted);
// The deliberately finite draft-07 subset emitted by ControlRegistry.
bool MatchesSchema(const Json &value, const Json &schema);
} // namespace okuflow::mcp
