// Editor utilities: launch external editors, temp file management, prompt editing
// Sources: editor.ts, promptEditor.ts
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.editor_utils;

import cc.utils.async;

export namespace cc::utils::editor {

// =========================================================================
// EditorOptions - Configuration for launching an external editor
// =========================================================================
struct EditorOptions {
    std::string editor;
    std::vector<std::string> args;
    std::optional<std::chrono::milliseconds> timeout;
    std::function<void(std::string_view)> on_progress;
};

/// Overload: create EditorOptions with just editor name
[[nodiscard]] inline EditorOptions make_editor_options(std::string editor) {
    return EditorOptions{
        .editor = std::move(editor),
        .args = {},
        .timeout = std::nullopt,
        .on_progress = nullptr,
    };
}

/// Overload: create EditorOptions with editor and args
[[nodiscard]] inline EditorOptions make_editor_options(
    std::string editor,
    std::vector<std::string> args
) {
    return EditorOptions{
        .editor = std::move(editor),
        .args = std::move(args),
        .timeout = std::nullopt,
        .on_progress = nullptr,
    };
}

// =========================================================================
// EditorResult - Result of running an editor session
// =========================================================================
struct EditorResult {
    std::string content;
    bool was_modified;
    int32_t exit_code;
};

// =========================================================================
// PromptEditorOptions - Extended options for prompt editing
// =========================================================================
struct PromptEditorOptions {
    EditorOptions base;
    std::string file_extension;       // e.g. ".md"
    std::string header_comment;       // comment prepended to file
    bool strip_header_on_read;        // remove header when reading back
};

/// Overload: create PromptEditorOptions with just base options
[[nodiscard]] inline PromptEditorOptions make_prompt_editor_options(
    EditorOptions base
) {
    return PromptEditorOptions{
        .base = std::move(base),
        .file_extension = ".md",
        .header_comment = {},
        .strip_header_on_read = true,
    };
}

// =========================================================================
// TempFile - RAII wrapper for temporary files used by editor sessions
// =========================================================================
class TempFile {
public:
    TempFile(std::filesystem::path path, std::string initial_content);
    ~TempFile();

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) noexcept;
    TempFile& operator=(TempFile&&) noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::expected<std::string, std::string> read() const;

private:
    std::filesystem::path path_;
    bool owns_file_;
};

// =========================================================================
// Editor detection and command resolution
// =========================================================================

/// Detect the user's preferred editor from $VISUAL / $EDITOR / fallback
[[nodiscard]] std::expected<std::string, std::string> get_editor_command();

/// Create a temporary file with the given content and extension
[[nodiscard]] std::expected<TempFile, std::string> create_temp_file(
    std::string_view content,
    std::string_view extension
);

/// Clean up a temporary file (called automatically by TempFile destructor)
void cleanup_temp_file(const std::filesystem::path& path) noexcept;

// =========================================================================
// Editor launching
// =========================================================================

/// Launch the specified editor on a file path and wait for exit
[[nodiscard]] async::Task<std::expected<EditorResult, std::string>>
launch_editor(
    std::string_view file_path,
    const EditorOptions& options
);

/// Open text in an editor, return edited text when the editor closes
[[nodiscard]] async::Task<std::expected<std::string, std::string>>
edit_text_in_editor(
    std::string_view text,
    const EditorOptions& options
);

// =========================================================================
// Prompt editing (higher-level wrappers)
// =========================================================================

/// Edit a prompt string in an external editor
[[nodiscard]] async::Task<std::expected<std::string, std::string>>
edit_prompt_in_editor(
    std::string_view prompt,
    const PromptEditorOptions& options
);

/// Edit a prompt with a system message displayed as a header comment
[[nodiscard]] async::Task<std::expected<std::string, std::string>>
edit_prompt_with_system_message(
    std::string_view prompt,
    std::string_view system_message,
    const PromptEditorOptions& options
);

} // namespace cc::utils::editor
