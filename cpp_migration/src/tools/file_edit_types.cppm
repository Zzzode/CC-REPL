// FileEditTool types — migrated from src/tools/FileEditTool/types.ts + constants.ts
// Agent 9: audit completed 2026-06-09. All TS types / error codes / constants ported.
module;
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <expected>

export module cc.tools.file_edit_types;

export namespace cc::tools::file_edit {

// ===========================================================================
// Constants from constants.ts
// ===========================================================================

/// Tool name registered with the tool registry (= FILE_EDIT_TOOL_NAME in TS)
inline constexpr std::string_view kToolName = "Edit";

/// Permission pattern for granting session-level access to the project's
/// .claude/ folder (= CLAUDE_FOLDER_PERMISSION_PATTERN in TS)
inline constexpr std::string_view kClaudeFolderPermissionPattern = "/.claude/**";

/// Permission pattern for granting session-level access to the global
/// ~/.claude/ folder (= GLOBAL_CLAUDE_FOLDER_PERMISSION_PATTERN in TS)
inline constexpr std::string_view kGlobalClaudeFolderPermissionPattern = "~/.claude/**";

/// Thrown when the file on disk changed between the read-time stamp check
/// and the actual write (= FILE_UNEXPECTEDLY_MODIFIED_ERROR in TS)
inline constexpr std::string_view kFileUnexpectedlyModifiedError =
    "File has been unexpectedly modified. Read it again before attempting to write it.";

// ===========================================================================
// Validation error codes from FileEditTool.ts validateInput() branches
// ===========================================================================

/// Error codes produced by the input validator. Each maps to a specific
/// rejection branch in the TS FileEditTool.validateInput() method.
enum class ValidationErrorCode : int {
    Ok                      = 0,   // no error
    OldEqualsNew            = 1,   // old_string == new_string
    PermissionDeniedDir     = 2,   // path matched a deny rule
    FileAlreadyExists       = 3,   // old_string == '' but file is non-empty
    FileNotFound            = 4,   // file missing and old_string != ''
    NotebookBlocked         = 5,   // .ipynb files -> use NotebookEditTool
    FileNotReadYet          = 6,   // no readFileState entry
    FileModifiedSinceRead   = 7,   // mtime newer than read timestamp
    OldStringNotFound       = 8,   // findActualString returned null
    MultipleMatchesNoReplaceAll = 9, // N>1 matches + replace_all=false
    FileTooLarge            = 10,  // exceeds MAX_EDIT_FILE_SIZE (1 GiB)
    SecretInTeamMemory      = 11,  // team-memory secrets guard rejected
    SettingsFileInvalid     = 12,  // validateInputForSettingsFileEdit rejected
};

/// Behavior hint for the caller when validation rejects an input.
enum class ValidationBehavior {
    Ask,   // surface a user-visible message / let the model retry
    Fail,  // hard error
};

/// Outcome of validate_input() — mirrors TS `{result, behavior?, message?, errorCode?, meta?}`.
struct ValidationOutcome {
    bool passed = true;
    ValidationBehavior behavior = ValidationBehavior::Fail;
    std::string message;
    ValidationErrorCode code = ValidationErrorCode::Ok;

    /// Arbitrary string-keyed metadata (TS uses this to pass
    /// actualOldString, isFilePathAbsolute, … through the call chain).
    std::vector<std::pair<std::string, std::string>> meta;

    /// Convenience factory
    static ValidationOutcome ok() { return {}; }
    static ValidationOutcome ok_with(std::vector<std::pair<std::string, std::string>> m) {
        return {
            .passed = true,
            .behavior = ValidationBehavior::Fail,
            .message = {},
            .code = ValidationErrorCode::Ok,
            .meta = std::move(m)
        };
    }
    static ValidationOutcome ask(std::string msg, ValidationErrorCode c) {
        return {.passed = false,
                .behavior = ValidationBehavior::Ask,
                .message = std::move(msg),
                .code = c,
                .meta = {}};
    }
};

// ===========================================================================
// Types from types.ts
// ===========================================================================

/// Parsed input from the model (= FileEditInput, z.output side in TS).
/// replace_all is always a defined bool after schema/semanticBoolean pass.
struct FileEditInput {
    std::filesystem::path file_path;
    std::string old_string;
    std::string new_string;
    bool replace_all = false;
};

/// Single edit without file_path (= EditInput in TS — the element type
/// used by HashlineEditTool / normalization helpers that operate on a
/// collection of edits against a single file).
struct EditInput {
    std::string old_string;
    std::string new_string;
    /// Runtime value; may be absent when built from a partial input.
    std::optional<bool> replace_all;
};

/// Runtime edit with replace_all always resolved (= FileEdit in TS).
struct FileEdit {
    std::string old_string;
    std::string new_string;
    bool replace_all = false;
};

/// Diff hunk (= hunkSchema in TS; note TS uses `StructuredPatchHunk`
/// from the `diff` npm package — we keep the names aligned with TS field
/// names, not the npm ones).
struct PatchHunk {
    int old_start = 0;   // oldStart
    int old_lines = 0;   // oldLines
    int new_start = 0;   // newStart
    int new_lines = 0;   // newLines
    std::vector<std::string> lines;
};

/// Git diff summary block (= gitDiffSchema in TS).
enum class GitDiffStatus { Modified, Added };

struct GitDiffInfo {
    std::string filename;
    GitDiffStatus status = GitDiffStatus::Modified;
    std::uint32_t additions = 0;
    std::uint32_t deletions = 0;
    std::uint32_t changes = 0;
    std::string patch;
    /// GitHub "owner/repo" when available (optional + nullable in TS).
    std::optional<std::string> repository;
};

/// FileEditTool output (= FileEditOutput / outputSchema in TS).
struct FileEditOutput {
    std::filesystem::path file_path;     // filePath
    std::string old_string;              // oldString
    std::string new_string;              // newString (from model, NOT actualNew)
    std::string original_file;           // originalFile — pre-edit content
    std::vector<PatchHunk> structured_patch;
    bool user_modified = false;          // userModified
    bool replace_all = false;            // replaceAll
    std::optional<GitDiffInfo> git_diff; // gitDiff (optional)
};

// ===========================================================================
// Legacy EditError / helpers (kept for backward compat with
// file_edit_prompt.cppm callers; migrated as-is).
// ===========================================================================

enum class EditError {
    FileNotFound,
    OldTextNotFound,
    MultipleMatches,
    PermissionDenied,
    BinaryFile,
};

/// Quick validator for simple callers (not used by the full FileEditTool
/// pipeline which runs the full validate_input() chain).
inline auto validate_edit_simple(const FileEditInput& op) -> std::expected<void, EditError> {
    namespace fs = std::filesystem;

    if (!fs::exists(op.file_path)) {
        return std::unexpected(EditError::FileNotFound);
    }

    auto perms = fs::status(op.file_path).permissions();
    if ((perms & fs::perms::owner_write) == fs::perms::none) {
        return std::unexpected(EditError::PermissionDenied);
    }

    auto ext = op.file_path.extension().string();
    static const std::vector<std::string> binary_extensions = {
        ".bin", ".exe", ".dll", ".so", ".dylib", ".o", ".obj",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico",
        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar",
        ".pdf", ".doc", ".xls", ".ppt"
    };
    for (const auto& bin_ext : binary_extensions) {
        if (ext == bin_ext) {
            return std::unexpected(EditError::BinaryFile);
        }
    }

    if (op.old_string.empty()) {
        return std::unexpected(EditError::OldTextNotFound);
    }

    return {};
}

inline auto edit_error_to_string(EditError error) -> std::string_view {
    switch (error) {
        case EditError::FileNotFound:    return "File not found";
        case EditError::OldTextNotFound: return "Old text not found in file";
        case EditError::MultipleMatches: return "Multiple matches found; set replace_all=true or provide more context";
        case EditError::PermissionDenied: return "Permission denied";
        case EditError::BinaryFile:      return "Cannot edit binary file";
    }
    return "Unknown error";
}

} // namespace cc::tools::file_edit
