module;
#include <string>
#include <string_view>
#include <sstream>
#include <algorithm>
#include <vector>

export module cc.tools.file_edit_prompt;


import cc.tools.file_edit_types;

export namespace cc::tools {


inline auto get_file_edit_prompt() -> std::string {
    return R"(## FileEditTool

Performs exact string replacements in files. Use this tool to make precise edits.

### Rules:
1. You MUST read the file first before editing. The tool will reject edits on unread files.
2. The `old_text` must match EXACTLY what appears in the file (including indentation).
3. The edit will FAIL if `old_text` is not unique. Provide more surrounding context to disambiguate.
4. Use `replace_all: true` to replace every occurrence of `old_text`.
5. NEVER include line numbers in `old_text` or `new_text`.
6. Prefer editing existing files over creating new ones.

### Parameters:
- `file_path` (required): Absolute path to the file to modify
- `old_string` (required): The exact text to replace
- `new_string` (required): The replacement text (must differ from old_string)
- `replace_all` (optional, default false): Replace all occurrences)";
}


inline auto format_edit_preview(
    const EditOperation& op,
    int context_lines
) -> std::string {
    std::ostringstream oss;
    oss << "--- " << op.file.string() << "\n";
    oss << "+++ " << op.file.string() << " (modified)\n";


    auto split_lines = [](std::string_view text) -> std::vector<std::string_view> {
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start < text.size()) {
            auto pos = text.find('\n', start);
            if (pos == std::string_view::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, pos - start));
            start = pos + 1;
        }
        return lines;
    };

    auto old_lines = split_lines(op.old_text);
    auto new_lines = split_lines(op.new_text);


    oss << "@@ removal/addition @@\n";


    for (const auto& line : old_lines) {
        oss << "- " << line << "\n";
    }


    for (const auto& line : new_lines) {
        oss << "+ " << line << "\n";
    }

    if (op.replace_all) {
        oss << "\n(replace_all: all occurrences will be replaced)\n";
    }

    return oss.str();
}


inline auto generate_edit_summary(const EditResult& result) -> std::string {
    std::ostringstream oss;

    if (result.success) {
        oss << "✓ Edit successful: " << result.replacements << " replacement(s) made";
        if (!result.affected_lines.empty()) {
            oss << " at line(s) ";
            for (size_t i = 0; i < result.affected_lines.size(); ++i) {
                if (i > 0) oss << ", ";
                if (i >= 5) {
                    oss << "... and " << (result.affected_lines.size() - i) << " more";
                    break;
                }
                oss << result.affected_lines[i];
            }
        }
    } else {
        oss << "✗ Edit failed: no replacements made";
    }

    return oss.str();
}

} // namespace cc::tools
