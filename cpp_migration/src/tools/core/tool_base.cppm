/// @file tool_base.cppm
/// @brief Phase 3-S skeleton: common types + abstract base for every Tool.
///
/// This module is intentionally dependency-free beyond the STL so the
/// tool-call loop in `cc.services.query_engine` can resolve types without
/// pulling in yyjson / uv_a / real filesystem headers during the S-stage.
/// Concrete tools eventually import real file I/O and libcurl; that work
/// lands in a follow-up Phase 3 pass.
module;

#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <expected>
#include <cstdint>

export module cc.tools.core;

export namespace cc::tools::core {

// --------------------------------------------------------------------------
// Permission model
// --------------------------------------------------------------------------

/// Grant decision produced by `ToolBase::requires_permission()`.
enum class Permission {
    Granted,        // no prompt needed (already approved / allowlisted)
    Denied,         // hard-block (never auto-approved)
    RequirePrompt,  // ask the user before dispatching
    Sandboxed,      // auto-run, but inside an isolated sandbox
};

// --------------------------------------------------------------------------
// Result model
// --------------------------------------------------------------------------

/// Outcome of any Tool invocation.  Mirrors TS `ToolResult` status bits.
enum class ToolResultStatus {
    Ok,
    Error,
    PermissionDenied,
    Cancelled,
    Timeout,
};

/// Generic typed result.  Phase 3 moves to a variant/monadic abstraction;
/// for the skeleton we keep it value-based.
template <typename T = std::string>
struct ToolResult {
    T                  value;
    ToolResultStatus   status = ToolResultStatus::Ok;
    std::string        error;   // populated when status != Ok

    [[nodiscard]] static auto ok(T v)      -> ToolResult { return {std::move(v), ToolResultStatus::Ok, {}}; }
    [[nodiscard]] static auto err(std::string_view msg,
                                  ToolResultStatus s = ToolResultStatus::Error)
        -> ToolResult { return {{}, s, std::string(msg)}; }
};

/// `Expected` alias we will standardise on after migration.
template <typename T>
using Expected = std::expected<ToolResult<T>, std::string>;

// --------------------------------------------------------------------------
// Invocation + context (S-stage minimal shapes)
// --------------------------------------------------------------------------

/// Simplified invocation descriptor.  In Phase 3 the `params` string will be
/// replaced with a strongly-typed parse tree over a JSON DOM (yyjson / simdjson).
struct ToolInvocation {
    std::string   name;
    std::string   params;       // skeleton: raw JSON-like string
    std::uint64_t call_id = 0;
};

/// Transient per-call context.  Real version carries user token, sandbox id,
/// working directory, permission session, etc.
struct ToolContext {
    bool          dry_run      = false;
    std::uint64_t session_id   = 0;
    std::string   cwd;
};

// --------------------------------------------------------------------------
// Abstract Tool base
// --------------------------------------------------------------------------

class ToolBase {
public:
    virtual ~ToolBase() = default;

    /// Canonical / registered name.  Must be unique within a registry.
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// Alternative names that can dispatch to the same implementation.
    [[nodiscard]] virtual auto aliases() const -> std::vector<std::string_view> { return {}; }

    /// Fast permission pre-check, runs BEFORE user prompt / sandbox setup.
    [[nodiscard]] virtual auto requires_permission(const ToolInvocation& inv) const
        -> Permission = 0;

    /// Skeleton entry point.  Concrete tools override this.
    /// Phase 3 refactors the signature to return `Expected<StreamHandle>` to
    /// support incremental delta emission.
    [[nodiscard]] virtual auto invoke(ToolInvocation inv, ToolContext ctx) const
        -> Expected<std::string> = 0;
};

} // namespace cc::tools::core
