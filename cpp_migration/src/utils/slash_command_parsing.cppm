module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.slash_command_parsing;

export namespace cc::utils::slash_command_parsing {

struct ParsedSlashCommand {
    std::string command_name;
    std::string args;
    bool is_mcp = false;
};

namespace detail {
    [[nodiscard]] inline std::string_view trim(std::string_view value) noexcept {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n' || value.front() == '\r')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' || value.back() == '\r')) value.remove_suffix(1);
        return value;
    }

    [[nodiscard]] inline std::vector<std::string> split_space(std::string_view value) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto pos = value.find(' ', start);
            if (pos == std::string_view::npos) {
                parts.emplace_back(value.substr(start));
                break;
            }
            parts.emplace_back(value.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    }

    [[nodiscard]] inline std::string join_space(const std::vector<std::string>& parts, std::size_t start) {
        if (start >= parts.size()) return "";
        std::string out = parts[start];
        for (std::size_t i = start + 1; i < parts.size(); ++i) {
            out.push_back(' ');
            out.append(parts[i]);
        }
        return out;
    }
} // namespace detail

[[nodiscard]] inline std::optional<ParsedSlashCommand> parse_slash_command(std::string_view input) {
    const auto trimmed = detail::trim(input);
    if (trimmed.empty() || trimmed.front() != '/') return std::nullopt;

    const auto without_slash = trimmed.substr(1);
    const auto words = detail::split_space(without_slash);
    if (words.empty() || words[0].empty()) return std::nullopt;

    ParsedSlashCommand parsed{.command_name = words[0], .args = "", .is_mcp = false};
    std::size_t args_start_index = 1;
    if (words.size() > 1 && words[1] == "(MCP)") {
        parsed.command_name += " (MCP)";
        parsed.is_mcp = true;
        args_start_index = 2;
    }
    parsed.args = detail::join_space(words, args_start_index);
    return parsed;
}

} // namespace cc::utils::slash_command_parsing
