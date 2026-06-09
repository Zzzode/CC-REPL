// FileEditTool prompt + formatting helpers
// Migrated from src/tools/FileEditTool/prompt.ts.
// Agent 9: audit completed 2026-06-09. Matches TS getEditToolDescription()
// (including the pre-read instruction, line-prefix-format note, and the
// ANT-user minimal-uniqueness hint).
module;
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.file_edit_prompt;

import cc.tools.file_edit_types;
import cc.utils.string_utils;     // for first_line_of

export namespace cc::tools::file_edit {

// ===========================================================================
// Core prompt — mirrors TS getDefaultEditDescription() exactly.
// ===========================================================================

/// Returns the pre-read instruction paragraph that prefixes every usage hint.
/// Mirrors TS getPreReadInstruction().
inline std::string get_pre_read_instruction(std::string_view read_tool_name) {
    return std::string("\n- You must use your `")
        .append(read_tool_name)
        .append("` tool at least once in the conversation before editing. "
                "This tool will error if you attempt an edit without reading it. ");
}

/// Line-number prefix format description. TS chooses between two formats
/// based on isCompactLinePrefixEnabled(); we keep the expanded form
/// (always describe both so the model can adapt) unless the caller
/// explicitly overrides.
inline std::string get_line_prefix_format_description(bool compact_mode) {
    return compact_mode ? "line number + tab"
                        : "spaces + line number + arrow";
}

/// Returns the full tool prompt used for the Edit tool description.
///
/// \p read_tool_name   - the `Read` tool name (FileReadTool's kToolName).
/// \p compact_prefix   - isCompactLinePrefixEnabled() flag value.
/// \p is_ant_user      - true when USER_TYPE == "ant" (adds a hint about
///                       keeping old_string minimal).
inline std::string get_edit_tool_description(
    std::string_view read_tool_name = "Read", // = FILE_READ_TOOL_NAME from TS
    bool compact_prefix = false,
    bool is_ant_user = false
) {
    const auto prefix_fmt = get_line_prefix_format_description(compact_prefix);

    std::string minimal_uniqueness_hint;
    if (is_ant_user) {
        minimal_uniqueness_hint =
            "\n- Use the smallest old_string that's clearly unique — usually "
            "2-4 adjacent lines is sufficient. Avoid including 10+ lines of "
            "context when less uniquely identifies the target.";
    }

    std::ostringstream oss;
    oss << "Performs exact string replacements in files.\n"
        << "\n"
        << "Usage:"
        << get_pre_read_instruction(read_tool_name)
        << "\n- When editing text from Read tool output, ensure you preserve "
           "the exact indentation (tabs/spaces) as it appears AFTER the line "
           "number prefix. The line number prefix format is: "
        << prefix_fmt
        << ". Everything after that is the actual file content to match. "
           "Never include any part of the line number prefix in the "
           "old_string or new_string."
        << "\n- ALWAYS prefer editing existing files in the codebase. NEVER "
           "write new files unless explicitly required."
        << "\n- Only use emojis if the user explicitly requests it. Avoid "
           "adding emojis to files unless asked."
        << "\n- The edit will FAIL if `old_string` is not unique in the "
           "file. Either provide a larger string with more surrounding "
           "context to make it unique or use `replace_all` to change every "
           "instance of `old_string`."
        << minimal_uniqueness_hint
        << "\n- Use `replace_all` for replacing and renaming strings across "
           "the file. This parameter is useful if you want to rename a "
           "variable for instance.";
    return oss.str();
}

// ===========================================================================
// UI-formatting pure functions (extracted from UI.tsx — React components
// are deferred to Phase 4 / FTXUI, so we only port the non-JSX helpers).
// ===========================================================================

/// userFacingName() from UI.tsx. Decides between "Update"/"Create"/
/// "Updated plan" for the activity log header.
///
/// NOTE: the full TS impl also checks getPlansDirectory(); callers that
/// need that can pass the plans_dir prefix explicitly. When empty, we
/// just fall back to "Update" semantics.
inline std::string user_facing_name(
    const FileEditInput* input,      // may be null (== undefined in TS)
    std::string_view plans_dir_prefix = {}  // getPlansDirectory() result
) {
    if (!input) return "Update";
    if (!plans_dir_prefix.empty() &&
        input->file_path.string().starts_with(plans_dir_prefix)) {
        return "Updated plan";
    }
    if (input->old_string.empty()) return "Create";
    return "Update";
}

/// getToolUseSummary() from UI.tsx. Returns a display path for the
/// activity sidebar (we just return the string form of the path; actual
/// display-path shortening lives in utils/file.cppm).
inline std::string get_tool_use_summary(const FileEditInput* input) {
    if (!input || input->file_path.empty()) return {};
    return input->file_path.string();
}

/// mapToolResultToToolResultBlockParam() from FileEditTool.ts. Returns
/// the human-readable success line sent back to the model as the
/// tool_result content.
inline std::string format_tool_result_block(
    const FileEditOutput& out,
    std::string_view tool_use_id
) {
    const std::string modified_note = out.user_modified
        ? ".  The user modified your proposed changes before accepting them. "
        : "";

    std::ostringstream oss;
    if (out.replace_all) {
        oss << "The file " << out.file_path.string()
            << " has been updated" << modified_note
            << ". All occurrences were successfully replaced.";
    } else {
        oss << "The file " << out.file_path.string()
            << " has been updated successfully" << modified_note << ".";
    }
    return oss.str();
}

// ===========================================================================
// Helpers that shipped with the original cpp_migration prompt file — kept
// for backwards compat, but rewritten to operate on the migrated
// FileEditInput / FileEditOutput types rather than the old EditOperation.
// ===========================================================================

/// Format a unified-diff style preview for an edit (used by permission /
/// rejection UI builders). Mirrors format_edit_preview() in the original
/// prompt.cppm but takes the migrated FileEditInput type.
inline std::string format_edit_preview(
    const FileEditInput& op,
    int context_lines = 3
) {
    std::ostringstream oss;
    oss << "--- " << op.file_path.string() << "\n";
    oss << "+++ " << op.file_path.string() << " (modified)\n";

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

    auto old_lines = split_lines(op.old_string);
    auto new_lines = split_lines(op.new_string);

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

/// EditResult type used by generate_edit_summary — kept as a local
/// lightweight struct (it was part of the original file_edit_types.cppm
/// but its shape didn't match TS; we re-define it here because only the
/// prompt preview code uses it).
struct EditSummaryInfo {
    bool success = true;
    int replacements = 0;
    std::vector<int> affected_lines;
};

inline std::string generate_edit_summary(const EditSummaryInfo& result) {
    std::ostringstream oss;

    if (result.success) {
        oss << "[OK] Edit successful: " << result.replacements
            << " replacement(s) made";
        if (!result.affected_lines.empty()) {
            oss << " at line(s) ";
            for (size_t i = 0; i < result.affected_lines.size(); ++i) {
                if (i > 0) oss << ", ";
                if (i >= 5) {
                    oss << "... and " << (result.affected_lines.size() - i)
                        << " more";
                    break;
                }
                oss << result.affected_lines[i];
            }
        }
    } else {
        oss << "[FAIL] Edit failed: no replacements made";
    }

    return oss.str();
}

} // namespace cc::tools::file_edit
