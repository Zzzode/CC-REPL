/// @file tool_bash.cppm
/// @brief Phase 3-S skeleton for the `Bash` / shell-execution tool.
///
/// The real implementation in Phase 3 will sandbox commands via the existing
/// `tools/bash_security.cppm` helpers, wire up sandboxed PTY spawning through
/// libuv, honour `bash_permissions` allow/deny rules, and stream line-by-line
/// output back to the engine.  This stub simply echoes its inputs so the
/// calling surfaces can be exercised in unit tests without side effects.
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>

export module cc.tools.bash_skel;

import cc.tools.core;

export namespace cc::tools::bash {

using namespace std::string_view_literals;

/// Invocation options.  These match the top-level Bash tool JSON-schema shape
/// in the TS `BashTool` so Phase 3 can drop in a real executor without
/// changing the option struct layout.
struct BashOptions {
    std::string  command;
    int          timeout_sec   = 30;
    bool         sandboxed     = true;
    bool         allow_fail    = false;
    std::string  description;   // plain-text intent, shown in the TUI
};

/// Serialise BashOptions to the skeleton "params" field.  Phase 3 replaces
/// this with a real JSON encoder (yyjson).
[[nodiscard]] inline auto encode_params(const BashOptions& o) -> std::string {
    using namespace std::string_literals;
    auto b = "BashOptions{cmd=\""s;
    b.append(o.command);
    b.append("\" timeout="s);
    b.append(std::to_string(o.timeout_sec));
    b.append(o.sandboxed ? " sandboxed"sv : ""sv);
    b.append(o.allow_fail ? " allow_fail"sv : ""sv);
    b.push_back('}');
    return b;
}

// --------------------------------------------------------------------------
// Tool
// --------------------------------------------------------------------------

class BashTool final : public core::ToolBase {
public:
    [[nodiscard]] auto name() const -> std::string_view override { return "Bash"sv; }

    [[nodiscard]] auto aliases() const -> std::vector<std::string_view> override {
        return {"Shell"sv, "Exec"sv};
    }

    /// Phase 3 delegates to `bash_permissions` + the sandboxed PTY runner;
    /// for now we conservatively require a user prompt unless dry_run is on.
    [[nodiscard]] auto requires_permission(const core::ToolInvocation& inv) const
        -> core::Permission override {
        // Heuristic: destructive-looking tokens always prompt.
        constexpr std::string_view danger_tokens[] = {"rm "sv, "mkfs"sv, "dd "sv, ":(){:"sv};
        for (auto tok : danger_tokens) {
            if (inv.params.find(tok) != std::string::npos)
                return core::Permission::RequirePrompt;
        }
        return core::Permission::Sandboxed;
    }

    /// Skeleton implementation: never execs.  Returns the command inside a
    /// readable banner prefixed with `[bash-dry-run]`.
    [[nodiscard]] auto invoke(core::ToolInvocation inv, core::ToolContext ctx) const
        -> core::Expected<std::string> override {
        using namespace std::string_literals;
        std::string out = "[bash-dry-run] cwd="s;
        out.append(ctx.cwd.empty() ? "<cwd-unset>"s : ctx.cwd);
        out.append(" call_id="s);
        out.append(std::to_string(inv.call_id));
        out.push_back('\n');
        out.append(inv.params.empty() ? "<no params>"s : inv.params);
        out.push_back('\n');
        return core::ToolResult<std::string>::ok(std::move(out));
    }

    // Type-safe convenience entry (Phase 3 stub).
    [[nodiscard]] auto run(BashOptions o, core::ToolContext ctx = {}) const
        -> core::ToolResult<std::string> {
        core::ToolInvocation inv;
        inv.name    = "Bash";
        inv.params  = encode_params(o);
        inv.call_id = 0;
        auto res = invoke(std::move(inv), std::move(ctx));
        if (!res) return core::ToolResult<std::string>::err(res.error());
        return *res;
    }
};

} // namespace cc::tools::bash
