module;
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.tool_messages;

import cc.ui.layout;

export namespace cc::ui::tool_messages {

// --- File edit diff types ---

// A single edit operation within a file
struct FileEdit {
    std::string old_string;
    std::string new_string;
};

// A hunk in a unified diff
struct DiffHunk {
    int old_start;
    int old_count;
    int new_start;
    int new_count;
    std::vector<std::string> lines;  // prefixed with ' ', '+', or '-'
};

// Full structured patch for display
struct StructuredPatch {
    std::string file_path;
    std::vector<DiffHunk> hunks;
};

// Props for the file edit diff component
struct FileEditToolDiffProps {
    std::string file_path;
    std::vector<FileEdit> edits;
};

// Diff rendering context configuration
inline constexpr int kContextLines = 3;
inline constexpr int kChunkSize = 4096;

// --- Diff computation ---

// Line-level diff between two texts
enum class DiffLineType { Context, Added, Removed };

struct DiffLine {
    DiffLineType type;
    std::string content;
    int old_line_num;  // -1 if not applicable
    int new_line_num;  // -1 if not applicable
};

// Split text into lines
[[nodiscard]] inline auto split_lines(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// Compute a simple line diff between original and updated content
[[nodiscard]] inline auto compute_line_diff(std::string_view original,
                                             std::string_view updated)
    -> std::vector<DiffLine> {
    auto old_lines = split_lines(original);
    auto new_lines = split_lines(updated);
    std::vector<DiffLine> result;

    // Simple LCS-based diff (Myers algorithm approximation)
    std::size_t i = 0, j = 0;
    int old_num = 1, new_num = 1;
    while (i < old_lines.size() && j < new_lines.size()) {
        if (old_lines[i] == new_lines[j]) {
            result.push_back({DiffLineType::Context, std::string(old_lines[i]), old_num, new_num});
            ++i; ++j; ++old_num; ++new_num;
        } else {
            // Look ahead for sync point
            bool found = false;
            for (std::size_t look = 1; look < 5 && !found; ++look) {
                if (j + look < new_lines.size() && old_lines[i] == new_lines[j + look]) {
                    for (std::size_t k = 0; k < look; ++k) {
                        result.push_back({DiffLineType::Added, std::string(new_lines[j + k]), -1, new_num});
                        ++new_num;
                    }
                    j += look;
                    found = true;
                }
                if (i + look < old_lines.size() && old_lines[i + look] == new_lines[j]) {
                    for (std::size_t k = 0; k < look; ++k) {
                        result.push_back({DiffLineType::Removed, std::string(old_lines[i + k]), old_num, -1});
                        ++old_num;
                    }
                    i += look;
                    found = true;
                }
            }
            if (!found) {
                result.push_back({DiffLineType::Removed, std::string(old_lines[i]), old_num, -1});
                result.push_back({DiffLineType::Added, std::string(new_lines[j]), -1, new_num});
                ++i; ++j; ++old_num; ++new_num;
            }
        }
    }
    while (i < old_lines.size()) {
        result.push_back({DiffLineType::Removed, std::string(old_lines[i]), old_num, -1});
        ++i; ++old_num;
    }
    while (j < new_lines.size()) {
        result.push_back({DiffLineType::Added, std::string(new_lines[j]), -1, new_num});
        ++j; ++new_num;
    }
    return result;
}

// --- Diff rendering ---

// Render a diff line with ANSI colors
[[nodiscard]] inline auto render_diff_line(const DiffLine& line) -> std::string {
    switch (line.type) {
        case DiffLineType::Added:
            return "\033[32m+ " + line.content + "\033[0m";
        case DiffLineType::Removed:
            return "\033[31m- " + line.content + "\033[0m";
        case DiffLineType::Context:
            return "  " + line.content;
    }
    return "  " + line.content;
}

// Render a structured patch with hunks and context
[[nodiscard]] inline auto render_structured_diff(const std::vector<DiffLine>& diff_lines,
                                                  std::string_view file_path,
                                                  int terminal_width)
    -> std::string {
    std::string result;
    int width = std::max(terminal_width - 4, 40);

    // Header
    result += "\033[1m--- " + std::string(file_path) + "\033[0m\n";
    result += "\033[1m+++ " + std::string(file_path) + "\033[0m\n";

    // Filter to show only changed regions with context
    std::vector<bool> show_line(diff_lines.size(), false);
    for (std::size_t i = 0; i < diff_lines.size(); ++i) {
        if (diff_lines[i].type != DiffLineType::Context) {
            auto ctx_start = (i >= static_cast<std::size_t>(kContextLines))
                ? i - static_cast<std::size_t>(kContextLines) : 0;
            auto ctx_end = std::min(i + static_cast<std::size_t>(kContextLines) + 1, diff_lines.size());
            for (auto k = ctx_start; k < ctx_end; ++k) show_line[k] = true;
        }
    }

    bool in_hunk = false;
    for (std::size_t i = 0; i < diff_lines.size(); ++i) {
        if (show_line[i]) {
            if (!in_hunk) {
                result += "\033[36m@@ ... @@\033[0m\n";
                in_hunk = true;
            }
            auto rendered = render_diff_line(diff_lines[i]);
            if (static_cast<int>(rendered.size()) > width + 20) {  // account for ANSI escapes
                rendered = rendered.substr(0, static_cast<std::size_t>(width + 20));
            }
            result += rendered + "\n";
        } else {
            in_hunk = false;
        }
    }

    return result;
}

// Render the FileEditToolDiff component
[[nodiscard]] inline auto render_file_edit_diff(const FileEditToolDiffProps& props,
                                                 std::string_view file_content,
                                                 int terminal_width)
    -> std::expected<std::string, std::string> {
    if (props.edits.empty()) {
        return std::unexpected(std::string("No edits provided"));
    }

    // Apply edits sequentially to compute final content
    std::string current = std::string(file_content);
    for (const auto& edit : props.edits) {
        auto pos = current.find(edit.old_string);
        if (pos == std::string::npos) {
            return std::unexpected("Edit target not found in file: " + props.file_path);
        }
        current.replace(pos, edit.old_string.size(), edit.new_string);
    }

    // Compute diff between original and result
    auto diff = compute_line_diff(file_content, current);
    if (diff.empty()) {
        return std::string("\033[2mNo changes\033[0m");
    }

    return render_structured_diff(diff, props.file_path, terminal_width);
}

// --- FTXUI element factories ---

// Create an FTXUI Element showing the file edit diff
[[nodiscard]] auto make_file_edit_diff_element(
    const FileEditToolDiffProps& props,
    std::string_view file_content,
    int terminal_width) -> ftxui::Element;

// Create an FTXUI Element for generic tool output display
[[nodiscard]] auto make_tool_output_element(
    std::string_view tool_name,
    std::string_view output,
    bool is_error,
    int terminal_width) -> ftxui::Element;

} // namespace cc::ui::tool_messages
