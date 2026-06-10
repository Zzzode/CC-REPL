/// @file use_permission.cppm
/// @brief Hook-level permission request/handle types (Phase 3-S3 skeleton).
///
/// The `cc.hooks.tool_permissions` module defines rule evaluation and audit;
/// this module defines the UI-facing bridge types used by permission-hook
/// front-ends: `PermissionRequest` carries what the user sees,
/// `PermissionHandle` is the opaque callback ticket issued by the hook,
/// and `MakePermissionHandleStub` supplies a no-op handle for dry-run/tests.
module;

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

export module cc.hooks.use_permission;

import cc.hooks.tool_permissions;  // PermissionDecision

export namespace cc::hooks {

// ── Re-export for convenience of hook consumers ────────────────────────────
using ::cc::hooks::PermissionDecision;

/// Final outcome of a user-facing permission prompt.
/// Extends the rule-level `PermissionDecision` with UI-only lifecycle states.
enum class PermissionOutcome : std::uint8_t {
    pending,      /// Request has been enqueued but not resolved yet
    allowed,      /// User (or rule) allowed the request
    denied,       /// User (or rule) denied the request
    cancelled,    /// Prompt was dismissed without a decision (Esc)
    timed_out,    /// UI scheduler expired the prompt
};

/// Convert PermissionOutcome to a display string.
[[nodiscard]] constexpr std::string_view permission_outcome_name(
    PermissionOutcome o) noexcept {
    switch (o) {
        case PermissionOutcome::pending:   return "pending";
        case PermissionOutcome::allowed:   return "allowed";
        case PermissionOutcome::denied:    return "denied";
        case PermissionOutcome::cancelled: return "cancelled";
        case PermissionOutcome::timed_out: return "timed-out";
    }
    return "unknown";
}

// ────────────────────────────────────────────────────────────────────────────
// PermissionRequest
// ────────────────────────────────────────────────────────────────────────────

/// Everything a permission dialog needs to render a prompt.
///
/// Skeleton fields mirror the TypeScript `PermissionRequest` shape; follow-up
/// Phase 4 work can add structured `args` JSON, `preview` text blocks, and
/// `allow_*_buttons` configuration without breaking ABI.
struct PermissionRequest {
    /// Stable id used to correlate request/response across the UI queue.
    std::string  request_id;
    /// Tool name, e.g. "bash", "file_write", "mcp_call".
    std::string  tool_name;
    /// One-line human summary (shown in the dialog header).
    std::string  summary;
    /// Longer description / preview payload (shown in the body).
    std::string  description;
    /// Which permission modes are available to the UI (button set).
    std::vector<PermissionDecision> allowed_decisions = {
        PermissionDecision::allow_once,
        PermissionDecision::allow,
        PermissionDecision::deny,
    };
    /// Monotonic tick for UI ordering / dedup; defaults to 0 so that callers
    /// who don't care get a stable sort.
    std::uint64_t sequence = 0;
    /// If true, UI should not show "allow for session" option (one-shot ops).
    bool force_once = false;
    /// If true, the dialog is non-blocking and can be dismissed with Esc.
    bool cancellable = true;
};

// ────────────────────────────────────────────────────────────────────────────
// PermissionHandle
// ────────────────────────────────────────────────────────────────────────────

/// Callback signature invoked when the user or a rule resolves a request.
///   first arg: final decision
///   second arg: optional human-readable reason (shown in tool-call banner)
using PermissionResolveFn = std::function<void(
    PermissionDecision decision,
    std::optional<std::string> reason)>;

/// RAII-style ticket issued by `use_permission` hook implementations.
///
/// The handle owns the lifecycle of one enqueued prompt: destroying it
/// before `resolve()` is called triggers the `on_cancel` callback so the
/// hook queue never leaks.  The handle is move-only; copies are deleted.
class PermissionHandle {
public:
    PermissionHandle() = default;

    /// Construct a live handle bound to a request id + resolver.
    PermissionHandle(std::string request_id,
                     PermissionResolveFn resolver,
                     std::function<void()> on_cancel = nullptr)
        : request_id_(std::move(request_id)),
          resolver_(std::make_shared<PermissionResolveFn>(std::move(resolver))),
          on_cancel_(std::make_shared<std::function<void()>>(std::move(on_cancel))) {}

    // Move-only
    PermissionHandle(const PermissionHandle&)            = delete;
    auto operator=(const PermissionHandle&) -> PermissionHandle& = delete;
    PermissionHandle(PermissionHandle&&) noexcept            = default;
    auto operator=(PermissionHandle&&) noexcept -> PermissionHandle& = default;

    /// Trigger cancel-on-destroy if the request was never resolved.
    ~PermissionHandle() {
        if (resolver_ && !resolved_ && on_cancel_ && *on_cancel_) {
            try { (*on_cancel_)(); } catch (...) {}
        }
    }

    /// Resolve the underlying request.  Safe to call multiple times; only
    /// the first invocation takes effect.
    void resolve(PermissionDecision decision,
                 std::optional<std::string> reason = std::nullopt) {
        if (resolved_ || !resolver_) return;
        resolved_ = true;
        if (*resolver_) {
            try { (*resolver_)(decision, std::move(reason)); } catch (...) {}
        }
    }

    /// Convenience: resolve with decision=deny + reason="cancelled".
    void cancel(std::string_view reason = "user cancelled"sv) {
        resolve(PermissionDecision::deny, std::string(reason));
    }

    /// True once resolve() / cancel() has been called at least once.
    [[nodiscard]] auto is_resolved() const noexcept -> bool { return resolved_; }

    /// Id of the request this handle is bound to.
    [[nodiscard]] auto request_id() const noexcept -> const std::string& {
        return request_id_;
    }

private:
    std::string                                    request_id_;
    // Shared so the handle can be moved into closures without dangling.
    std::shared_ptr<PermissionResolveFn>           resolver_;
    std::shared_ptr<std::function<void()>>         on_cancel_;
    bool                                           resolved_ = false;
};

// ────────────────────────────────────────────────────────────────────────────
// MakePermissionHandleStub
// ────────────────────────────────────────────────────────────────────────────

/// Produce a `PermissionHandle` that:
///   * always resolves as `stub_decision` (default: allow_once) on resolve(),
///   * performs no I/O, emits no UI, logs nothing.
///
/// Used by `--dry-run`, unit tests, the `swarm_permission_poller` fallback
/// path, and any subsystem that needs a handle without wiring up the full
/// permission dialog stack.
[[nodiscard]] auto MakePermissionHandleStub(
    std::string request_id                        = "stub-request",
    PermissionDecision stub_decision              = PermissionDecision::allow_once,
    std::optional<std::string> stub_reason        = "permission-stub: no-op"
) -> PermissionHandle {
    auto resolver = [stub_decision, stub_reason](
                        PermissionDecision /*decision_ignored*/,
                        std::optional<std::string> /*reason_ignored*/) {
        // Stubs are pure; the caller-supplied decision is what the caller
        // expects to see, so we ignore arguments and honor the stub config.
        (void)stub_decision;
        (void)stub_reason;
    };
    return PermissionHandle(std::move(request_id), std::move(resolver),
                            /*on_cancel=*/nullptr);
}

}  // namespace cc::hooks
