module;

#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <sstream>
#include <algorithm>

export module cc.utils.git_diff;

export namespace cc::utils {

namespace fs = std::filesystem;

// A single line in a diff hunk
struct DiffLine {
    enum class Type { Context, Added, Removed };
    Type type;
    std::string content;
};

// A contiguous diff hunk with line ranges and content
struct DiffHunk {
    int old_start = 0;
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::string header;
    std::vector<DiffLine> lines;
};

// A complete file diff with metadata and hunks
struct FileDiff {
    fs::path old_path;
    fs::path new_path;
    std::vector<DiffHunk> hunks;
    bool is_binary = false;
};

// Statistics for a collection of diffs
struct DiffStats {
    int additions = 0;
    int deletions = 0;
    int files_changed = 0;
};

// Parse a @@ header line and extract line numbers
inline bool parse_hunk_header(std::string_view line, DiffHunk& hunk) {
    // Format: @@ -old_start[,old_count] +new_start[,new_count] @@[ header]
    auto at_pos = line.find("@@", 2);
    if (at_pos == std::string_view::npos) return false;

    std::string range_str(line.substr(3, at_pos - 4));
    auto header_pos = at_pos + 2;
    if (header_pos < line.size() && line[header_pos] == ' ') {
        ++header_pos;
    }
    hunk.header = header_pos < line.size()
        ? std::string(line.substr(header_pos))
        : std::string{};

    // Parse -old_start,old_count
    auto space = range_str.find(' ');
    std::string old_range = range_str.substr(0, space);
    std::string new_range = (space != std::string::npos) ? range_str.substr(space + 1) : "";

    // Parse old range (-X,Y or -X)
    if (!old_range.empty() && old_range[0] == '-') {
        auto comma = old_range.find(',');
        hunk.old_start = std::stoi(old_range.substr(1, comma - 1));
        hunk.old_count = (comma != std::string::npos) ? std::stoi(old_range.substr(comma + 1)) : 1;
    }

    // Parse new range (+X,Y or +X)
    if (!new_range.empty() && new_range[0] == '+') {
        auto comma = new_range.find(',');
        hunk.new_start = std::stoi(new_range.substr(1, comma - 1));
        hunk.new_count = (comma != std::string::npos) ? std::stoi(new_range.substr(comma + 1)) : 1;
    }

    return true;
}

// Parse unified diff format into structured representation
inline std::vector<FileDiff> parse_unified_diff(std::string_view input) {
    std::vector<FileDiff> diffs;
    std::istringstream stream{std::string(input)};
    std::string line;

    FileDiff* current_diff = nullptr;
    DiffHunk* current_hunk = nullptr;

    while (std::getline(stream, line)) {
        if (line.starts_with("diff --git ")) {
            // Start of a new file diff
            diffs.emplace_back();
            current_diff = &diffs.back();
            current_hunk = nullptr;

            // Parse "diff --git a/path b/path"
            auto a_pos = line.find(" a/");
            auto b_pos = line.find(" b/", a_pos + 1);
            if (a_pos != std::string::npos && b_pos != std::string::npos) {
                current_diff->old_path = line.substr(a_pos + 3, b_pos - a_pos - 3);
                current_diff->new_path = line.substr(b_pos + 3);
            }
        } else if (line.starts_with("--- ") && current_diff) {
            // Old file path (already captured from diff line)
            if (line.size() > 6 && line.substr(4, 2) == "a/") {
                current_diff->old_path = line.substr(6);
            }
        } else if (line.starts_with("+++ ") && current_diff) {
            // New file path
            if (line.size() > 6 && line.substr(4, 2) == "b/") {
                current_diff->new_path = line.substr(6);
            }
        } else if (line.starts_with("Binary files") && current_diff) {
            current_diff->is_binary = true;
        } else if (line.starts_with("@@ ") && current_diff) {
            // Hunk header
            current_diff->hunks.emplace_back();
            current_hunk = &current_diff->hunks.back();
            parse_hunk_header(line, *current_hunk);
        } else if (current_hunk) {
            // Diff content lines
            if (line.starts_with("+")) {
                current_hunk->lines.push_back({DiffLine::Type::Added, line.substr(1)});
            } else if (line.starts_with("-")) {
                current_hunk->lines.push_back({DiffLine::Type::Removed, line.substr(1)});
            } else if (line.starts_with(" ")) {
                current_hunk->lines.push_back({DiffLine::Type::Context, line.substr(1)});
            }
        }
    }

    return diffs;
}

// Generate unified diff from two strings
inline std::string generate_unified_diff(std::string_view old_content,
                                         std::string_view new_content,
                                         std::string_view filename) {
    // Split into lines
    auto split_lines = [](std::string_view sv) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::istringstream stream{std::string{sv}};
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(std::move(line));
        }
        return lines;
    };

    auto old_lines = split_lines(old_content);
    auto new_lines = split_lines(new_content);

    if (old_lines == new_lines) {
        return "";
    }

    std::vector<std::vector<int>> lcs(
        old_lines.size() + 1,
        std::vector<int>(new_lines.size() + 1, 0));
    for (std::size_t i = old_lines.size(); i-- > 0;) {
        for (std::size_t j = new_lines.size(); j-- > 0;) {
            if (old_lines[i] == new_lines[j]) {
                lcs[i][j] = lcs[i + 1][j + 1] + 1;
            } else {
                lcs[i][j] = std::max(lcs[i + 1][j], lcs[i][j + 1]);
            }
        }
    }

    std::vector<DiffLine> diff_lines;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < old_lines.size() || new_index < new_lines.size()) {
        if (old_index < old_lines.size() &&
            new_index < new_lines.size() &&
            old_lines[old_index] == new_lines[new_index]) {
            diff_lines.push_back({DiffLine::Type::Context, old_lines[old_index]});
            ++old_index;
            ++new_index;
        } else if (new_index < new_lines.size() &&
                   (old_index == old_lines.size() ||
                    lcs[old_index][new_index + 1] >= lcs[old_index + 1][new_index])) {
            diff_lines.push_back({DiffLine::Type::Added, new_lines[new_index]});
            ++new_index;
        } else if (old_index < old_lines.size()) {
            diff_lines.push_back({DiffLine::Type::Removed, old_lines[old_index]});
            ++old_index;
        }
    }

    std::string result;
    result += "diff --git a/" + std::string(filename) + " b/" + std::string(filename) + "\n";
    result += "--- a/" + std::string(filename) + "\n";
    result += "+++ b/" + std::string(filename) + "\n";

    int old_count = static_cast<int>(old_lines.size());
    int new_count = static_cast<int>(new_lines.size());
    int old_start = old_count == 0 ? 0 : 1;
    int new_start = new_count == 0 ? 0 : 1;

    result += "@@ -" + std::to_string(old_start) + "," + std::to_string(old_count) +
              " +" + std::to_string(new_start) + "," + std::to_string(new_count) + " @@\n";

    for (const auto& line : diff_lines) {
        switch (line.type) {
            case DiffLine::Type::Context:
                result += " " + line.content + "\n";
                break;
            case DiffLine::Type::Added:
                result += "+" + line.content + "\n";
                break;
            case DiffLine::Type::Removed:
                result += "-" + line.content + "\n";
                break;
        }
    }

    return result;
}

// Calculate diff statistics from parsed diffs
inline DiffStats get_diff_stats(const std::vector<FileDiff>& diffs) {
    DiffStats stats;
    stats.files_changed = static_cast<int>(diffs.size());

    for (const auto& diff : diffs) {
        for (const auto& hunk : diff.hunks) {
            for (const auto& line : hunk.lines) {
                if (line.type == DiffLine::Type::Added) ++stats.additions;
                else if (line.type == DiffLine::Type::Removed) ++stats.deletions;
            }
        }
    }

    return stats;
}

} // namespace cc::utils
