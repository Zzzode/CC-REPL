/// @file diff_in_ide.cppm
/// @brief Open diff view in connected IDE (VS Code, JetBrains, etc.).
/// Supports bridge connection (MCP IDE extension) and shell-out fallback.
/// Faithful port of src/hooks/useDiffInIDE.ts.
module;

#include <array>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.diff_in_ide;

import cc.utils.bash_execution;
import cc.utils.path_utils;

export namespace cc::hooks::diff_in_ide {

namespace fs = std::filesystem;

// =========================================================================
// Types
// =========================================================================

/// Request to open a diff view in the IDE.
/// TS REF: src/hooks/useDiffInIDE.ts:32-44 (Props type)
struct DiffRequest {
    std::string file_path;          ///< Path to the file being edited
    std::string original_content;   ///< Original file content before edits
    std::string modified_content;   ///< Modified file content after edits
    std::optional<std::string> title; ///< Optional custom tab title
    int context_lines{3};           ///< Lines of context around changes
    std::optional<int> start_line;  ///< Optional start line for range selection
    std::optional<int> end_line;    ///< Optional end line for range selection
};

/// Result of attempting to open a diff in the IDE.
/// TS REF: src/hooks/useDiffInIDE.ts:52-57 (return type of useDiffInIDE)
struct DiffResult {
    bool opened{false};             ///< True if diff was successfully opened
    std::string ide_used;           ///< Name of the IDE that was used
    std::string tab_name;           ///< Generated tab name in IDE
    std::optional<std::string> error; ///< Error message if failed
};

/// Supported IDE types.
/// TS REF: src/hooks/useDiffInIDE.ts (IDE detection via getConnectedIdeName)
enum class IdeType {
    VSCode,
    Cursor,
    Zed,
    IntelliJ,
    CLion,
    Vim,
    Neovim,
    Emacs,
    Unknown
};

/// Messages that can come back from the IDE extension.
/// TS REF: src/hooks/useDiffInIDE.ts:299-378 (isSaveMessage, isClosedMessage, isRejectedMessage)
enum class IdeMessageType {
    FileSaved,      ///< User saved the file in IDE (accept edits)
    TabClosed,      ///< User closed the diff tab (accept as-is)
    DiffRejected,   ///< User rejected the diff in IDE
    Unknown,
};

/// A message received from the IDE extension.
struct IdeMessage {
    IdeMessageType type{IdeMessageType::Unknown};
    std::string text;  ///< New file content for FileSaved messages
};

// =========================================================================
// IDE detection
// =========================================================================

namespace detail {

/// Detect which IDE is currently running or configured.
/// TS REF: src/utils/ide.ts (getConnectedIdeName, hasAccessToIDEExtensionDiffFeature)
[[nodiscard]] inline auto detect_ide() -> IdeType {
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

    // Check environment hints for VS Code
    const char* vscode_ipc = std::getenv("VSCODE_IPC_HOOK_CLI");
    if (vscode_ipc && vscode_ipc[0] != '\0') return IdeType::VSCode;

    // Check JetBrains IDE indicators
    const char* jb_env = std::getenv("JB_ENVIRONMENT");
    if (jb_env && jb_env[0] != '\0') {
        std::string_view jb{jb_env};
        if (jb.find("CLION") != std::string_view::npos) return IdeType::CLion;
        if (jb.find("IDEA") != std::string_view::npos) return IdeType::IntelliJ;
        return IdeType::IntelliJ;
    }

    // Check VISUAL/EDITOR
    if (check(visual, "code") || check(editor, "code")) return IdeType::VSCode;
    if (check(visual, "cursor") || check(editor, "cursor")) return IdeType::Cursor;
    if (check(visual, "zed") || check(editor, "zed")) return IdeType::Zed;
    if (check(visual, "idea") || check(editor, "idea")) return IdeType::IntelliJ;
    if (check(visual, "clion") || check(editor, "clion")) return IdeType::CLion;
    if (check(visual, "nvim") || check(editor, "nvim")) return IdeType::Neovim;
    if (check(visual, "vim") || check(editor, "vim")) return IdeType::Vim;
    if (check(visual, "emacs") || check(editor, "emacs")) return IdeType::Emacs;

    return IdeType::Unknown;
}

/// Get the diff command for the detected IDE type.
[[nodiscard]] inline auto get_diff_command_for_ide(IdeType ide) -> std::optional<std::string> {
    switch (ide) {
        case IdeType::VSCode:   return "code --diff";
        case IdeType::Cursor:   return "cursor --diff";
        case IdeType::Zed:      return "zed --diff";
        case IdeType::IntelliJ: return "idea diff";
        case IdeType::CLion:    return "clion diff";
        case IdeType::Neovim:   return "nvim -d";
        case IdeType::Vim:      return "vimdiff";
        case IdeType::Emacs:    return "emacs --eval '(ediff-files)'";
        case IdeType::Unknown:  return std::nullopt;
    }
    return std::nullopt;
}

/// Convert IdeType to a human-readable name.
[[nodiscard]] inline auto ide_type_to_string(IdeType ide) -> std::string {
    switch (ide) {
        case IdeType::VSCode:   return "VS Code";
        case IdeType::Cursor:   return "Cursor";
        case IdeType::Zed:      return "Zed";
        case IdeType::IntelliJ: return "IntelliJ";
        case IdeType::CLion:    return "CLion";
        case IdeType::Neovim:   return "Neovim";
        case IdeType::Vim:      return "Vim";
        case IdeType::Emacs:    return "Emacs";
        case IdeType::Unknown:  return "Unknown";
    }
    return "Unknown";
}

/// Generate a short random hex nonce (6 chars) for tab name uniqueness.
/// TS REF: src/hooks/useDiffInIDE.ts:61 (randomUUID().slice(0, 6))
[[nodiscard]] inline auto generate_sha_nonce() -> std::string {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 15);

    std::string result;
    result.reserve(6);
    for (int i = 0; i < 6; ++i) {
        result += hex[dist(gen)];
    }
    return result;
}

/// Generate the IDE tab name for a diff view.
/// TS REF: src/hooks/useDiffInIDE.ts:62-65 (tabName = `✻ [Claude Code] ${basename(filePath)} (${sha}) ⧉`)
[[nodiscard]] inline auto generate_tab_name(std::string_view file_path, std::string_view sha) -> std::string {
    // Extract basename
    auto pos = file_path.find_last_of("/\\");
    std::string basename = (pos != std::string_view::npos)
        ? std::string{file_path.substr(pos + 1)}
        : std::string{file_path};

    return std::string{"[Claude Code] "} + basename + " (" + std::string{sha} + ")";
}

} // namespace detail

// =========================================================================
// Bridge / IDE extension support (stub)
// =========================================================================

namespace bridge {

/// Check if a bridge-connected IDE extension is available for diffing.
/// TS REF: src/utils/ide.ts (hasAccessToIDEExtensionDiffFeature, getConnectedIdeClient)
[[nodiscard]] inline auto has_bridge_ide() -> bool {
    // Check for VS Code extension IPC or JetBrains plugin connection
    const char* vscode_ipc = std::getenv("VSCODE_IPC_HOOK_CLI");
    if (vscode_ipc && vscode_ipc[0] != '\0') return true;

    const char* cc_ide_bridge = std::getenv("CC_REPL_IDE_BRIDGE");
    if (cc_ide_bridge && cc_ide_bridge[0] != '\0') return true;

    return false;
}

/// Get the name of the connected IDE via bridge.
/// TS REF: src/utils/ide.ts (getConnectedIdeName)
[[nodiscard]] inline auto get_bridge_ide_name() -> std::optional<std::string> {
    if (!has_bridge_ide()) return std::nullopt;

    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string_view tp{term_program};
        if (tp.find("vscode") != std::string_view::npos || tp.find("Code") != std::string_view::npos) {
            return "VS Code";
        }
        if (tp.find("cursor") != std::string_view::npos) {
            return "Cursor";
        }
    }
    return "IDE";
}

/// Send an RPC call to the connected IDE extension.
/// TS REF: src/utils/ide.ts (callIdeRpc)
/// This is a stub — full bridge RPC requires the MCP transport layer.
inline auto call_ide_rpc(std::string_view method, std::string_view params_json)
    -> std::expected<std::string, std::string>
{
    if (!has_bridge_ide()) {
        return std::unexpected(std::string{"no bridge IDE available"});
    }
    (void)params_json;  // used in full RPC implementation

    // Stub: in production this would send via MCP/WebSocket to the IDE extension
    // For now, log that the RPC was attempted
    return std::unexpected(std::string{"bridge RPC not yet implemented: "} + std::string{method});
}

} // namespace bridge

// =========================================================================
// Core API: open_diff_in_ide
// =========================================================================

/// Open a diff view in the user's IDE for the given file changes.
/// Attempts bridge connection first, falls back to shell-out CLI.
/// TS REF: src/hooks/useDiffInIDE.ts:77-138 (showDiff function)
/// @param request The diff request containing file path and contents
/// @return DiffResult indicating success/failure and which IDE was used
[[nodiscard]] inline auto open_diff_in_ide(DiffRequest request)
    -> std::expected<DiffResult, std::string>
{
    if (request.file_path.empty()) {
        return std::unexpected(std::string{"file_path must not be empty"});
    }
    if (request.original_content == request.modified_content) {
        return DiffResult{
            .opened = false,
            .ide_used = "",
            .tab_name = "",
            .error = std::string{"No changes to diff"}
        };
    }

    // Generate unique tab name
    auto sha = detail::generate_sha_nonce();
    auto tab_name = request.title.value_or(
        detail::generate_tab_name(request.file_path, sha)
    );

    // ── Attempt 1: Bridge-connected IDE extension ─────────────
    if (bridge::has_bridge_ide()) {
        // Build openDiff RPC params
        auto ide_name = bridge::get_bridge_ide_name().value_or("IDE");

        // Expand the file path
        auto expanded_path = cc::utils::expand_tilde(request.file_path);
        std::string path_str = expanded_path.string();

        // Build JSON params (simplified)
        std::string params = "{\"old_file_path\":\"" + path_str +
                           "\",\"new_file_path\":\"" + path_str +
                           "\",\"new_file_contents\":\"" + request.modified_content +
                           "\",\"tab_name\":\"" + tab_name + "\"}";

        auto rpc_result = bridge::call_ide_rpc("openDiff", params);
        if (rpc_result) {
            return DiffResult{
                .opened = true,
                .ide_used = ide_name,
                .tab_name = tab_name,
                .error = std::nullopt
            };
        }
        // Bridge failed, fall through to shell-out
    }

    // ── Attempt 2: Shell out to IDE CLI ───────────────────────
    auto ide = detail::detect_ide();
    auto diff_cmd = detail::get_diff_command_for_ide(ide);

    if (!diff_cmd) {
        return DiffResult{
            .opened = false,
            .ide_used = detail::ide_type_to_string(ide),
            .tab_name = tab_name,
            .error = std::string{"No supported IDE detected. Set VISUAL or EDITOR environment variable."}
        };
    }

    // Create temp files for original and modified content
    auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto temp_dir = fs::temp_directory_path() / "cc-repl-diffs";
    std::error_code ec;
    fs::create_directories(temp_dir, ec);
    if (ec) return std::unexpected("Failed to create diff temp directory: " + ec.message());

    auto original_path = temp_dir / ("original-" + nonce);
    auto modified_path = temp_dir / ("modified-" + nonce);
    {
        std::ofstream original(original_path, std::ios::binary);
        std::ofstream modified(modified_path, std::ios::binary);
        if (!original.is_open() || !modified.is_open()) {
            return std::unexpected("Failed to create temporary diff files");
        }
        original << request.original_content;
        modified << request.modified_content;
    }

    // Launch IDE diff command
    auto command = *diff_cmd + " " +
        cc::utils::bash::escape_shell_arg(original_path.string()) + " " +
        cc::utils::bash::escape_shell_arg(modified_path.string()) +
        " >/dev/null 2>&1 &";
    auto launched = cc::utils::bash::execute_command(command);
    if (!launched || launched->exit_code != 0) {
        return std::unexpected("Failed to launch IDE diff command");
    }

    return DiffResult{
        .opened = true,
        .ide_used = detail::ide_type_to_string(ide),
        .tab_name = tab_name,
        .error = std::nullopt
    };
}

/// Open a diff view for a specific line range in a file.
/// TS REF: src/hooks/useDiffInIDE.ts (showDiffInIDE with file_path, edits)
/// @param file_path Path to the file
/// @param start_line Starting line number (1-based)
/// @param end_line Ending line number (1-based, inclusive)
/// @return DiffResult indicating success/failure
[[nodiscard]] inline auto open_diff_in_ide_range(std::string_view file_path,
                                                  int start_line, int end_line)
    -> std::expected<DiffResult, std::string>
{
    if (file_path.empty()) {
        return std::unexpected(std::string{"file_path must not be empty"});
    }

    auto expanded = cc::utils::expand_tilde(file_path);
    std::string path_str = expanded.string();

    // Generate tab name
    auto sha = detail::generate_sha_nonce();
    auto tab_name = detail::generate_tab_name(file_path, sha);

    // Try bridge first
    if (bridge::has_bridge_ide()) {
        auto ide_name = bridge::get_bridge_ide_name().value_or("IDE");
        // Build openDiff with line range params
        std::string params = "{\"file_path\":\"" + path_str +
                           "\",\"start_line\":" + std::to_string(start_line) +
                           ",\"end_line\":" + std::to_string(end_line) +
                           ",\"tab_name\":\"" + tab_name + "\"}";

        auto rpc_result = bridge::call_ide_rpc("openFile", params);
        if (rpc_result) {
            return DiffResult{
                .opened = true,
                .ide_used = ide_name,
                .tab_name = tab_name,
                .error = std::nullopt
            };
        }
    }

    // Fallback: open file in IDE with line:column notation (VS Code style)
    auto ide = detail::detect_ide();
    std::string ide_name = detail::ide_type_to_string(ide);

    std::string open_cmd;
    switch (ide) {
        case IdeType::VSCode:
        case IdeType::Cursor:
            open_cmd = (ide == IdeType::Cursor ? "cursor" : "code") +
                       std::string{" --goto "} +
                       cc::utils::bash::escape_shell_arg(path_str + ":" +
                           std::to_string(start_line) + ":1");
            break;
        case IdeType::IntelliJ:
        case IdeType::CLion: {
            std::string cmd = (ide == IdeType::CLion) ? "clion" : "idea";
            open_cmd = cmd + " --line " + std::to_string(start_line) + " " +
                       cc::utils::bash::escape_shell_arg(path_str);
            break;
        }
        case IdeType::Neovim:
            open_cmd = "nvim +" + std::to_string(start_line) + " " +
                       cc::utils::bash::escape_shell_arg(path_str);
            break;
        case IdeType::Vim:
            open_cmd = "vim +" + std::to_string(start_line) + " " +
                       cc::utils::bash::escape_shell_arg(path_str);
            break;
        default:
            return DiffResult{
                .opened = false,
                .ide_used = ide_name,
                .tab_name = tab_name,
                .error = std::string{"IDE does not support line-range opening"}
            };
    }

    open_cmd += " >/dev/null 2>&1 &";
    auto launched = cc::utils::bash::execute_command(open_cmd);
    if (!launched || launched->exit_code != 0) {
        return std::unexpected("Failed to launch IDE open command");
    }

    return DiffResult{
        .opened = true,
        .ide_used = ide_name,
        .tab_name = tab_name,
        .error = std::nullopt
    };
}

// =========================================================================
// Utility functions
// =========================================================================

/// Check if IDE diff is available (an IDE is detected or bridge is connected).
/// TS REF: src/hooks/useDiffInIDE.ts:67-72 (shouldShowDiffInIDE)
[[nodiscard]] inline auto is_ide_diff_supported() -> bool {
    if (bridge::has_bridge_ide()) return true;
    auto ide = detail::detect_ide();
    return detail::get_diff_command_for_ide(ide).has_value();
}

/// Get the diff command for a specific IDE type (by name string).
[[nodiscard]] inline auto get_ide_diff_command(std::string_view ide_type) -> std::optional<std::string> {
    IdeType ide = IdeType::Unknown;
    if (ide_type == "vscode" || ide_type == "code") ide = IdeType::VSCode;
    else if (ide_type == "cursor") ide = IdeType::Cursor;
    else if (ide_type == "zed") ide = IdeType::Zed;
    else if (ide_type == "intellij" || ide_type == "idea") ide = IdeType::IntelliJ;
    else if (ide_type == "clion") ide = IdeType::CLion;
    else if (ide_type == "nvim" || ide_type == "neovim") ide = IdeType::Neovim;
    else if (ide_type == "vim") ide = IdeType::Vim;
    else if (ide_type == "emacs") ide = IdeType::Emacs;
    return detail::get_diff_command_for_ide(ide);
}

/// Get the currently detected IDE type.
[[nodiscard]] inline auto get_detected_ide() -> IdeType {
    return detail::detect_ide();
}

/// Get the name of the detected or bridge-connected IDE.
[[nodiscard]] inline auto get_ide_name() -> std::string {
    if (auto bridge_name = bridge::get_bridge_ide_name()) {
        return *bridge_name;
    }
    return detail::ide_type_to_string(detail::detect_ide());
}

/// Get a list of all supported IDE type names.
[[nodiscard]] inline auto get_supported_ides() -> std::vector<std::string> {
    return {"vscode", "cursor", "zed", "intellij", "clion", "neovim", "vim", "emacs"};
}

/// Close a diff tab in the IDE by tab name.
/// TS REF: src/hooks/useDiffInIDE.ts:329-344 (closeTabInIDE)
inline auto close_tab_in_ide(std::string_view tab_name) -> bool {
    if (bridge::has_bridge_ide()) {
        std::string params = "{\"tab_name\":\"" + std::string{tab_name} + "\"}";
        auto result = bridge::call_ide_rpc("close_tab", params);
        return result.has_value();
    }
    // Shell-out IDEs don't have tab-close API; return true (no-op success)
    return true;
}

/// Parse an IDE extension message from raw text.
/// TS REF: src/hooks/useDiffInIDE.ts:346-378 (isClosedMessage, isRejectedMessage, isSaveMessage)
[[nodiscard]] inline auto parse_ide_message(std::string_view text) -> IdeMessage {
    if (text == "TAB_CLOSED") {
        return IdeMessage{.type = IdeMessageType::TabClosed, .text = ""};
    }
    if (text == "DIFF_REJECTED") {
        return IdeMessage{.type = IdeMessageType::DiffRejected, .text = ""};
    }
    if (text.starts_with("FILE_SAVED")) {
        // Format: "FILE_SAVED\n<content>"
        auto newline = text.find('\n');
        std::string content;
        if (newline != std::string_view::npos) {
            content = std::string{text.substr(newline + 1)};
        }
        return IdeMessage{.type = IdeMessageType::FileSaved, .text = std::move(content)};
    }
    return IdeMessage{.type = IdeMessageType::Unknown, .text = std::string{text}};
}

} // namespace cc::hooks::diff_in_ide
