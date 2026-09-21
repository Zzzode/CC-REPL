/// @file runtime_shared_utils.cppm
/// @brief General-purpose helpers shared across the runtime-tool subsystems,
/// extracted from the (former) monolithic runtime_registry.cppm as part of
/// audit §13 #1.
///
/// These are pure, dependency-light utilities (shell quoting, XML escaping,
/// filesystem-path sanitisation, agent-name parsing) used by several runtime
/// concerns (native-agent formatting, team message delivery, task summaries).
/// Hoisting them into their own module breaks the shared-helper coupling that
/// previously kept those concerns jammed into one 3700-line file.
///
/// Content extracted:
///   * shell_quote              (was runtime_shell_quote L387–395)
///   * escape_xml               (was escape_xml_text   L557–571)
///   * runtime_timestamp_string (was runtime_timestamp_string L1999–2002)
///   * runtime_delivery_message_id (was runtime_delivery_message_id L588–591)
///   * safe_runtime_dir_component (was safe_runtime_dir_component L623–634)
///   * path_has_prefix         (was path_has_prefix L649–656)
///   * normalized_absolute_path (was normalized_absolute_path L658–663)
///   * format_agent_pending_user_message (L593–603)
module;

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

export module cc.tools.runtime_shared_utils;

import cc.tools.send_message;   // MessagePriority / message_priority_name

export namespace cc::tools::runtime_shared_utils {

namespace fs = std::filesystem;

/// Single-quote a string for safe interpolation into a POSIX shell command
/// (escapes embedded single quotes via the '"'"' idiom).
[[nodiscard]] inline std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

/// Escape the five XML special characters for safe embedding inside XML/HTML
/// (used when composing prompt fragments and result sections).
[[nodiscard]] inline std::string escape_xml(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}

/// Millisecond-resolution wall-clock timestamp used when tagging structured
/// runtime messages (shutdown requests, plan approvals, …).
[[nodiscard]] inline std::string runtime_timestamp_string() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

/// Monotonic-clock id used to tag queued delivery messages. Uses nanosecond
/// resolution so rapid-fire messages from the same process stay ordered.
[[nodiscard]] inline std::string runtime_delivery_message_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::format("msg-{}", std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

/// Sanitise an arbitrary byte string into a component safe to use as a
/// directory or file name on POSIX filesystems. Any character that isn't
/// alphanumeric, a dash, an underscore or a dot is replaced with '_'. If
/// the result would be empty, returns the `fallback` verbatim.
[[nodiscard]] inline std::string safe_runtime_dir_component(
    std::string_view value,
    std::string_view fallback
) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string(fallback) : out;
}

/// Lexicographic path-prefix check. Returns true when every segment in `prefix`
/// appears as a leading segment of `path` (avoids false matches like
/// `/tmp/foo-bar` being considered under `/tmp/foo`).
[[nodiscard]] inline bool path_has_prefix(const fs::path& path, const fs::path& prefix) {
    auto path_it = path.begin();
    auto prefix_it = prefix.begin();
    for (; prefix_it != prefix.end(); ++prefix_it, ++path_it) {
        if (path_it == path.end() || *path_it != *prefix_it) return false;
    }
    return true;
}

/// Best-effort absolute + lexically-normal path. Swallows any filesystem
/// error from `fs::absolute` by falling back to the input path (the normalised
/// form is only used for prefix/artifact comparisons, not IO).
[[nodiscard]] inline fs::path normalized_absolute_path(const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal();
}

/// Human-readable prefix used when a pending user-visible message is queued
/// for a background agent — rendered verbatim in the agent's transcript.
[[nodiscard]] inline std::string format_agent_pending_user_message(
    std::string_view from_agent,
    MessagePriority priority,
    std::string_view message
) {
    return std::format(
        "[Message from {} priority={}]\n{}",
        from_agent,
        message_priority_name(priority),
        message);
}

} // namespace cc::tools::runtime_shared_utils
