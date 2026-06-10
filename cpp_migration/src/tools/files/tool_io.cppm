/// @file tool_io.cppm
/// @brief Phase 3-S skeletons for the filesystem tool trio: Read / Write / Edit.
///
/// Real implementations in Phase 3 will:
///   * honour the `readonly_validation` / `path_validation` modules;
///   * stream reads larger than 1 MB with chunked callbacks;
///   * use `sed_edit_parser` for the edit semantics;
///   * surface permission prompts through the TUI.
/// The skeletons below never touch the filesystem; they echo back the
/// intended operation with enough detail that callers (repl_screen dispatch,
/// query_engine RunOnce, tests) can resolve and exercise types.
module;

#include <string>
#include <string_view>
#include <expected>

export module cc.tools.files;

import cc.tools.core;

export namespace cc::tools::files {

using namespace std::string_view_literals;

// --------------------------------------------------------------------------
// Helpers (skeleton-only)
// --------------------------------------------------------------------------

namespace detail {
    inline auto concat(std::string_view label, std::initializer_list<std::string_view> parts)
        -> std::string {
        std::string out;
        out.append(label);
        for (auto p : parts) {
            out.append(" ");
            out.append(p);
        }
        return out;
    }
} // namespace detail

// --------------------------------------------------------------------------
// FileReadTool
// --------------------------------------------------------------------------

class FileReadTool final : public core::ToolBase {
public:
    [[nodiscard]] auto name() const -> std::string_view override { return "FileRead"sv; }

    [[nodiscard]] auto requires_permission(const core::ToolInvocation& /*inv*/) const
        -> core::Permission override {
        // Real version inspects path against the readonly / allowlist set.
        return core::Permission::Granted;
    }

    [[nodiscard]] auto invoke(core::ToolInvocation inv, core::ToolContext ctx) const
        -> core::Expected<std::string> override {
        (void)ctx;
        auto banner = detail::concat("[file-read-dry-run] call_id="sv,
                                     {std::to_string(inv.call_id),
                                      inv.params.empty() ? "<no params>"sv : std::string_view(inv.params)});
        return core::ToolResult<std::string>::ok(std::move(banner));
    }

    /// Convenience wrapper: echoes path + offset + limit.
    [[nodiscard]] auto read(std::string_view path,
                            int offset = 0,
                            int limit = 0,
                            core::ToolContext ctx = {}) const
        -> core::ToolResult<std::string> {
        core::ToolInvocation inv;
        inv.name   = "FileRead";
        inv.params = detail::concat("path="sv,
                                    {path,
                                     "offset=" + std::to_string(offset),
                                     "limit="  + std::to_string(limit)});
        auto r = invoke(std::move(inv), std::move(ctx));
        return r ? *r : core::ToolResult<std::string>::err(r.error());
    }
};

// --------------------------------------------------------------------------
// FileWriteTool
// --------------------------------------------------------------------------

enum class WriteMode {
    Overwrite,
    Append,
};

class FileWriteTool final : public core::ToolBase {
public:
    [[nodiscard]] auto name() const -> std::string_view override { return "FileWrite"sv; }

    [[nodiscard]] auto aliases() const -> std::vector<std::string_view> override {
        return {"OverwriteFile"sv, "AppendFile"sv};
    }

    [[nodiscard]] auto requires_permission(const core::ToolInvocation& inv) const
        -> core::Permission override {
        // Heuristic: overwriting a non-trivial body always prompts for
        // review; the real version also checks `destructive_command_warning`.
        if (inv.params.size() > 4096) return core::Permission::RequirePrompt;
        return core::Permission::Granted;
    }

    [[nodiscard]] auto invoke(core::ToolInvocation inv, core::ToolContext ctx) const
        -> core::Expected<std::string> override {
        (void)ctx;
        auto banner = detail::concat("[file-write-dry-run] call_id="sv,
                                     {std::to_string(inv.call_id),
                                      inv.params.empty() ? "<no params>"sv : std::string_view(inv.params)});
        return core::ToolResult<std::string>::ok(std::move(banner));
    }

    [[nodiscard]] auto write(std::string_view path,
                             std::string_view content,
                             WriteMode mode = WriteMode::Overwrite,
                             core::ToolContext ctx = {}) const
        -> core::ToolResult<std::string> {
        core::ToolInvocation inv;
        inv.name   = "FileWrite";
        inv.params = detail::concat("path="sv,
                                    {path,
                                     mode == WriteMode::Overwrite ? "mode=overwrite"sv : "mode=append"sv,
                                     "content_len=" + std::to_string(content.size())});
        auto r = invoke(std::move(inv), std::move(ctx));
        return r ? *r : core::ToolResult<std::string>::err(r.error());
    }
};

// --------------------------------------------------------------------------
// FileEditTool
// --------------------------------------------------------------------------

class FileEditTool final : public core::ToolBase {
public:
    [[nodiscard]] auto name() const -> std::string_view override { return "FileEdit"sv; }

    [[nodiscard]] auto aliases() const -> std::vector<std::string_view> override {
        return {"EditFile"sv, "Replace"sv};
    }

    [[nodiscard]] auto requires_permission(const core::ToolInvocation& /*inv*/) const
        -> core::Permission override {
        // The real version always previews the diff in the TUI prompt.
        return core::Permission::RequirePrompt;
    }

    [[nodiscard]] auto invoke(core::ToolInvocation inv, core::ToolContext ctx) const
        -> core::Expected<std::string> override {
        (void)ctx;
        auto banner = detail::concat("[file-edit-dry-run] call_id="sv,
                                     {std::to_string(inv.call_id),
                                      inv.params.empty() ? "<no params>"sv : std::string_view(inv.params)});
        return core::ToolResult<std::string>::ok(std::move(banner));
    }

    [[nodiscard]] auto edit(std::string_view path,
                            std::string_view old_string,
                            std::string_view new_string,
                            bool replace_all = false,
                            core::ToolContext ctx = {}) const
        -> core::ToolResult<std::string> {
        core::ToolInvocation inv;
        inv.name   = "FileEdit";
        inv.params = detail::concat("path="sv,
                                    {path,
                                     "old_len=" + std::to_string(old_string.size()),
                                     "new_len=" + std::to_string(new_string.size()),
                                     replace_all ? "replace_all=true"sv : "replace_all=false"sv});
        auto r = invoke(std::move(inv), std::move(ctx));
        return r ? *r : core::ToolResult<std::string>::err(r.error());
    }
};

} // namespace cc::tools::files
