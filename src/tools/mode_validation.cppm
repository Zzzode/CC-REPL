// mode_validation.cppm
// Checks if commands should be handled differently based on the current
// permission mode (acceptEdits, bypassPermissions, dontAsk, etc.).
// Ported from src/tools/BashTool/modeValidation.ts

module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <algorithm>
#include <format>
#include <utility>

export module cc.tools.mode_validation;

import cc.tools.bash_security;

export namespace cc::tools::mode_validation {

// ---------------------------------------------------------------------------
// ToolPermissionContext (sufficient subset for mode-based checks)
//
// TS version pulls ToolPermissionContext from src/Tool.js which contains many
// fields unrelated to mode validation. We define only the fields we actually
// use: .mode and the helpers required to express PermissionResult.
// ---------------------------------------------------------------------------

/// Permission mode enum — values match the TS string union keys so callers can
/// map one-to-one.  Unknown / future values map to kDefault.
enum class PermissionMode {
    kDefault,             // Normal interactive prompts (no special mode)
    kAcceptEdits,         // 'acceptEdits' — auto-allow filesystem operations
    kBypassPermissions,   // 'bypassPermissions' — handled by main permission flow
    kDontAsk,             // 'dontAsk' — handled by main permission flow
};

/// Behaviour enum for permission results — matches TS PermissionResult::behavior.
enum class PermissionBehavior {
    kPassthrough,  // No mode-specific handling; defer to other checks
    kAllow,        // Mode permits auto-approval
    kAsk,          // Command needs approval in this mode
    kDeny,         // Mode explicitly rejects
};

/// The 'decision reason' attached to allow/ask/deny results so the permission
/// UI can explain *why* a decision was reached.
enum class DecisionReasonType {
    kMode,       // Decision came from permission mode (e.g. acceptEdits)
    kRule,       // Decision came from a user-defined allow/deny rule
    kSafety,     // Decision came from a safety check
    kOther,      // Catch-all
};

struct DecisionReason {
    DecisionReasonType type{DecisionReasonType::kOther};
    std::optional<PermissionMode> mode;     // set when type == kMode
    std::string reason;                      // human-readable when type == kOther
};

/// Result of a mode validation check.
struct PermissionResult {
    PermissionBehavior behavior{PermissionBehavior::kPassthrough};
    std::optional<std::string> message;
    std::optional<std::string> updated_command;   // for 'allow' with updated input
    std::optional<DecisionReason> decision_reason;
};

// ---------------------------------------------------------------------------
// Filesystem command allowlist for Accept Edits mode
//
// These are the commands that are auto-allowed when running in acceptEdits
// mode.  Corresponds to ACCEPT_EDITS_ALLOWED_COMMANDS in the TS source.
// ---------------------------------------------------------------------------

inline constexpr std::array<std::string_view, 7> kAcceptEditsAllowedCommands = {
    "mkdir", "touch", "rm", "rmdir", "mv", "cp", "sed"
};

/// Returns true if `command` is one of the filesystem operations allowed by
/// Accept Edits mode.
[[nodiscard]] inline bool is_filesystem_command(std::string_view command) {
    return std::ranges::find(kAcceptEditsAllowedCommands, command) !=
           kAcceptEditsAllowedCommands.end();
}

/// Splits a command string into its constituent subcommands.
///
/// NOTE: the TS source uses splitCommand_DEPRECATED from utils/bash/commands.js
/// which handles &&/||/;/|/| bash operators.  For the *mode validation* path
/// we do NOT need a full shell parser — we just need to extract the "base
/// command" (first whitespace-delimited token) of each subcommand.  We split
/// on the standard operator characters and trim whitespace.  This is the same
/// precision the TS achieve when they later call `.split(/\s+/)` to pull out
/// baseCmd.
[[nodiscard]] inline std::vector<std::string> split_subcommands(std::string_view input) {
    std::vector<std::string> result;
    std::string current;

    auto flush = [&] {
        // trim leading / trailing whitespace
        auto b = current.find_first_not_of(" \t\n\r");
        if (b == std::string::npos) { current.clear(); return; }
        auto e = current.find_last_not_of(" \t\n\r");
        std::string trimmed = current.substr(b, e - b + 1);
        if (!trimmed.empty()) result.push_back(std::move(trimmed));
        current.clear();
    };

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        // Handle multi-character operators: &&  ||  |&
        if (c == '&' && i + 1 < input.size() && input[i + 1] == '&') { flush(); ++i; continue; }
        if (c == '|' && i + 1 < input.size() && input[i + 1] == '|') { flush(); ++i; continue; }
        if (c == '|' || c == ';' || c == '\n') { flush(); continue; }
        current.push_back(c);
    }
    flush();
    return result;
}

/// Extracts the base command (first whitespace token) from a trimmed subcommand.
/// Returns empty string_view if the command is blank.
[[nodiscard]] inline std::string_view extract_base_command(std::string_view trimmed_cmd) {
    auto pos = trimmed_cmd.find_first_of(" \t\n\r");
    if (pos == std::string_view::npos) return trimmed_cmd;
    return trimmed_cmd.substr(0, pos);
}

/// Performs mode-specific validation for a single (already-trimmed) subcommand.
///
/// @param cmd                   Full subcommand string (before stripping env
///                              vars or wrappers — the TS path only looks at
///                              the first whitespace token which is fine for
///                              the filesystem allowlist).
/// @param mode                  Current permission mode.
[[nodiscard]] inline PermissionResult
validate_command_for_mode(std::string_view cmd, PermissionMode mode) {
    auto trimmed = cmd;
    // trim leading whitespace
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    if (trimmed.empty()) {
        PermissionResult result;
        result.behavior = PermissionBehavior::kPassthrough;
        result.message = "Base command not found";
        return result;
    }
    auto base_cmd = extract_base_command(trimmed);
    if (base_cmd.empty()) {
        PermissionResult result;
        result.behavior = PermissionBehavior::kPassthrough;
        result.message = "Base command not found";
        return result;
    }

    // In Accept Edits mode, auto-allow filesystem operations
    if (mode == PermissionMode::kAcceptEdits &&
        is_filesystem_command(base_cmd)) {
        PermissionResult result;
        result.behavior = PermissionBehavior::kAllow;
        result.updated_command = std::string(cmd);
        result.decision_reason = DecisionReason{
            .type = DecisionReasonType::kMode,
            .mode = PermissionMode::kAcceptEdits,
            .reason = {},
        };
        return result;
    }

    PermissionResult result;
    result.behavior = PermissionBehavior::kPassthrough;
    result.message = std::format(
        "No mode-specific handling for '{}' in mode {}",
        std::string(base_cmd),
        static_cast<int>(mode));
    return result;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// Main entry point for mode-based permission logic.
///
/// Currently handles Accept Edits mode for filesystem commands, but designed
/// to be extended for other modes.
///
/// @param full_command          The raw command as entered (may contain &&,
///                              pipes, etc.).
/// @param mode                  Current permission mode from ToolPermissionContext.
///
/// @returns
///   - kAllow        if the current mode permits auto-approval
///   - kAsk          (reserved; no current path produces this)
///   - kPassthrough  if no mode-specific handling applies
[[nodiscard]] inline PermissionResult
check_permission_mode(std::string_view full_command, PermissionMode mode) {
    // Bypass mode is handled elsewhere in the main permission flow.
    if (mode == PermissionMode::kBypassPermissions) {
        PermissionResult result;
        result.behavior = PermissionBehavior::kPassthrough;
        result.message = "Bypass mode is handled in main permission flow";
        return result;
    }

    // DontAsk mode is handled elsewhere in the main permission flow.
    if (mode == PermissionMode::kDontAsk) {
        PermissionResult result;
        result.behavior = PermissionBehavior::kPassthrough;
        result.message = "DontAsk mode is handled in main permission flow";
        return result;
    }

    auto subcommands = split_subcommands(full_command);

    // Check each subcommand — if ANY triggers mode-specific behaviour, return
    // that result immediately (conservative short-circuit).
    for (const auto& sub : subcommands) {
        auto r = validate_command_for_mode(sub, mode);
        if (r.behavior != PermissionBehavior::kPassthrough) {
            return r;
        }
    }

    PermissionResult result;
    result.behavior = PermissionBehavior::kPassthrough;
    result.message = "No mode-specific validation required";
    return result;
}

/// Returns the list of commands auto-allowed for a given permission mode.
/// Used by the permission UI to inform the user which operations are
/// implicitly approved.
[[nodiscard]] inline std::vector<std::string_view>
get_auto_allowed_commands(PermissionMode mode) {
    if (mode == PermissionMode::kAcceptEdits) {
        return std::vector<std::string_view>(
            kAcceptEditsAllowedCommands.begin(),
            kAcceptEditsAllowedCommands.end());
    }
    return {};
}

// ---------------------------------------------------------------------------
// TS-compatible aliases (for call sites still using TS-style camelCase)
// ---------------------------------------------------------------------------

[[nodiscard]] inline PermissionResult
checkPermissionMode(std::string_view c, PermissionMode m) {
    return check_permission_mode(c, m);
}

[[nodiscard]] inline std::vector<std::string_view>
getAutoAllowedCommands(PermissionMode m) {
    return get_auto_allowed_commands(m);
}

} // namespace cc::tools::mode_validation
