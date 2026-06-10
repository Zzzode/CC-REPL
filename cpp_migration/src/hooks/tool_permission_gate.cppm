/// @file tool_permission_gate.cppm
/// @brief Phase 3-G PermissionGate: Resolver + interactive prompt wrapper.
///
/// The resolver is UI-agnostic; the gate wires the resolver up to the
/// user-visible permission prompt so that, when a request cannot be
/// auto-resolved, the UI hook runs (if registered) and any Always*
/// decision it returns is persisted back into the resolver cache.
///
/// If no interactive prompt is installed the gate falls back to a safe
/// default: AllowOnce for low-risk sandboxed calls and Deny otherwise.
module;

#include <functional>
#include <string>
#include <string_view>
#include <utility>

import cc.hooks.permission_resolver;

export module cc.hooks.tool_permission_gate;

export namespace cc::hooks::permission {

// ────────────────────────────────────────────────────────────────────────────
// Re-exports so downstream consumers get the resolver types with one import.
// ────────────────────────────────────────────────────────────────────────────
using ::cc::hooks::permission::Decision;
using ::cc::hooks::permission::ActionKind;
using ::cc::hooks::permission::RiskLevel;
using ::cc::hooks::permission::PermissionRequest;
using ::cc::hooks::permission::PermissionResolver;

/// Callback signature for the interactive prompt hook.
/// When the resolver cannot auto-approve, the gate invokes this callback
/// (if non-null) with the original request and uses its return value as
/// the decision.  Returning AlwaysAllow / AlwaysDeny here persists the
/// choice into the resolver cache keyed by (tool, first affected_path or
/// "*") so future identical calls skip the prompt.
using InteractivePromptFn = std::function<Decision(const PermissionRequest&)>;

// ────────────────────────────────────────────────────────────────────────────
// PermissionGate
// ────────────────────────────────────────────────────────────────────────────

class PermissionGate {
public:
    /// Underlying resolver — exposed so callers can prepopulate caches,
    /// serialize to JSON, or query cache_size() in tests.
    PermissionResolver   resolver;
    /// User-visible prompt hook.  May be left empty for safe-default mode.
    InteractivePromptFn  interactive_prompt;

    /// Run the full gate pipeline: resolver → (optional) prompt → cache
    /// update.  Returns `true` if the caller should PROCEED with the tool
    /// execution, `false` if it should be denied / aborted.
    [[nodiscard]] bool AllowToolExecute(const PermissionRequest& req) {
        const Decision d = ResolveWithPrompt(req);
        // Treat Abort as deny (and signal the caller should tear down
        // the whole enclosing query rather than just skip this call).
        if (d == Decision::Abort) return false;
        if (d == Decision::Deny || d == Decision::AlwaysDeny) return false;
        // AllowOnce / AlwaysAllow both permit this call.
        return true;
    }

    /// Helper: returns the *actual* decision produced by the pipeline,
    /// not just a bool.  Useful for audit logging.
    [[nodiscard]] Decision ResolveWithPrompt(const PermissionRequest& req) {
        // Step 1: consult the resolver.  Any non-Deny terminal result
        // short-circuits (resolver owns the always-* cache lookups).
        const Decision base = resolver.Resolve(req);
        if (base != Decision::Deny) return base;

        // Step 2: prompt the user if a hook is installed.
        if (interactive_prompt) {
            Decision user;
            try {
                user = interactive_prompt(req);
            } catch (...) {
                // Broken UI hook: fail closed.
                return Decision::Deny;
            }
            // Persist any Always* choice into the resolver, keyed by the
            // concrete (tool_name, first path-or-"*").
            if (user == Decision::AlwaysAllow || user == Decision::AlwaysDeny) {
                const std::string& path_key =
                    req.affected_paths.empty()
                        ? empty_path_star()
                        : req.affected_paths.front();
                resolver.CacheAlways(std::string(req.tool_name),
                                     path_key,
                                     user);
            }
            return user;
        }

        // Step 3: no prompt hook — safe default.  Re-check the risk+sbox
        // condition because resolver.Resolve() already handled this case
        // and returned AllowOnce above; we only land here when risk>=Medium
        // and/or !sandboxed, so deny.
        return Decision::Deny;
    }

private:
    /// Canonical "all paths" glob used for requests with no concrete
    /// affected paths.  Returned by ref so CacheAlways doesn't copy a
    /// temporary every call.
    [[nodiscard]] static const std::string& empty_path_star() {
        static const std::string kStar = "*";
        return kStar;
    }
};

}  // namespace cc::hooks::permission
