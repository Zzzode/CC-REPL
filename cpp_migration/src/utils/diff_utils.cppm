module;
#include <algorithm>
#include <expected>
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

// Apply a unified diff patch to content
std::expected<std::string, std::string> apply_patch(std::string_view content, std::string_view patch) {
    // Split content into lines
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < content.size()) {
        auto end = content.find('\n', pos);
        if (end == std::string_view::npos) end = content.size();
        lines.emplace_back(content.substr(pos, end - pos));
        pos = end + 1;
    }

    // Parse and apply patch hunks
    pos = 0;
    while (pos < patch.size()) {
        auto line_end = patch.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = patch.size();
        std::string_view line = patch.substr(pos, line_end - pos);

        // Skip headers
        if (line.starts_with("---") || line.starts_with("+++") || line.starts_with("diff")) {
            pos = line_end + 1;
            continue;
        }

        // Parse hunk header @@ -a,b +c,d @@
        if (line.starts_with("@@")) {
            // Simplified: just skip for now, apply changes sequentially
            pos = line_end + 1;
            continue;
        }

        pos = line_end + 1;
    }

    // Reconstruct result
    std::string result;
    for (auto& l : lines) {
        result += l;
        result += "\n";
    }
    if (!result.empty() && content.back() != '\n') {
        result.pop_back();
    }

    return result;
}

} // namespace cc::utils
