module;
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <variant>
#include <optional>
#include <expected>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <charconv>
#include <stdexcept>

export module cc.utils.json_read;

export namespace cc::utils {


struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, JsonArray, JsonObject> data;

    JsonValue() : data(nullptr) {}
    JsonValue(std::nullptr_t) : data(nullptr) {}
    JsonValue(bool b) : data(b) {}
    JsonValue(int64_t i) : data(i) {}
    JsonValue(double d) : data(d) {}
    JsonValue(std::string s) : data(std::move(s)) {}
    JsonValue(const char* s) : data(std::string(s)) {}
    JsonValue(JsonArray arr) : data(std::move(arr)) {}
    JsonValue(JsonObject obj) : data(std::move(obj)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(data); }
    [[nodiscard]] bool is_int() const { return std::holds_alternative<int64_t>(data); }
    [[nodiscard]] bool is_double() const { return std::holds_alternative<double>(data); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<JsonArray>(data); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<JsonObject>(data); }

    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data); }
    [[nodiscard]] int64_t as_int() const { return std::get<int64_t>(data); }
    [[nodiscard]] double as_double() const { return std::get<double>(data); }
    [[nodiscard]] bool as_bool() const { return std::get<bool>(data); }
    [[nodiscard]] const JsonArray& as_array() const { return std::get<JsonArray>(data); }
    [[nodiscard]] const JsonObject& as_object() const { return std::get<JsonObject>(data); }
};

namespace json_detail {
    inline void skip_ws(std::string_view sv, size_t& pos) {
        while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t' ||
               sv[pos] == '\n' || sv[pos] == '\r')) {
            ++pos;
        }
    }

    inline std::string parse_string_literal(std::string_view sv, size_t& pos) {
        if (pos >= sv.size() || sv[pos] != '"') {
            throw std::runtime_error("Expected '\"' at position " + std::to_string(pos));
        }
        ++pos;
        std::string result;
        while (pos < sv.size() && sv[pos] != '"') {
            if (sv[pos] == '\\') {
                ++pos;
                if (pos >= sv.size()) throw std::runtime_error("Unexpected end of string");
                switch (sv[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {

                        if (pos + 4 >= sv.size()) throw std::runtime_error("Invalid unicode escape");
                        std::string hex_str(sv.substr(pos + 1, 4));
                        unsigned int cp = std::stoul(hex_str, nullptr, 16);
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        pos += 4;
                        break;
                    }
                    default: result += sv[pos]; break;
                }
            } else {
                result += sv[pos];
            }
            ++pos;
        }
        if (pos >= sv.size()) throw std::runtime_error("Unterminated string");
        ++pos;
        return result;
    }

    inline JsonValue parse_value(std::string_view sv, size_t& pos);

    inline JsonValue parse_number(std::string_view sv, size_t& pos) {
        size_t start = pos;
        bool is_float = false;
        if (sv[pos] == '-') ++pos;
        while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
        if (pos < sv.size() && sv[pos] == '.') { is_float = true; ++pos; }
        while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
        if (pos < sv.size() && (sv[pos] == 'e' || sv[pos] == 'E')) {
            is_float = true; ++pos;
            if (pos < sv.size() && (sv[pos] == '+' || sv[pos] == '-')) ++pos;
            while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') ++pos;
        }

        auto num_str = sv.substr(start, pos - start);
        if (is_float) {
            // libc++18 lacks std::from_chars for floating-point; use strtod
            std::string tmp(num_str);
            char* end = nullptr;
            double val = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str()) throw std::runtime_error("Invalid number");
            return JsonValue(val);
        } else {
            int64_t val{};
            auto [p, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val);
            if (ec != std::errc{}) throw std::runtime_error("Invalid integer");
            return JsonValue(val);
        }
    }

    inline JsonValue parse_array(std::string_view sv, size_t& pos) {
        ++pos;
        JsonArray arr;
        skip_ws(sv, pos);
        if (pos < sv.size() && sv[pos] == ']') { ++pos; return JsonValue(std::move(arr)); }

        while (pos < sv.size()) {
            arr.push_back(parse_value(sv, pos));
            skip_ws(sv, pos);
            if (pos < sv.size() && sv[pos] == ',') { ++pos; skip_ws(sv, pos); }
            else break;
        }
        if (pos >= sv.size() || sv[pos] != ']') throw std::runtime_error("Expected ']'");
        ++pos;
        return JsonValue(std::move(arr));
    }

    inline JsonValue parse_object(std::string_view sv, size_t& pos) {
        ++pos;
        JsonObject obj;
        skip_ws(sv, pos);
        if (pos < sv.size() && sv[pos] == '}') { ++pos; return JsonValue(std::move(obj)); }

        while (pos < sv.size()) {
            skip_ws(sv, pos);
            auto key = parse_string_literal(sv, pos);
            skip_ws(sv, pos);
            if (pos >= sv.size() || sv[pos] != ':') throw std::runtime_error("Expected ':'");
            ++pos;
            skip_ws(sv, pos);
            obj[std::move(key)] = parse_value(sv, pos);
            skip_ws(sv, pos);
            if (pos < sv.size() && sv[pos] == ',') { ++pos; }
            else break;
        }
        if (pos >= sv.size() || sv[pos] != '}') throw std::runtime_error("Expected '}'");
        ++pos;
        return JsonValue(std::move(obj));
    }

    inline JsonValue parse_value(std::string_view sv, size_t& pos) {
        skip_ws(sv, pos);
        if (pos >= sv.size()) throw std::runtime_error("Unexpected end of input");

        switch (sv[pos]) {
            case '"': return JsonValue(parse_string_literal(sv, pos));
            case '{': return parse_object(sv, pos);
            case '[': return parse_array(sv, pos);
            case 't':
                if (sv.substr(pos, 4) == "true") { pos += 4; return JsonValue(true); }
                throw std::runtime_error("Invalid literal");
            case 'f':
                if (sv.substr(pos, 5) == "false") { pos += 5; return JsonValue(false); }
                throw std::runtime_error("Invalid literal");
            case 'n':
                if (sv.substr(pos, 4) == "null") { pos += 4; return JsonValue(nullptr); }
                throw std::runtime_error("Invalid literal");
            default:
                if (sv[pos] == '-' || (sv[pos] >= '0' && sv[pos] <= '9')) {
                    return parse_number(sv, pos);
                }
                throw std::runtime_error("Unexpected character at position " + std::to_string(pos));
        }
    }


    inline void stringify(const JsonValue& val, std::string& out, bool pretty, int depth) {
        std::string indent_str = pretty ? std::string(depth * 2, ' ') : "";
        std::string child_indent = pretty ? std::string((depth + 1) * 2, ' ') : "";
        std::string nl = pretty ? "\n" : "";
        std::string sep = pretty ? ": " : ":";

        std::visit([&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += arg ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                out += std::to_string(arg);
            } else if constexpr (std::is_same_v<T, double>) {
                std::ostringstream oss;
                oss << arg;
                out += oss.str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                out += '"';
                for (char c : arg) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\f': out += "\\f"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (static_cast<unsigned char>(c) < 0x20) {
                                char buf[8];
                                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                                out += buf;
                            } else {
                                out += c;
                            }
                    }
                }
                out += '"';
            } else if constexpr (std::is_same_v<T, JsonArray>) {
                if (arg.empty()) { out += "[]"; return; }
                out += '[';
                out += nl;
                for (size_t i = 0; i < arg.size(); ++i) {
                    out += child_indent;
                    stringify(arg[i], out, pretty, depth + 1);
                    if (i + 1 < arg.size()) out += ',';
                    out += nl;
                }
                out += indent_str;
                out += ']';
            } else if constexpr (std::is_same_v<T, JsonObject>) {
                if (arg.empty()) { out += "{}"; return; }
                out += '{';
                out += nl;
                size_t count = 0;
                for (const auto& [key, value] : arg) {
                    out += child_indent;
                    out += '"';
                    out += key;
                    out += '"';
                    out += sep;
                    stringify(value, out, pretty, depth + 1);
                    if (++count < arg.size()) out += ',';
                    out += nl;
                }
                out += indent_str;
                out += '}';
            }
        }, val.data);
    }
}


[[nodiscard]] inline JsonValue parse_json(std::string_view sv) {
    size_t pos = 0;
    auto result = json_detail::parse_value(sv, pos);
    json_detail::skip_ws(sv, pos);
    return result;
}


[[nodiscard]] inline std::expected<JsonValue, std::string> parse_json_file(
    const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected("Failed to open file: " + path.string());
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    try {
        return parse_json(oss.str());
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Parse error in ") + path.string() + ": " + e.what());
    }
}


[[nodiscard]] inline std::string json_to_string(const JsonValue& val, bool pretty = false) {
    std::string result;
    json_detail::stringify(val, result, pretty, 0);
    return result;
}


template <typename T>
[[nodiscard]] inline std::optional<T> json_get(const JsonValue& root, std::string_view path) {
    const JsonValue* current = &root;

    size_t start = 0;
    while (start < path.size()) {
        auto dot = path.find('.', start);
        auto segment = (dot == std::string_view::npos)
                       ? path.substr(start)
                       : path.substr(start, dot - start);
        start = (dot == std::string_view::npos) ? path.size() : dot + 1;

        if (!current->is_object()) return std::nullopt;
        auto& obj = current->as_object();
        auto it = obj.find(std::string(segment));
        if (it == obj.end()) return std::nullopt;
        current = &it->second;
    }


    if constexpr (std::is_same_v<T, std::string>) {
        if (current->is_string()) return current->as_string();
    } else if constexpr (std::is_same_v<T, int64_t>) {
        if (current->is_int()) return current->as_int();
    } else if constexpr (std::is_same_v<T, double>) {
        if (current->is_double()) return current->as_double();
        if (current->is_int()) return static_cast<double>(current->as_int());
    } else if constexpr (std::is_same_v<T, bool>) {
        if (current->is_bool()) return current->as_bool();
    }
    return std::nullopt;
}

} // namespace cc::utils
