module;

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.bash_helpers;

export namespace cc::tools::bash_helpers {

struct CommentLabel {
    std::string tool_name;
    std::string session_id;
    std::optional<std::string> description;
};

inline std::string make_comment_label(const CommentLabel& label) {
    return "# " + label.tool_name;
}

inline std::string get_tool_name() {
    return "bash";
}

inline bool should_use_sandbox(std::string_view command, bool user_requested) {
    return user_requested;
}

inline std::vector<std::string> split_compound_command(std::string_view command) {
    return {std::string(command)};
}

inline std::string normalize_line_endings(std::string_view text) {
    return std::string(text);
}

inline std::optional<std::string> extract_shebang(std::string_view script) {
    return std::nullopt;
}

// =========================================================================
// Comment-label utilities — migrated from src/tools/BashTool/commentLabel.ts
// =========================================================================

/// If the first line of a bash command is a `# comment` (NOT a `#!`
/// shebang), return the comment text stripped of the leading `#` and
/// any whitespace immediately after it.
///
/// Under fullscreen mode this label doubles as both the non-verbose
/// tool-use label AND the collapse-group ⎿ hint — it's what Claude
/// wrote for the human to read, so we surface it prominently.
///
/// Returns std::nullopt when the command has no leading comment or
/// when the only leading `#` line is a shebang.
inline std::optional<std::string>
extract_bash_comment_label(std::string_view command) noexcept
{
    // Find first newline, or the full string if single-line
    const auto nl = command.find('\n');
    std::string_view first_line =
        (nl == std::string_view::npos) ? command : command.substr(0, nl);

    // Trim leading / trailing whitespace in place
    auto ltrim = [](std::string_view s) -> std::string_view {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        return s;
    };
    auto rtrim = [](std::string_view s) -> std::string_view {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    };
    first_line = ltrim(rtrim(first_line));

    if (first_line.empty()) return std::nullopt;
    if (first_line[0] != '#') return std::nullopt;
    if (first_line.starts_with("#!")) return std::nullopt;  // shebang, not label

    // Strip the leading `#` runs and any whitespace right after
    size_t i = 0;
    while (i < first_line.size() && first_line[i] == '#') ++i;
    while (i < first_line.size() &&
           std::isspace(static_cast<unsigned char>(first_line[i])))
        ++i;

    const auto label = first_line.substr(i);
    if (label.empty()) return std::nullopt;
    return std::string(label);
}

/// Wrap `output` so every line is prefixed with a human-readable
/// `# <label>: ` tag (or just `# ` when `label` is empty). Used to
/// distinguish tool output blocks in conversation transcripts and
/// collapse-group renders.
///
/// When `with_prefix` is false the tag is placed on its own header
/// line instead of being repeated on every output line.
inline std::string label_output_block(std::string_view output,
                                      std::string_view label,
                                      bool with_prefix = true)
{
    const std::string tag =
        label.empty() ? "# " : std::string("# ") + std::string(label) + ": ";

    if (output.empty()) {
        return with_prefix ? std::string() : tag;
    }

    if (!with_prefix) {
        std::string result;
        result.reserve(tag.size() + 1 + output.size());
        result = tag;
        result += "\n";
        result += output;
        // ensure trailing newline
        if (!result.empty() && result.back() != '\n') result.push_back('\n');
        return result;
    }

    // Repeat the tag on every line. Walk through the string-view once.
    std::ostringstream oss;
    size_t start = 0;
    while (start < output.size()) {
        const auto nl = output.find('\n', start);
        const auto line = (nl == std::string_view::npos)
            ? output.substr(start)
            : output.substr(start, nl - start);
        oss << tag << line << "\n";
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return oss.str();
}

// =========================================================================
// Output & content utilities — migrated from src/tools/BashTool/utils.ts
// =========================================================================

/// Strip leading and trailing lines that contain only whitespace/newlines.
/// Unlike trim(), this preserves whitespace within content lines and only
/// removes completely-empty lines from the beginning and end.
///
/// migrated: stripEmptyLines() from utils.ts
inline std::string strip_empty_lines(std::string_view content) {
    if (content.empty()) return {};

    // Split into lines without allocating a full vector — use two passes.
    const auto is_blank_line = [](std::string_view line) -> bool {
        for (char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };

    // Pass 1: find start index (first non-blank line start)
    size_t start = 0;
    while (start < content.size()) {
        const auto nl = content.find('\n', start);
        const std::string_view line = (nl == std::string_view::npos)
            ? content.substr(start)
            : content.substr(start, nl - start);
        if (!is_blank_line(line)) break;
        if (nl == std::string_view::npos) { start = content.size(); break; }
        start = nl + 1;
    }

    if (start >= content.size()) return {};

    // Pass 2: find end index (after last non-blank line, exclusive)
    size_t end = content.size();
    // Walk backwards from end by scanning newlines in reverse direction:
    // repeatedly pop a trailing (possibly blank) line.
    while (end > start) {
        // Find the newline immediately before `end`
        size_t prev_nl = content.rfind('\n', end - 1);
        size_t line_start = (prev_nl == std::string_view::npos || prev_nl < start)
            ? start
            : prev_nl + 1;
        std::string_view line = content.substr(line_start, end - line_start);
        // Strip trailing \n from consideration (it's part of previous line's terminator)
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (!is_blank_line(line)) break;
        end = line_start;
        // If there was a \n before, step back over it too
        if (end > start && content[end - 1] == '\n') --end;
    }

    if (end <= start) return {};
    return std::string(content.substr(start, end - start));
}

/// Check if content is a base64-encoded image data URL.
///
/// migrated: isImageOutput() from utils.ts
inline bool is_image_output(std::string_view content) {
    // Match: data:image/<media-subtype>;base64,  (case insensitive)
    if (!content.starts_with("data:image/") && !content.starts_with("DATA:IMAGE/")) {
        if (content.size() < 11) return false;
        std::string head(content.substr(0, 11));
        std::transform(head.begin(), head.end(), head.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (head != "data:image/") return false;
    } else {
        // shortcut path matched prefix
    }

    // More robust: use a proper regex for the whole pattern
    static const std::regex image_data_uri_re(
        R"(^data:image\/[a-z0-9.+\-_]+;base64,)",
        std::regex::icase);
    std::cmatch m;
    return std::regex_search(content.begin(), content.end(), m, image_data_uri_re,
                             std::regex_constants::match_continuous);
}

struct ParsedDataUri {
    std::string media_type;
    std::string data;  // raw base64 payload (NOT decoded)
};

/// Parse a data-URI string into its media type and base64 payload.
/// Returns std::nullopt when the URI does not match the data:MIME;base64,... shape.
///
/// migrated: parseDataUri() from utils.ts
inline std::optional<ParsedDataUri> parse_data_uri(std::string_view s) {
    // Trim whitespace first (caller contract mirrors TS s.trim().match(RE))
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);

    static const std::regex data_uri_re(R"(^data:([^;]+);base64,(.+)$)");
    std::cmatch m;
    if (!std::regex_match(s.begin(), s.end(), m, data_uri_re)) return std::nullopt;
    std::string media(m[1].first, m[1].second);
    std::string data(m[2].first, m[2].second);
    if (media.empty() || data.empty()) return std::nullopt;
    return ParsedDataUri{std::move(media), std::move(data)};
}

struct FormattedOutput {
    std::uint64_t total_lines = 0;
    std::string truncated_content;
    bool is_image = false;
};

/// Apply output-length capping with a "[N lines truncated]" notice.
/// `max_output_length` is the byte cap for the pre-truncation portion.
///
/// migrated: formatOutput() from utils.ts (getMaxOutputLength call-site).
inline FormattedOutput format_output(std::string_view content,
                                     std::size_t max_output_length) {
    if (is_image_output(content)) {
        return {
            /*total_lines*/ 1,
            /*truncated_content*/ std::string(content),
            /*is_image*/ true,
        };
    }

    const auto count_newlines_up_to = [](std::string_view s, std::size_t lim) -> std::uint64_t {
        std::uint64_t n = 0;
        const std::size_t up = std::min(lim, s.size());
        for (std::size_t i = 0; i < up; ++i) if (s[i] == '\n') ++n;
        return n;
    };

    if (content.size() <= max_output_length) {
        return {
            /*total_lines*/ count_newlines_up_to(content, content.size()) + 1,
            /*truncated_content*/ std::string(content),
            /*is_image*/ false,
        };
    }

    const std::string truncated_part(content.substr(0, max_output_length));
    const std::uint64_t remaining_lines =
        count_newlines_up_to(content.substr(max_output_length),
                             content.size() - max_output_length) + 1;
    const auto suffix =
        std::format("\n\n... [{} lines truncated] ...", remaining_lines);

    return {
        /*total_lines*/ count_newlines_up_to(content, content.size()) + 1,
        /*truncated_content*/ truncated_part + suffix,
        /*is_image*/ false,
    };
}

/// Overload of format_output that applies a built-in sensible default
/// (30 000 bytes) matching bash_tool.cppm detail::kMaxOutput.
inline FormattedOutput format_output(std::string_view content) {
    constexpr std::size_t kDefaultMaxOutput = 30'000;
    return format_output(content, kDefaultMaxOutput);
}

/// Count occurrences of `ch` in `s`, optionally bounded to the first `limit`
/// bytes. Returns the number of matches. Exposed for reuse by callers that
/// want to produce TS-compatible line-count statistics.
///
/// migrated: countCharInString() from src/utils/stringUtils.js
constexpr std::uint64_t count_char_in_string(std::string_view s, char ch,
                                              std::size_t limit = ~std::size_t{0}) {
    std::uint64_t n = 0;
    const auto up = std::min(limit, s.size());
    for (std::size_t i = 0; i < up; ++i) if (s[i] == ch) ++n;
    return n;
}

} // namespace cc::tools::bash_helpers
