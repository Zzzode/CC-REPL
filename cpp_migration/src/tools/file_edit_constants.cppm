// FileEditTool constants: tool name, permission patterns, error messages.
// Mirrors src/tools/FileEditTool/constants.ts
module;
#include <string>
#include <string_view>

export module cc.tools.file_edit_constants;

export namespace cc::tools::file_edit {

inline constexpr std::string_view FILE_EDIT_TOOL_NAME = "Edit";

/// Permission pattern for granting session-level access to the project's .claude/ folder
inline constexpr std::string_view CLAUDE_FOLDER_PERMISSION_PATTERN = "/.claude/**";

/// Permission pattern for granting session-level access to the global ~/.claude/ folder
inline constexpr std::string_view GLOBAL_CLAUDE_FOLDER_PERMISSION_PATTERN = "~/.claude/**";

/// Error message when the file was modified between read and write
inline constexpr std::string_view FILE_UNEXPECTEDLY_MODIFIED_ERROR =
    "File has been unexpectedly modified. Read it again before attempting to write it.";

} // namespace cc::tools::file_edit
