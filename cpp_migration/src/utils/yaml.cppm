module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <variant>
#include <cstdint>
#include <charconv>
#include <optional>
#include <sstream>
#include <iomanip>

export module cc.utils.yaml;

export namespace cc::utils {


struct YamlValue;
using YamlMap = std::map<std::string, YamlValue>;
using YamlArray = std::vector<YamlValue>;

struct YamlValue {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, YamlArray, YamlMap> data;

    YamlValue() : data(nullptr) {}
    YamlValue(std::nullptr_t) : data(nullptr) {}
    YamlValue(bool b) : data(b) {}
    YamlValue(int64_t i) : data(i) {}
    YamlValue(double d) : data(d) {}
    YamlValue(std::string s) : data(std::move(s)) {}
    YamlValue(const char* s) : data(std::string(s)) {}
    YamlValue(YamlArray arr) : data(std::move(arr)) {}
    YamlValue(YamlMap m) : data(std::move(m)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool is_map() const { return std::holds_alternative<YamlMap>(data); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<YamlArray>(data); }
};

namespace yaml_detail {

    inline std::string_view trim_line(std::string_view line) {
        auto start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos) return {};
        auto end = line.find_last_not_of(" \t\r\n");
        return line.substr(start, end - start + 1);
    }


    inline size_t indent_level(std::string_view line) {
        size_t indent = 0;
        for (char c : line) {
            if (c == ' ') ++indent;
            else if (c == '\t') indent += 2;
            else break;
        }
        return indent;
    }


    inline YamlValue parse_scalar(std::string_view value) {
        if (value.empty() || value == "null" || value == "~") return YamlValue(nullptr);
        if (value == "true" || value == "True" || value == "TRUE") return YamlValue(true);
        if (value == "false" || value == "False" || value == "FALSE") return YamlValue(false);


        int64_t int_val{};
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
        if (ec == std::errc{} && ptr == value.data() + value.size()) {
            return YamlValue(int_val);
        }


        double dbl_val{};
        auto [ptr2, ec2] = std::from_chars(value.data(), value.data() + value.size(), dbl_val);
        if (ec2 == std::errc{} && ptr2 == value.data() + value.size()) {
            return YamlValue(dbl_val);
        }


        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            return YamlValue(std::string(value.substr(1, value.size() - 2)));
        }

        return YamlValue(std::string(value));
    }


    inline std::vector<std::string_view> split_lines(std::string_view sv) {
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start < sv.size()) {
            auto pos = sv.find('\n', start);
            if (pos == std::string_view::npos) {
                lines.push_back(sv.substr(start));
                break;
            }
            lines.push_back(sv.substr(start, pos - start));
            start = pos + 1;
        }
        return lines;
    }


    inline YamlValue parse_block(const std::vector<std::string_view>& lines,
                                  size_t& idx, size_t base_indent) {
        if (idx >= lines.size()) return YamlValue(nullptr);

        auto line = lines[idx];
        auto trimmed = trim_line(line);


        if (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            YamlArray arr;
            while (idx < lines.size()) {
                auto current = lines[idx];
                size_t cur_indent = indent_level(current);
                if (cur_indent < base_indent) break;

                auto cur_trimmed = trim_line(current);
                if (cur_trimmed.empty() || cur_trimmed[0] == '#') { ++idx; continue; }
                if (cur_trimmed[0] != '-') break;


                auto item_value = trim_line(cur_trimmed.substr(2));
                ++idx;


                if (item_value.find(':') != std::string_view::npos &&
                    idx < lines.size() && indent_level(lines[idx]) > cur_indent) {

                    --idx;
                    auto nested_indent = cur_indent + 2;

                    YamlMap map;
                    auto colon = item_value.find(':');
                    auto key = std::string(item_value.substr(0, colon));
                    auto val_str = trim_line(item_value.substr(colon + 1));
                    map[key] = parse_scalar(val_str);
                    ++idx;

                    while (idx < lines.size() && indent_level(lines[idx]) >= nested_indent) {
                        auto nested_trimmed = trim_line(lines[idx]);
                        if (nested_trimmed.empty() || nested_trimmed[0] == '#') { ++idx; continue; }
                        auto nc = nested_trimmed.find(':');
                        if (nc == std::string_view::npos) break;
                        auto nk = std::string(nested_trimmed.substr(0, nc));
                        auto nv = trim_line(nested_trimmed.substr(nc + 1));
                        map[nk] = parse_scalar(nv);
                        ++idx;
                    }
                    arr.emplace_back(YamlValue(std::move(map)));
                } else {
                    arr.emplace_back(parse_scalar(item_value));
                }
            }
            return YamlValue(std::move(arr));
        }


        YamlMap map;
        while (idx < lines.size()) {
            auto current = lines[idx];
            size_t cur_indent = indent_level(current);
            if (cur_indent < base_indent) break;

            auto cur_trimmed = trim_line(current);
            if (cur_trimmed.empty() || cur_trimmed[0] == '#') { ++idx; continue; }


            auto colon = cur_trimmed.find(':');
            if (colon == std::string_view::npos) { ++idx; continue; }

            auto key = std::string(trim_line(cur_trimmed.substr(0, colon)));
            auto value_part = cur_trimmed.substr(colon + 1);
            auto value_trimmed = trim_line(value_part);
            ++idx;

            if (value_trimmed.empty() && idx < lines.size()) {

                size_t child_indent = indent_level(lines[idx]);
                if (child_indent > cur_indent) {
                    map[key] = parse_block(lines, idx, child_indent);
                } else {
                    map[key] = YamlValue(nullptr);
                }
            } else {
                map[key] = parse_scalar(value_trimmed);
            }
        }
        return YamlValue(std::move(map));
    }
}


[[nodiscard]] inline YamlValue parse_yaml(std::string_view sv) {
    auto lines = yaml_detail::split_lines(sv);
    size_t idx = 0;

    if (!lines.empty() && yaml_detail::trim_line(lines[0]) == "---") ++idx;
    return yaml_detail::parse_block(lines, idx, 0);
}


[[nodiscard]] inline std::string yaml_to_string(const YamlValue& value, size_t indent = 0) {
    std::string prefix(indent * 2, ' ');
    std::string result;

    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            result = "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            result = arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int64_t>) {
            result = std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            std::ostringstream oss;
            oss << arg;
            result = oss.str();
        } else if constexpr (std::is_same_v<T, std::string>) {

            bool needs_quote = arg.find(':') != std::string::npos ||
                             arg.find('#') != std::string::npos ||
                             arg.empty();
            if (needs_quote) {
                result = "\"" + arg + "\"";
            } else {
                result = arg;
            }
        } else if constexpr (std::is_same_v<T, YamlArray>) {
            for (const auto& item : arg) {
                result += prefix + "- " + yaml_to_string(item, indent + 1) + "\n";
            }
        } else if constexpr (std::is_same_v<T, YamlMap>) {
            for (const auto& [key, val] : arg) {
                if (val.is_map() || val.is_array()) {
                    result += prefix + key + ":\n" + yaml_to_string(val, indent + 1);
                } else {
                    result += prefix + key + ": " + yaml_to_string(val, indent) + "\n";
                }
            }
        }
    }, value.data);

    return result;
}

} // namespace cc::utils
