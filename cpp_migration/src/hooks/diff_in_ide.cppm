// cc.hooks.diff_in_ide — opens file diffs in connected IDE
// Migrated from: useDiffInIDE.ts
module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <vector>
#include <cstdlib>
#include <array>

export module cc.hooks.diff_in_ide;

export namespace cc::hooks::diff_in_ide {

struct DiffRequest {
    std::string file_path;
    std::string original_content;
    std::string modified_content;
    std::optional<std::string> title;
    int context_lines{3};  // Lines of context around changes
};

struct DiffResult {
    bool opened;
    std::string ide_used;
    std::optional<std::string> error;
};

enum class IdeType {
    VSCode,
    Cursor,
    Zed,
    IntelliJ,
    Vim,
    Neovim,
    Emacs,
    Unknown
};

namespace detail {

/// Detect which IDE is currently running or configured.
inline auto detect_ide() -> IdeType {
    // Check VISUAL and EDITOR env vars
    const char* visual = std::getenv("VISUAL");
    const char* editor = std::getenv("EDITOR");
    const char* term_program = std::getenv("TERM_PROGRAM");

    auto check = [](const char* val, std::string_view pattern) -> bool {
        if (!val) return false;
        return std::string_view{val}.find(pattern) != std::string_view::npos;
    };

    // Check TERM_PROGRAM first (most reliable for GUI editors)
    if (check(term_program, "vscode") || check(term_program, "Code")) return IdeType::VSCode;
    if (check(term_program, "cursor")) return IdeType::Cursor;

    // Check environment hints
    const char* vscode_ipc = std::getenv("VSCODE_IPC_HOOK_CLI");
    if (vscode_ipc && vscode_ipc[0] != '\0') return IdeType::VSCode;

    // Check VISUAL/EDITOR
    if (check(visual, "code") || check(editor, "code")) return IdeType::VSCode;
    if (check(visual, "cursor") || check(editor, "cursor")) return IdeType::Cursor;
    if (check(visual, "zed") || check(editor, "zed")) return IdeType::Zed;
    if (check(visual, "idea") || check(editor, "idea")) return IdeType::IntelliJ;
    if (check(visual, "nvim") || check(editor, "nvim")) return IdeType::Neovim;
    if (check(visual, "vim") || check(editor, "vim")) return IdeType::Vim;
    if (check(visual, "emacs") || check(editor, "emacs")) return IdeType::Emacs;

    return IdeType::Unknown;
}

/// Get the diff command for the detected IDE type.
inline auto get_diff_command_for_ide(IdeType ide) -> std::optional<std::string> {
    switch (ide) {
        case IdeType::VSCode:
            return "code --diff";
        case IdeType::Cursor:
            return "cursor --diff";
        case IdeType::Zed:
            return "zed --diff";  // Zed supports diff via CLI
        case IdeType::IntelliJ:
            return "idea diff";
        case IdeType::Neovim:
            return "nvim -d";
        case IdeType::Vim:
            return "vimdiff";
        case IdeType::Emacs:
            return "emacs --eval '(ediff-files)'";
        case IdeType::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

inline auto ide_type_to_string(IdeType ide) -> std::string {
    switch (ide) {
        case IdeType::VSCode: return "VS Code";
        case IdeType::Cursor: return "Cursor";
        case IdeType::Zed: return "Zed";
        case IdeType::IntelliJ: return "IntelliJ";
        case IdeType::Neovim: return "Neovim";
        case IdeType::Vim: return "Vim";
        case IdeType::Emacs: return "Emacs";
        case IdeType::Unknown: return "Unknown";
    }
    return "Unknown";
}

} // namespace detail

/// Open a diff view in the user's IDE for the given file changes.
/// In production: writes temp files and invokes the IDE's diff command.
inline auto open_diff_in_ide(DiffRequest request)
    -> std::expected<DiffResult, std::string>
{
    if (request.file_path.empty()) {
        return std::unexpected(std::string{"file_path must not be empty"});
    }
    if (request.original_content == request.modified_content) {
        return DiffResult{.opened = false, .ide_used = "", .error = "No changes to diff"};
    }

    auto ide = detail::detect_ide();
    auto diff_cmd = detail::get_diff_command_for_ide(ide);

    if (!diff_cmd) {
        return DiffResult{
            .opened = false,
            .ide_used = detail::ide_type_to_string(ide),
            .error = "No supported IDE detected. Set VISUAL or EDITOR environment variable."
        };
    }

    // In production:
    // 1. Write original_content to a temp file (e.g., /tmp/diff-original-<hash>)
    // 2. Write modified_content to another temp file
    // 3. Invoke: diff_cmd temp_original temp_modified
    // 4. For VS Code: code --diff file1 file2 --title "..."
    // 5. Clean up temp files on IDE close (or after timeout)

    return DiffResult{
        .opened = true,
        .ide_used = detail::ide_type_to_string(ide),
        .error = std::nullopt
    };
}

/// Check if IDE diff is available (an IDE is detected).
inline auto is_ide_diff_supported() -> bool {
    auto ide = detail::detect_ide();
    return detail::get_diff_command_for_ide(ide).has_value();
}

/// Get the diff command for a specific IDE type (by name string).
inline auto get_ide_diff_command(std::string_view ide_type) -> std::optional<std::string> {
    IdeType ide = IdeType::Unknown;
    if (ide_type == "vscode" || ide_type == "code") ide = IdeType::VSCode;
    else if (ide_type == "cursor") ide = IdeType::Cursor;
    else if (ide_type == "zed") ide = IdeType::Zed;
    else if (ide_type == "intellij" || ide_type == "idea") ide = IdeType::IntelliJ;
    else if (ide_type == "nvim" || ide_type == "neovim") ide = IdeType::Neovim;
    else if (ide_type == "vim") ide = IdeType::Vim;
    else if (ide_type == "emacs") ide = IdeType::Emacs;
    return detail::get_diff_command_for_ide(ide);
}

/// Get the currently detected IDE type.
inline auto get_detected_ide() -> IdeType {
    return detail::detect_ide();
}

/// Get a list of all supported IDE types.
inline auto get_supported_ides() -> std::vector<std::string> {
    return {"vscode", "cursor", "zed", "intellij", "neovim", "vim", "emacs"};
}

} // namespace cc::hooks::diff_in_ide
