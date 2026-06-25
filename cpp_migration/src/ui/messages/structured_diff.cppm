module;
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

export module cc.ui.messages.structured_diff;

export namespace cc::ui::messages {

[[nodiscard]] inline std::string repeat_structured_diff(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

namespace detail {

// Split text into lines
inline auto split_lines(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty()) lines.emplace_back("");
    return lines;
}

// Simple LCS-based diff: returns pairs of (old_line_idx, new_line_idx) for matching lines
inline auto compute_lcs(const std::vector<std::string>& old_lines,
                        const std::vector<std::string>& new_lines)
    -> std::vector<std::pair<int, int>> {
    int m = static_cast<int>(old_lines.size());
    int n = static_cast<int>(new_lines.size());

    // DP table for LCS length
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            if (old_lines[i - 1] == new_lines[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }

    // Backtrack to find matching pairs
    std::vector<std::pair<int, int>> matches;
    int i = m, j = n;
    while (i > 0 && j > 0) {
        if (old_lines[i - 1] == new_lines[j - 1]) {
            matches.emplace_back(i - 1, j - 1);
            --i;
            --j;
        } else if (dp[i - 1][j] > dp[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(matches.begin(), matches.end());
    return matches;
}

} // namespace detail

// Render a unified diff view with additions/deletions highlighted
inline auto render_structured_diff(std::string_view old_content,
                                    std::string_view new_content,
                                    int) -> std::string {
    auto old_lines = detail::split_lines(old_content);
    auto new_lines = detail::split_lines(new_content);
    auto matches = detail::compute_lcs(old_lines, new_lines);

    std::ostringstream out;
    int old_idx = 0, new_idx = 0;
    size_t match_idx = 0;

    while (old_idx < static_cast<int>(old_lines.size()) ||
           new_idx < static_cast<int>(new_lines.size())) {

        if (match_idx < matches.size()) {
            auto [mo, mn] = matches[match_idx];

            // Print deletions (old lines before this match)
            while (old_idx < mo) {
                out << "\033[31m- " << old_lines[old_idx] << "\033[0m\n";
                ++old_idx;
            }
            // Print additions (new lines before this match)
            while (new_idx < mn) {
                out << "\033[32m+ " << new_lines[new_idx] << "\033[0m\n";
                ++new_idx;
            }
            // Print context (matching line)
            out << "  " << old_lines[old_idx] << "\n";
            ++old_idx;
            ++new_idx;
            ++match_idx;
        } else {
            // Remaining lines after all matches
            while (old_idx < static_cast<int>(old_lines.size())) {
                out << "\033[31m- " << old_lines[old_idx] << "\033[0m\n";
                ++old_idx;
            }
            while (new_idx < static_cast<int>(new_lines.size())) {
                out << "\033[32m+ " << new_lines[new_idx] << "\033[0m\n";
                ++new_idx;
            }
        }
    }

    return out.str();
}

// Render a side-by-side diff view
inline auto render_side_by_side_diff(std::string_view old_c,
                                      std::string_view new_c,
                                      int width) -> std::string {
    auto old_lines = detail::split_lines(old_c);
    auto new_lines = detail::split_lines(new_c);

    int half_width = (width - 3) / 2; // "│" separator
    std::ostringstream out;

    // Header
    out << "\033[2m" << repeat_structured_diff("─", half_width) << "┬"
        << repeat_structured_diff("─", half_width) << "\033[0m\n";

    int max_lines = std::max(static_cast<int>(old_lines.size()),
                             static_cast<int>(new_lines.size()));

    for (int i = 0; i < max_lines; ++i) {
        // Left side (old)
        if (i < static_cast<int>(old_lines.size())) {
            std::string left = old_lines[i];
            if (static_cast<int>(left.size()) > half_width) {
                left = left.substr(0, half_width - 1) + "…";
            }
            int pad = half_width - static_cast<int>(left.size());
            bool is_removed = (i >= static_cast<int>(new_lines.size()) ||
                              old_lines[i] != new_lines[i]);
            if (is_removed) {
                out << "\033[31m" << left << std::string(std::max(0, pad), ' ') << "\033[0m";
            } else {
                out << left << std::string(std::max(0, pad), ' ');
            }
        } else {
            out << std::string(half_width, ' ');
        }

        out << "\033[2m│\033[0m";

        // Right side (new)
        if (i < static_cast<int>(new_lines.size())) {
            std::string right = new_lines[i];
            if (static_cast<int>(right.size()) > half_width) {
                right = right.substr(0, half_width - 1) + "…";
            }
            bool is_added = (i >= static_cast<int>(old_lines.size()) ||
                            old_lines[i] != new_lines[i]);
            if (is_added) {
                out << "\033[32m" << right << "\033[0m";
            } else {
                out << right;
            }
        }

        out << "\n";
    }

    // Footer
    out << "\033[2m" << repeat_structured_diff("─", half_width) << "┴"
        << repeat_structured_diff("─", half_width) << "\033[0m";

    return out.str();
}

// Render an inline diff showing character-level changes
inline auto render_inline_diff(std::string_view old_c,
                                std::string_view new_c) -> std::string {
    std::ostringstream out;

    // Find common prefix
    size_t prefix_len = 0;
    while (prefix_len < old_c.size() && prefix_len < new_c.size() &&
           old_c[prefix_len] == new_c[prefix_len]) {
        ++prefix_len;
    }

    // Find common suffix
    size_t suffix_len = 0;
    while (suffix_len < (old_c.size() - prefix_len) &&
           suffix_len < (new_c.size() - prefix_len) &&
           old_c[old_c.size() - 1 - suffix_len] == new_c[new_c.size() - 1 - suffix_len]) {
        ++suffix_len;
    }

    // Common prefix
    if (prefix_len > 0) {
        out << old_c.substr(0, prefix_len);
    }

    // Deleted portion
    size_t old_mid_len = old_c.size() - prefix_len - suffix_len;
    if (old_mid_len > 0) {
        out << "\033[31;9m" << old_c.substr(prefix_len, old_mid_len) << "\033[0m";
    }

    // Added portion
    size_t new_mid_len = new_c.size() - prefix_len - suffix_len;
    if (new_mid_len > 0) {
        out << "\033[32m" << new_c.substr(prefix_len, new_mid_len) << "\033[0m";
    }

    // Common suffix
    if (suffix_len > 0) {
        out << old_c.substr(old_c.size() - suffix_len);
    }

    return out.str();
}

} // namespace cc::ui::messages
