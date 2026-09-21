module;
#include <algorithm>
#include <charconv>
#include <cctype>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.diff_utils;

export namespace cc::utils {

// Generate an inline diff with +/- markers
std::string generate_inline_diff(std::string_view old_text, std::string_view new_text) {
    std::string result;

    // Split into lines
    auto split_lines = [](std::string_view text) {
        std::vector<std::string_view> lines;
        std::size_t pos = 0;
        while (pos < text.size()) {
            auto end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            lines.push_back(text.substr(pos, end - pos));
            pos = end + 1;
        }
        return lines;
    };

    auto old_lines = split_lines(old_text);
    auto new_lines = split_lines(new_text);

    // Simple LCS-based diff
    std::size_t i = 0, j = 0;
    while (i < old_lines.size() || j < new_lines.size()) {
        if (i < old_lines.size() && j < new_lines.size() && old_lines[i] == new_lines[j]) {
            result += "  ";
            result += old_lines[i];
            result += "\n";
            ++i; ++j;
        } else if (j < new_lines.size() &&
                   (i >= old_lines.size() || (j + 1 < new_lines.size() && new_lines[j + 1] == old_lines[i]))) {
            result += "+ ";
            result += new_lines[j];
            result += "\n";
            ++j;
        } else if (i < old_lines.size()) {
            result += "- ";
            result += old_lines[i];
            result += "\n";
            ++i;
        } else {
            result += "+ ";
            result += new_lines[j];
            result += "\n";
            ++j;
        }
    }

    return result;
}

// Word-level diff with ANSI coloring
std::string word_diff(std::string_view old_text, std::string_view new_text) {
    std::string result;

    // Split into words
    auto split_words = [](std::string_view text) {
        std::vector<std::string_view> words;
        std::size_t pos = 0;
        while (pos < text.size()) {
            // Skip whitespace
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            if (pos >= text.size()) break;
            auto start = pos;
            while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            words.push_back(text.substr(start, pos - start));
        }
        return words;
    };

    auto old_words = split_words(old_text);
    auto new_words = split_words(new_text);

    std::size_t i = 0, j = 0;
    while (i < old_words.size() || j < new_words.size()) {
        if (i < old_words.size() && j < new_words.size() && old_words[i] == new_words[j]) {
            result += old_words[i];
            result += " ";
            ++i; ++j;
        } else if (i < old_words.size() &&
                   (j >= new_words.size() || old_words[i] != new_words[j])) {
            result += "\033[31m[-";
            result += old_words[i];
            result += "-]\033[0m ";
            ++i;
        } else {
            result += "\033[32m{+";
            result += new_words[j];
            result += "+}\033[0m ";
            ++j;
        }
    }

    return result;
}

namespace diff_detail {

struct HunkHeader {
    std::size_t old_start = 0;
    std::size_t old_count = 0;
    std::size_t new_start = 0;
    std::size_t new_count = 0;
};

[[nodiscard]] inline std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        lines.emplace_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return lines;
}

[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>>
parse_range(std::string_view value) {
    value = trim(value);
    auto comma = value.find(',');
    auto start_view = comma == std::string_view::npos ? value : value.substr(0, comma);
    auto count_view = comma == std::string_view::npos ? std::string_view{"1"} : value.substr(comma + 1);

    std::size_t start = 0;
    auto [start_ptr, start_ec] = std::from_chars(start_view.data(), start_view.data() + start_view.size(), start);
    if (start_ec != std::errc{} || start_ptr != start_view.data() + start_view.size()) {
        return std::nullopt;
    }

    std::size_t count = 0;
    auto [count_ptr, count_ec] = std::from_chars(count_view.data(), count_view.data() + count_view.size(), count);
    if (count_ec != std::errc{} || count_ptr != count_view.data() + count_view.size()) {
        return std::nullopt;
    }

    return std::pair{start, count};
}

[[nodiscard]] inline std::optional<HunkHeader> parse_hunk_header(std::string_view line) {
    if (!line.starts_with("@@")) return std::nullopt;

    auto old_pos = line.find('-');
    auto new_pos = line.find('+', old_pos == std::string_view::npos ? 0 : old_pos);
    auto end_pos = line.find("@@", new_pos == std::string_view::npos ? 0 : new_pos);
    if (old_pos == std::string_view::npos || new_pos == std::string_view::npos ||
        end_pos == std::string_view::npos || new_pos <= old_pos) {
        return std::nullopt;
    }

    auto old_range = parse_range(line.substr(old_pos + 1, new_pos - old_pos - 1));
    auto new_range = parse_range(line.substr(new_pos + 1, end_pos - new_pos - 1));
    if (!old_range || !new_range) return std::nullopt;

    return HunkHeader{
        .old_start = old_range->first,
        .old_count = old_range->second,
        .new_start = new_range->first,
        .new_count = new_range->second,
    };
}

[[nodiscard]] inline std::string format_context_error(
    std::size_t line_number,
    std::string_view expected,
    std::string_view actual
) {
    return "Patch context mismatch at source line " + std::to_string(line_number) +
        ": expected '" + std::string(expected) + "', got '" + std::string(actual) + "'";
}

} // namespace diff_detail

// Apply a unified diff patch to content
std::expected<std::string, std::string> apply_patch(std::string_view content, std::string_view patch) {
    auto lines = diff_detail::split_lines(content);
    auto patch_lines = diff_detail::split_lines(patch);
    const bool had_trailing_newline = content.ends_with('\n');

    std::vector<std::string> output;
    std::size_t source_index = 0;
    std::size_t patch_index = 0;
    bool saw_hunk = false;

    while (patch_index < patch_lines.size()) {
        std::string_view line = patch_lines[patch_index];

        if (line.starts_with("---") || line.starts_with("+++") || line.starts_with("diff ") ||
            line.starts_with("index ")) {
            ++patch_index;
            continue;
        }

        if (line.starts_with("@@")) {
            auto header = diff_detail::parse_hunk_header(line);
            if (!header) {
                return std::unexpected("Invalid unified diff hunk header: " + std::string(line));
            }

            const std::size_t target_index = header->old_start == 0 ? 0 : header->old_start - 1;
            if (target_index < source_index) {
                return std::unexpected("Patch hunks overlap or are out of order");
            }

            while (source_index < target_index && source_index < lines.size()) {
                output.push_back(lines[source_index++]);
            }
            if (source_index != target_index) {
                return std::unexpected("Patch hunk starts beyond end of source content");
            }

            saw_hunk = true;
            ++patch_index;
            std::size_t old_seen = 0;
            std::size_t new_seen = 0;

            while (patch_index < patch_lines.size()) {
                std::string_view hunk_line = patch_lines[patch_index];
                if (old_seen == header->old_count && new_seen == header->new_count) {
                    break;
                }
                if (hunk_line.starts_with("@@") || hunk_line.starts_with("diff ")) {
                    break;
                }
                if (hunk_line.starts_with("\\ No newline at end of file")) {
                    ++patch_index;
                    continue;
                }
                if (hunk_line.empty()) {
                    return std::unexpected("Invalid empty line inside unified diff hunk");
                }

                const char tag = hunk_line.front();
                std::string_view text = hunk_line.substr(1);

                if (tag == ' ') {
                    if (source_index >= lines.size()) {
                        return std::unexpected("Patch context extends beyond end of source content");
                    }
                    if (lines[source_index] != text) {
                        return std::unexpected(diff_detail::format_context_error(
                            source_index + 1, text, lines[source_index]));
                    }
                    output.push_back(lines[source_index++]);
                    ++old_seen;
                    ++new_seen;
                } else if (tag == '-') {
                    if (source_index >= lines.size()) {
                        return std::unexpected("Patch deletion extends beyond end of source content");
                    }
                    if (lines[source_index] != text) {
                        return std::unexpected(diff_detail::format_context_error(
                            source_index + 1, text, lines[source_index]));
                    }
                    ++source_index;
                    ++old_seen;
                } else if (tag == '+') {
                    output.emplace_back(text);
                    ++new_seen;
                } else {
                    return std::unexpected("Invalid unified diff hunk line: " + std::string(hunk_line));
                }

                ++patch_index;
            }

            if (old_seen != header->old_count || new_seen != header->new_count) {
                return std::unexpected("Unified diff hunk line count does not match header");
            }
            continue;
        }

        ++patch_index;
    }

    if (!saw_hunk) {
        return std::string(content);
    }

    while (source_index < lines.size()) {
        output.push_back(lines[source_index++]);
    }

    std::string result;
    for (const auto& l : output) {
        result += l;
        result += "\n";
    }
    if (!result.empty() && !had_trailing_newline) {
        result.pop_back();
    }

    return result;
}

} // namespace cc::utils
