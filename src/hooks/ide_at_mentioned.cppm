module;
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

export module cc.hooks.ide_at_mentioned;

export namespace cc::hooks {


struct AtMention {
    std::string type;                 // "file" | "symbol"
    std::string value;                // path / symbol text (line anchor stripped)
    int start;                        // byte offset of the leading '@' in the input
    int end;                          // byte offset one past the last token char
    std::optional<int> line_start;    // parsed from a trailing "#L<start>" anchor
    std::optional<int> line_end;      // parsed from a trailing "#L<start>-<end>" anchor
};


namespace detail {

// Parse a non-empty run of decimal digits into an int. Returns nullopt on the
// first non-digit (or empty input). Never throws.
inline std::optional<int> parse_digits(std::string_view s) {
    if (s.empty()) return std::nullopt;
    int value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

// Strip a trailing "#L<start>[-<end>]" line anchor off `token`. The path
// portion (everything before the anchor) is returned; when a valid anchor is
// present, line_start/line_end are populated. Tokens without a valid anchor
// are returned verbatim with the optionals left empty.
inline std::string strip_line_anchor(std::string_view token,
                                     std::optional<int>& line_start,
                                     std::optional<int>& line_end) {
    line_start.reset();
    line_end.reset();

    auto hash = token.find('#');
    if (hash == std::string_view::npos) {
        return std::string(token);
    }

    std::string_view anchor = token.substr(hash);
    // Anchor must be "#L" (case-insensitive) followed by at least one digit.
    if (anchor.size() < 3 || anchor[0] != '#' ||
        (anchor[1] != 'L' && anchor[1] != 'l') || anchor[2] < '0' || anchor[2] > '9') {
        return std::string(token);
    }

    std::string_view nums = anchor.substr(2);
    std::size_t i = 0;
    while (i < nums.size() && nums[i] >= '0' && nums[i] <= '9') ++i;
    line_start = parse_digits(nums.substr(0, i));

    if (i < nums.size() && nums[i] == '-') {
        std::size_t j = i + 1;
        std::size_t k = j;
        while (k < nums.size() && nums[k] >= '0' && nums[k] <= '9') ++k;
        if (k > j) {
            line_end = parse_digits(nums.substr(j, k - j));
        }
    }

    return std::string(token.substr(0, hash));
}

} // namespace detail


inline std::vector<AtMention> parse_at_mentions(std::string_view input) {
    std::vector<AtMention> mentions;
    std::size_t pos = 0;

    while (pos < input.size()) {

        auto at_pos = input.find('@', pos);
        if (at_pos == std::string_view::npos) break;


        auto end_pos = input.find_first_of(" \t\n", at_pos + 1);
        if (end_pos == std::string_view::npos) end_pos = input.size();

        if (end_pos > at_pos + 1) {
            auto token = input.substr(at_pos + 1, end_pos - at_pos - 1);

            AtMention mention;
            mention.value = detail::strip_line_anchor(token, mention.line_start, mention.line_end);

            mention.type = "symbol";
            if (mention.value.find('/') != std::string::npos ||
                mention.value.find('.') != std::string::npos) {
                mention.type = "file";
            }
            mention.start = static_cast<int>(at_pos);
            mention.end = static_cast<int>(end_pos);
            mentions.push_back(std::move(mention));
        }
        pos = end_pos;
    }
    return mentions;
}


// Resolve a parsed @mention against a workspace root.
//
// File mentions are resolved to a canonical absolute path: relative paths are
// joined to workspace_root, the result is weakly-canonicalised, and the path
// must exist and be a regular file. Symbol mentions are not resolvable here
// (no symbol index is wired) and return an explanatory error.
inline std::expected<std::string, std::string>
resolve_at_mention(const AtMention& mention, std::string_view workspace_root) {
    if (mention.value.empty()) {
        return std::unexpected("Empty mention value");
    }
    if (mention.type == "symbol") {
        return std::unexpected("Symbol resolution is not implemented (no symbol index)");
    }
    if (mention.type != "file") {
        return std::unexpected("Unknown mention type: " + mention.type);
    }

    namespace fs = std::filesystem;
    fs::path p(mention.value);
    if (p.is_relative()) {
        if (workspace_root.empty()) {
            return std::unexpected("Cannot resolve relative path '" + mention.value +
                                   "' without a workspace root");
        }
        p = fs::path(workspace_root) / p;
    }

    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(p, ec);
    if (ec) {
        return std::unexpected("Failed to canonicalise '" + mention.value + "': " + ec.message());
    }
    if (!fs::exists(resolved, ec) || ec) {
        return std::unexpected("Path does not exist: " + resolved.string());
    }
    if (!fs::is_regular_file(resolved, ec) || ec) {
        return std::unexpected("Mentioned path is not a regular file: " + resolved.string());
    }
    return resolved.string();
}


// Convenience overload: resolve against the process current working directory.
inline std::expected<std::string, std::string>
resolve_at_mention(const AtMention& mention) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return std::unexpected("Could not determine workspace root: " + ec.message());
    }
    return resolve_at_mention(mention, cwd.string());
}


inline std::vector<std::string> get_at_mention_completions(std::string_view prefix) {
    // Return common @mention targets that match the given prefix
    static const std::vector<std::string> known_targets = {
        "file", "symbol", "url", "workspace", "selection", "terminal",
        "git", "problems", "codebase",
    };
    std::vector<std::string> completions;
    for (const auto& target : known_targets) {
        if (prefix.empty() || target.find(prefix) == 0) {
            completions.push_back(target);
        }
    }
    return completions;
}

} // namespace cc::hooks
