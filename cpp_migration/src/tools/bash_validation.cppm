// bash_validation.cppm
// Aggregate module for BashTool validation concerns.
//
// ORIGINAL NOTE (Agent 3 — migration):
//   This module originally contained empty stub implementations of
//   validate_mode / validate_paths / validate_read_only / is_destructive_command /
//   get_destructive_warning.  Those have been replaced by dedicated
//   per-concern modules (destructive_command_warning, mode_validation,
//   path_validation, readonly_validation, should_use_sandbox).  This file
//   re-exports everything from those modules under a single namespace so
//   existing callers don't break, and adds a lightweight
//   ValidationMode / ValidationResult / PathValidationContext envelope for
//   callers that want a single flat API.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>

export module cc.tools.bash_validation;

import cc.tools.destructive_command_warning;
import cc.tools.mode_validation;
import cc.tools.path_validation;
import cc.tools.readonly_validation;
import cc.tools.should_use_sandbox;

export namespace cc::tools::bash_validation {

// ── Re-exported types ──────────────────────────────────────────────────────

using PermissionMode     = mode_validation::PermissionMode;
using PermissionBehavior = mode_validation::PermissionBehavior;
using PermissionResult   = mode_validation::PermissionResult;
using DecisionReason     = mode_validation::DecisionReason;
using DecisionReasonType = mode_validation::DecisionReasonType;

using FileOperationType  = path_validation::FileOperationType;
using PathCommand        = path_validation::PathCommand;
using PathPermissionContext = path_validation::PathPermissionContext;
using PathPermissionResult  = path_validation::PathPermissionResult;
using PermissionUpdate      = path_validation::PermissionUpdate;
using PermissionUpdateType  = path_validation::PermissionUpdateType;

using SandboxInput          = sandbox::SandboxInput;
using SandboxRuntimeConfig  = sandbox::SandboxRuntimeConfig;

// ── Back-compat envelope types (original stub API preserved) ───────────────

enum class ValidationMode {
    Normal,
    ReadOnly,
    Restricted,
    Sandbox
};

struct EnvelopeResult {
    bool valid;
    std::optional<std::string> reason;
};

struct PathValidationContext {
    std::string working_dir;
    std::vector<std::string> allowed_paths;
    bool allow_absolute{true};
};

// ── Envelope implementations (delegate to the real logic) ──────────────────

/// Validate a command against a ValidationMode (aggregate wrapper).
inline EnvelopeResult validate_mode(std::string_view command,
                                     ValidationMode mode) {
    PermissionMode pm = PermissionMode::kDefault;
    switch (mode) {
        case ValidationMode::ReadOnly:
        case ValidationMode::Restricted: {
            auto rr = readonly_validation::check_read_only_constraints(command, false);
            if (rr.behavior == PermissionBehavior::kAllow) return {true, std::nullopt};
            if (rr.message) return {false, *rr.message};
            return {false, "command is not read-only"};
        }
        case ValidationMode::Sandbox: {
            bool sb = sandbox::should_use_sandbox(
                std::optional<std::string>(std::string(command)), false);
            if (!sb) return {true, "command excluded from sandbox"};
            return {true, std::nullopt};
        }
        case ValidationMode::Normal:
        default:
            // Mode-specific (acceptEdits) handling.
            auto mr = mode_validation::check_permission_mode(command, pm);
            return {mr.behavior != PermissionBehavior::kDeny, mr.message};
    }
}

/// Validate paths in a command string — wraps check_path_constraints.
inline EnvelopeResult validate_paths(std::string_view command,
                                     const PathValidationContext& ctx) {
    std::vector<std::filesystem::path> dirs;
    dirs.reserve(ctx.allowed_paths.size());
    for (auto& p : ctx.allowed_paths) dirs.emplace_back(p);
    path_validation::PathPermissionContext ppc{
        .cwd = ctx.working_dir.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(ctx.working_dir),
        .allowed_dirs = std::move(dirs),
    };
    auto r = path_validation::check_path_constraints(command, ppc, false);
    if (r.behavior == PermissionBehavior::kAsk ||
        r.behavior == PermissionBehavior::kDeny) {
        return {false, r.message};
    }
    return {true, std::nullopt};
}

/// Read-only wrapper.
inline EnvelopeResult validate_read_only(std::string_view command) {
    auto r = readonly_validation::check_read_only_constraints(command, false);
    if (r.behavior == PermissionBehavior::kAllow) return {true, std::nullopt};
    return {false, r.message};
}

// ── Destructive-command detection (delegates to destructive_command_warning
//    module which exports the same functions into cc::tools::bash_validation) ──

// ── sed validation (kept as a passthrough — TS version lives in separate
//    sedValidation.ts which Agent 3 is NOT migrating per task scope) ─────────

inline EnvelopeResult validate_sed_command(std::string_view sed_expr) {
    // Conservative: the read-only allowlist only accepts -n print-mode sed.
    if (sed_expr.find("-n") == std::string_view::npos &&
        sed_expr.find("--quiet") == std::string_view::npos &&
        sed_expr.find("--silent") == std::string_view::npos) {
        return {false, "non-readonly sed expressions require manual approval"};
    }
    return {true, std::nullopt};
}

} // namespace cc::tools::bash_validation
