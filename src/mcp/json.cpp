#include "okuflow/mcp/json.hpp"
#include <charconv>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace okuflow::mcp {
namespace {
[[noreturn]] void Bad() { throw std::invalid_argument("invalid_json"); }
void Utf8(std::string &s, unsigned cp) {
    if (cp <= 0x7f)
        s.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        s.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp <= 0xffff) {
        s.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        s.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        s.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
void ValidateUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]), cp = c, n = 0, min = 0;
        if (c < 128)
            continue;
        if ((c & 0xe0) == 0xc0) {
            cp = c & 31;
            n = 1;
            min = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 15;
            n = 2;
            min = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 7;
            n = 3;
            min = 0x10000;
        } else
            Bad();
        while (n--) {
            if (i >= s.size())
                Bad();
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80)
                Bad();
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            Bad();
    }
}
class Parser {
  public:
    explicit Parser(std::string_view s) : s_(s) {
        if (s.size() > 1024 * 1024)
            Bad();
        ValidateUtf8(s);
    }
    Json All() {
        auto v = Read(0);
        Space();
        if (i_ != s_.size())
            Bad();
        return v;
    }

  private:
    void Space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
            ++i_;
    }
    bool Take(char c) {
        Space();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    unsigned Hex() {
        unsigned n = 0;
        for (int j = 0; j < 4; ++j) {
            if (i_ >= s_.size())
                Bad();
            char c = s_[i_++];
            n <<= 4;
            if (c >= '0' && c <= '9')
                n |= c - '0';
            else if (c >= 'a' && c <= 'f')
                n |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                n |= c - 'A' + 10;
            else
                Bad();
        }
        return n;
    }
    std::string String() {
        if (!Take('"'))
            Bad();
        std::string o;
        while (i_ < s_.size()) {
            unsigned char c = s_[i_++];
            if (c == '"')
                return o;
            if (c < 32)
                Bad();
            if (c != '\\') {
                o.push_back(static_cast<char>(c));
                continue;
            }
            if (i_ >= s_.size())
                Bad();
            char e = s_[i_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                o.push_back(e);
                break;
            case 'b':
                o.push_back('\b');
                break;
            case 'f':
                o.push_back('\f');
                break;
            case 'n':
                o.push_back('\n');
                break;
            case 'r':
                o.push_back('\r');
                break;
            case 't':
                o.push_back('\t');
                break;
            case 'u': {
                auto cp = Hex();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (i_ + 2 > s_.size() || s_[i_++] != '\\' || s_[i_++] != 'u')
                        Bad();
                    auto lo = Hex();
                    if (lo < 0xdc00 || lo > 0xdfff)
                        Bad();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff)
                    Bad();
                Utf8(o, cp);
                break;
            }
            default:
                Bad();
            }
        }
        Bad();
    }
    Json Read(unsigned depth) {
        if (depth > 32 || ++nodes_ > 65536)
            Bad();
        Space();
        if (i_ >= s_.size())
            Bad();
        char c = s_[i_];
        if (c == '"')
            return Json(String());
        if (Take('{')) {
            Json::Object o;
            if (Take('}'))
                return o;
            do {
                auto k = String();
                if (!Take(':'))
                    Bad();
                auto v = Read(depth + 1);
                if (!o.emplace(std::move(k), std::move(v)).second)
                    Bad();
                if (Take('}'))
                    return o;
            } while (Take(','));
            Bad();
        }
        if (Take('[')) {
            Json::Array a;
            if (Take(']'))
                return a;
            do {
                a.push_back(Read(depth + 1));
                if (Take(']'))
                    return a;
            } while (Take(','));
            Bad();
        }
        for (auto [word, val] :
             {std::pair{"true", Json(true)}, std::pair{"false", Json(false)}, std::pair{"null", Json()}}) {
            if (s_.substr(i_, std::string_view(word).size()) == word) {
                i_ += std::string_view(word).size();
                return val;
            }
        }
        auto begin = i_;
        if (s_[i_] == '-')
            ++i_;
        if (i_ >= s_.size())
            Bad();
        if (s_[i_] == '0')
            ++i_;
        else {
            if (s_[i_] < '1' || s_[i_] > '9')
                Bad();
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            auto b = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
            if (i_ == b)
                Bad();
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            auto b = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
            if (i_ == b)
                Bad();
        }
        double n;
        auto r = std::from_chars(s_.data() + begin, s_.data() + i_, n);
        if (r.ec != std::errc{} || r.ptr != s_.data() + i_ || !std::isfinite(n))
            Bad();
        return n;
    }
    std::string_view s_;
    size_t i_{}, nodes_{};
};
std::string Quote(std::string_view s) {
    std::string o = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c < 32) {
            o += "\\u00";
            o += hex[c >> 4];
            o += hex[c & 15];
        } else
            o += c;
    }
    return o + '"';
}
} // namespace
const Json *Json::Find(std::string_view name) const {
    if (!IsObject())
        return nullptr;
    auto i = Members().find(name);
    return i == Members().end() ? nullptr : &i->second;
}
Json Json::Parse(std::string_view s) { return Parser(s).All(); }
std::string Json::Dump() const {
    if (IsNull())
        return "null";
    if (IsBool())
        return Boolean() ? "true" : "false";
    if (IsString())
        return Quote(String());
    if (IsNumber()) {
        char b[64];
        auto r = std::to_chars(b, b + 64, Number());
        if (r.ec != std::errc{} || !std::isfinite(Number()))
            Bad();
        return {b, r.ptr};
    }
    std::string o;
    bool first = true;
    if (IsArray()) {
        o = "[";
        for (const auto &v : Elements()) {
            if (!first)
                o += ',';
            first = false;
            o += v.Dump();
        }
        return o + ']';
    }
    o = "{";
    for (const auto &[k, v] : Members()) {
        if (!first)
            o += ',';
        first = false;
        o += Quote(k) + ':' + v.Dump();
    }
    return o + '}';
}
std::string Base64(std::string_view s) {
    constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((s.size() + 2) / 3 * 4);
    for (size_t i = 0; i < s.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(s[i]) << 16;
        if (i + 1 < s.size())
            n |= static_cast<unsigned char>(s[i + 1]) << 8;
        if (i + 2 < s.size())
            n |= static_cast<unsigned char>(s[i + 2]);
        o += table[(n >> 18) & 63];
        o += table[(n >> 12) & 63];
        o += i + 1 < s.size() ? table[(n >> 6) & 63] : '=';
        o += i + 2 < s.size() ? table[n & 63] : '=';
    }
    return o;
}
std::string SafeClientName(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        if (o.size() == 60)
            break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
            c == '-' || c == '_')
            o += c;
    }
    return o.empty() ? "MCP client" : o;
}
namespace {
bool ExactType(const Json &value, std::string_view type) {
    if (type == "object")
        return value.IsObject();
    if (type == "array")
        return value.IsArray();
    if (type == "string")
        return value.IsString();
    if (type == "boolean")
        return value.IsBool();
    if (type == "null")
        return value.IsNull();
    if (type == "number")
        return value.IsNumber() && std::isfinite(value.Number());
    if (type == "integer")
        return value.IsNumber() && std::isfinite(value.Number()) &&
               std::floor(value.Number()) == value.Number();
    return false;
}
bool Bound(const Json &schema, std::string_view key, double actual, bool minimum) {
    const auto bound = schema.Find(key);
    return !bound || (bound->IsNumber() && std::isfinite(bound->Number()) &&
                      (minimum ? actual >= bound->Number() : actual <= bound->Number()));
}
bool Matches(const Json &value, const Json &schema, unsigned depth) {
    if (depth > 32)
        return false;
    if (value.IsNumber() && !std::isfinite(value.Number()))
        return false;
    if (schema.IsBool())
        return schema.Boolean();
    if (!schema.IsObject())
        return false;
    if (const auto one = schema.Find("oneOf")) {
        if (!one->IsArray() || one->Elements().empty())
            return false;
        unsigned count = 0;
        for (const auto &branch : one->Elements())
            count += Matches(value, branch, depth + 1);
        if (count != 1)
            return false;
    }
    if (const auto excluded = schema.Find("not"); excluded && Matches(value, *excluded, depth + 1))
        return false;
    if (const auto constant = schema.Find("const"); constant && constant->Dump() != value.Dump())
        return false;
    if (const auto choices = schema.Find("enum")) {
        if (!choices->IsArray() || choices->Elements().empty())
            return false;
        bool found = false;
        for (const auto &choice : choices->Elements())
            found |= choice.Dump() == value.Dump();
        if (!found)
            return false;
    }
    if (const auto type = schema.Find("type")) {
        if (type->IsString()) {
            if (!ExactType(value, type->String()))
                return false;
        } else if (type->IsArray()) {
            if (type->Elements().empty())
                return false;
            bool found = false;
            for (const auto &t : type->Elements()) {
                if (!t.IsString())
                    return false;
                const auto &name = t.String();
                if (name != "object" && name != "array" && name != "string" && name != "boolean" &&
                    name != "null" && name != "number" && name != "integer")
                    return false;
                found |= ExactType(value, name);
            }
            if (!found)
                return false;
        } else
            return false;
    }
    if (value.IsObject()) {
        if (!Bound(schema, "minProperties", static_cast<double>(value.Members().size()), true) ||
            !Bound(schema, "maxProperties", static_cast<double>(value.Members().size()), false))
            return false;
        if (const auto required = schema.Find("required")) {
            if (!required->IsArray())
                return false;
            for (const auto &key : required->Elements())
                if (!key.IsString() || !value.Find(key.String()))
                    return false;
        }
        const auto properties = schema.Find("properties"), additional = schema.Find("additionalProperties");
        if (properties && !properties->IsObject())
            return false;
        for (const auto &[key, item] : value.Members()) {
            const auto property = properties ? properties->Find(key) : nullptr;
            if (property) {
                if (!Matches(item, *property, depth + 1))
                    return false;
            } else if (additional && !Matches(item, *additional, depth + 1))
                return false;
        }
    }
    if (value.IsArray()) {
        if (!Bound(schema, "minItems", static_cast<double>(value.Elements().size()), true) ||
            !Bound(schema, "maxItems", static_cast<double>(value.Elements().size()), false))
            return false;
        if (const auto unique = schema.Find("uniqueItems")) {
            if (!unique->IsBool())
                return false;
            if (unique->Boolean()) {
                std::set<std::string> values;
                for (const auto &item : value.Elements())
                    if (!values.insert(item.Dump()).second)
                        return false;
            }
        }
        if (const auto items = schema.Find("items"))
            for (const auto &item : value.Elements())
                if (!Matches(item, *items, depth + 1))
                    return false;
    }
    if (value.IsNumber()) {
        if (!std::isfinite(value.Number()) || !Bound(schema, "minimum", value.Number(), true) ||
            !Bound(schema, "maximum", value.Number(), false))
            return false;
    }
    if (value.IsString()) {
        std::size_t points = 0;
        for (unsigned char ch : value.String())
            if ((ch & 0xc0) != 0x80)
                ++points;
        if (!Bound(schema, "minLength", static_cast<double>(points), true) ||
            !Bound(schema, "maxLength", static_cast<double>(points), false))
            return false;
        if (const auto pattern = schema.Find("pattern")) {
            if (!pattern->IsString())
                return false;
            try {
                if (!std::regex_match(value.String(), std::regex(pattern->String())))
                    return false;
            } catch (const std::regex_error &) {
                return false;
            }
        }
    }
    return true;
}
} // namespace
bool MatchesSchema(const Json &value, const Json &schema) { return Matches(value, schema, 0); }
} // namespace okuflow::mcp
