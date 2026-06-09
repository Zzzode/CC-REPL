// Bash comment label extraction: extracts #comment from first line of commands.
// Mirrors src/tools/BashTool/commentLabel.ts
module;
#include <string>
#include <string_view>
#include <optional>

export module cc.tools.bash_comment_label;

export namespace cc::tools::bash {

/// If the first line of a bash command is a `# comment` (not a `#!` shebang),
/// return the comment text stripped of the `#` prefix. Otherwise nullopt.
/// Under fullscreen mode this is the non-verbose tool-use label AND the
/// collapse-group hint — it's what Claude wrote for the human to read.
[[nodiscard]] inline auto extract_bash_comment_label(std::string_view command)
    -> std::optional<std::string> {
    const auto nl = command.find('\n');
    const auto first_line_full = (nl == std::string_view::npos)
        ? command
        : command.substr(0, nl);

    // Trim leading/trailing whitespace
    auto start = first_line_full.find_first_not_of(" \t\r");
    if (start == std::string_view::npos) return std::nullopt;
    auto end = first_line_full.find_last_not_of(" \t\r");
    const auto first_line = first_line_full.substr(start, end - start + 1);

    if (first_line.empty() || first_line[0] != '#') return std::nullopt;
    if (first_line.starts_with("#!")) return std::nullopt;

    // Strip leading `#` characters and whitespace
    auto content_start = first_line.find_first_not_of('#');
    if (content_start == std::string_view::npos) return std::nullopt;
    content_start = first_line.find_first_not_of(" \t", content_start);
    if (content_start == std::string_view::npos) return std::nullopt;

    auto result = std::string(first_line.substr(content_start));
    if (result.empty()) return std::nullopt;
    return result;
}

} // namespace cc::tools::bash
