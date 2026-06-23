/// @file tool_ui_bash.cppm
/// @brief Bash tool UI — userFacingName, renderToolUseMessage, etc.
///
/// Faithful TS port of BashTool.tsx UI methods.
///
/// MODULE:   cc.ui.tools.bash
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/BashTool/BashTool.tsx
///   - userFacingName = "Run"
///   - renderToolUseMessage = truncates command to ~60 chars
///   - renderToolUseProgressMessage = last output line
///   - renderToolUseTag = null (no tag)
///   - isTransparentWrapper = false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.bash;

import cc.ui.tools.registry;

export namespace cc::ui::tools::bash_ui {

using namespace cc::ui::tools;

namespace detail {

/// Truncate a bash command for display, preserving the end (head ellipsis).
/// TS uses MAX_COMMAND_DISPLAY_CHARS (60) for inline summary.
[[nodiscard]] inline std::string truncate_command(std::string_view cmd,
                                                   std::size_t max_len = 60) {
    if (cmd.size() <= max_len) return std::string(cmd);

    // Head ellipsis: show the *end* of the command (most relevant part)
    // TS: headEllipsisCommand or similar
    std::string_view tail = cmd.substr(cmd.size() - max_len + 1);
    return "\xE2\x80\xA6" + std::string(tail);  // … + tail
}

/// Extract the "command" field from bash tool input JSON.
/// Lightweight string extraction — no full JSON parser needed for this field.
[[nodiscard]] inline std::string extract_command(std::string_view input_json) {
    // Look for "command": "..." pattern
    auto pos = input_json.find("\"command\"");
    if (pos == std::string_view::npos) {
        // Fallback: try "command" without quotes around key
        pos = input_json.find("command");
        if (pos == std::string_view::npos) return {};
    }

    // Find the colon
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};

    // Find the opening quote of the value
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};

    // Find the closing quote (handle escaped quotes)
    std::size_t end = quote + 1;
    while (end < input_json.size() && input_json[end] != '"') {
        if (input_json[end] == '\\' && end + 1 < input_json.size()) {
            end += 2;  // skip escaped char
        } else {
            ++end;
        }
    }
    if (end >= input_json.size()) return {};

    // Extract and unescape basic escapes
    std::string result;
    result.reserve(end - quote - 1);
    for (std::size_t i = quote + 1; i < end; ++i) {
        if (input_json[i] == '\\' && i + 1 < end) {
            ++i;
            char c = input_json[i];
            switch (c) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '\\': result += '\\'; break;
                case '"': result += '"'; break;
                default: result += c; break;
            }
        } else {
            result += input_json[i];
        }
    }

    // Collapse whitespace to first line
    auto nl = result.find('\n');
    if (nl != std::string::npos) {
        result = result.substr(0, nl);
    }
    return result;
}

/// Get last non-empty line from partial output.
/// Used for progress message.
[[nodiscard]] inline std::string last_output_line(std::string_view partial) {
    if (partial.empty()) return {};

    // Find last newline
    auto end = partial.size();
    while (end > 0 && (partial[end - 1] == '\n' || partial[end - 1] == '\r')) {
        --end;
    }
    if (end == 0) return {};

    auto start = partial.rfind('\n', end - 1);
    if (start == std::string_view::npos) start = 0;
    else ++start;  // skip the newline

    std::string_view line = partial.substr(start, end - start);

    // Trim leading whitespace
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }

    // Truncate
    constexpr std::size_t kMaxLen = 100;
    if (line.size() > kMaxLen) {
        return std::string(line.substr(0, kMaxLen)) + "\xE2\x80\xA6";  // …
    }
    return std::string(line);
}

}  // namespace detail

// ============================================================
// BashTool UI functions
// ============================================================

/// Build ToolUIFunctions for the Bash tool.
[[nodiscard]] inline ToolUIFunctions make_bash_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view input_json) {
        // TS: default = "Bash", sandbox mode = "SandboxedBash",
        // sed edits delegate to fileEditUserFacingName ("Update").
        // Faithful port: default to "Bash" (sandbox detection TBD).
        (void)input_json;
        return std::string{"Bash"};
    };

    fns.message = [](std::string_view input_json) {
        std::string cmd = detail::extract_command(input_json);
        if (cmd.empty()) {
            // Fallback
            return std::string{"running command\342\200\246"};  // …
        }
        return detail::truncate_command(cmd, 60);
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        // TS: renderToolUseProgressMessage(progressMessagesForMessage, ...)
        //   if (!lastProgress || !lastProgress.data) return "Running…";
        //   else return ShellProgressMessage (last line of output)
        std::string last_line = detail::last_output_line(partial_result);
        if (!last_line.empty()) {
            return last_line;
        }
        // No progress data yet → "Running…"
        (void)input_json;
        return std::string{"Running\342\200\246"};  // Running…
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to run\342\200\246"};  // Waiting to run…
    };

    fns.is_transparent_wrapper = false;

    return fns;
}

/// Register bash tool UI in the global registry.
inline void register_bash_ui() {
    global_tool_ui_registry().register_tool_ui("bash", make_bash_ui());
    // Also register under "Bash" (case-insensitive matching would be nice,
    // but we register common variants for safety)
    global_tool_ui_registry().register_tool_ui("Bash", make_bash_ui());
}

}  // namespace cc::ui::tools::bash_ui
