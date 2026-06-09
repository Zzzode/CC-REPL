/// @file permissions_cmd.cppm
/// @brief PermissionsCommand implementing the /permissions slash command.
/// 100% delegates to cc.utils.permissions_engine for all rule management.
/// Sub-commands: list, show TOOL, allow TOOL [SCOPE], deny TOOL [SCOPE],
///               reset TOOL, reset-all, dump
/// Output is plain-text rows ready for Phase 4 FTXUI table rendering.
module;

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.commands.permissions_cmd;

import cc.types.types;
import cc.commands.command;
import cc.utils.permissions_engine;

export namespace cc::commands {

using namespace cc::core;
namespace perm = cc::utils::permissions;

// ============================================================
// Formatting helpers (pure)
// ============================================================

/// Human-readable scope.
[[nodiscard]] inline constexpr std::string_view scope_str(perm::PermissionScope s) noexcept {
    switch (s) {
        case perm::PermissionScope::Global:  return "global";
        case perm::PermissionScope::Project: return "project";
        case perm::PermissionScope::Session: return "session";
        case perm::PermissionScope::Command: return "command";
    }
    return "?";
}

/// Human-readable action.
[[nodiscard]] inline constexpr std::string_view action_str(perm::PermissionAction a) noexcept {
    switch (a) {
        case perm::PermissionAction::Allow:    return "ALLOW";
        case perm::PermissionAction::Deny:     return "DENY";
        case perm::PermissionAction::Ask:      return "ASK";
        case perm::PermissionAction::AskOnce:  return "ASK-ONCE";
    }
    return "?";
}

/// Human-readable match strategy.
[[nodiscard]] inline constexpr std::string_view strategy_str(perm::MatchStrategy s) noexcept {
    switch (s) {
        case perm::MatchStrategy::Exact:  return "exact";
        case perm::MatchStrategy::Prefix: return "prefix";
        case perm::MatchStrategy::Glob:   return "glob";
        case perm::MatchStrategy::Regex:  return "regex";
    }
    return "?";
}

/// Parse a scope string. Returns nullopt on unknown.
[[nodiscard]] inline std::optional<perm::PermissionScope> parse_scope(std::string_view s) {
    if (s == "global"  || s == "g") return perm::PermissionScope::Global;
    if (s == "project" || s == "p") return perm::PermissionScope::Project;
    if (s == "session" || s == "s") return perm::PermissionScope::Session;
    if (s == "command" || s == "c") return perm::PermissionScope::Command;
    return std::nullopt;
}

/// Format a single PermissionRule as a human-readable row.
/// Phase 4 can re-parse these rows into an FTXUI table.
[[nodiscard]] inline std::string format_rule_row(const perm::PermissionRule& rule) {
    std::string row;
    row += std::format("{:<18} ", rule.id.empty() ? "-" : rule.id);
    row += std::format("{:<8} ", action_str(rule.action));
    row += std::format("{:<8} ", scope_str(rule.scope));
    row += std::format("{:<6} ", strategy_str(rule.strategy));
    row += std::format("{:<3} ", rule.priority);
    row += std::format("{:<24} ", rule.tool_pattern);
    if (rule.path_pattern.has_value() && !rule.path_pattern->empty()) {
        row += std::format(" path=\"{}\"", *rule.path_pattern);
    }
    return row;
}

/// Column header matching format_rule_row().
[[nodiscard]] inline std::string rules_table_header() {
    std::string h;
    h += std::format("{:<18} ", "ID");
    h += std::format("{:<8} ", "ACTION");
    h += std::format("{:<8} ", "SCOPE");
    h += std::format("{:<6} ", "MATCH");
    h += std::format("{:<3} ", "PRI");
    h += std::format("{:<24} ", "TOOL_PATTERN");
    h += " PATH";
    return h;
}

/// Render a list of rules as a nicely formatted block (header + rows).
[[nodiscard]] inline std::string format_rules_list(
    std::span<const perm::PermissionRule> rules) {
    if (rules.empty()) {
        return "(no permission rules configured)";
    }
    std::string out = rules_table_header();
    out.push_back('\n');
    out += std::string(rules_table_header().size(), '-');
    out.push_back('\n');
    for (const auto& r : rules) {
        out += format_rule_row(r);
        out.push_back('\n');
    }
    out += std::format("\nTotal: {} rule(s)", rules.size());
    return out;
}

/// Generate a short unique rule id from tool pattern + scope + random suffix.
[[nodiscard]] inline std::string make_rule_id(std::string_view tool,
                                              perm::PermissionScope scope,
                                              perm::PermissionAction action) {
    // Shorten long tool names; scope prefix; action first letter.
    std::string base(tool);
    if (base.size() > 16) base.resize(16);
    // sanitize: replace non-alnum with underscore
    for (auto& c : base) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '*') {
            c = '_';
        }
    }
    auto now = std::chrono::system_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();
    std::ostringstream oss;
    oss << std::string(action_str(action).substr(0, 1))
        << std::string(scope_str(scope).substr(0, 1)) << "-"
        << base << "-" << std::hex << (dur & 0xFFFF);
    return oss.str();
}

// ============================================================
// PermissionsCommand
// ============================================================

/// PermissionsCommand implements the /permissions slash command.
/// All state is held in cc::utils::permissions_engine (global singleton) —
/// this class is a thin command-layer dispatch that formats rows suitable
/// for both plain-text output and Phase 4 FTXUI tables.
class PermissionsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "permissions",
            .description = "Manage tool execution permission rules",
            .aliases = {"perms"},
            .args = {
                CommandArg{.name = "action", .description =
                    "list | show | allow | deny | reset | reset-all | dump",
                    .type = ArgType::Choice, .required = false,
                    .choices = {"list", "show", "allow", "deny",
                                "reset", "reset-all", "dump"}},
                CommandArg{.name = "tool", .description = "Tool name or pattern",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "scope", .description =
                    "global|project|session|command (default: session)",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"global", "project", "session", "command",
                                       "g", "p", "s", "c"}},
            },
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        const auto& action = ctx.args[0];
        static constexpr std::array valid = {
            "list", "show", "allow", "deny", "reset", "reset-all", "dump"
        };
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format(
                    "Invalid action '{}'.\n"
                    "Usage: /permissions <list|show|allow|deny|reset|reset-all|dump> ...",
                    action)));
        }
        if ((action == "show" || action == "allow" || action == "deny" || action == "reset")
            && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format(
                    "Action '{}' requires a tool name/pattern argument.",
                    action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_status_summary());

        const auto& action = ctx.args[0];
        if (action == "list") {
            return CommandResult::success(cmd_list(ctx));
        }
        if (action == "show") {
            return CommandResult::success(cmd_show(ctx.args[1]));
        }
        if (action == "allow") {
            return cmd_allow(ctx);
        }
        if (action == "deny") {
            return cmd_deny(ctx);
        }
        if (action == "reset") {
            return cmd_reset(ctx.args[1]);
        }
        if (action == "reset-all") {
            return cmd_reset_all();
        }
        if (action == "dump") {
            return CommandResult::success(perm::export_rules());
        }
        return CommandResult::success(format_status_summary());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> out;
        for (auto s : {"list", "show", "allow", "deny", "reset", "reset-all",
                       "dump", "global", "project", "session", "command"}) {
            if (std::string_view(s).starts_with(partial)) {
                out.emplace_back(s);
            }
        }
        return out;
    }

private:
    // ---- subcommand implementations ----

    /// list [scope]
    [[nodiscard]] std::string cmd_list(const CommandContext& ctx) {
        std::optional<perm::PermissionScope> scope = std::nullopt;
        if (ctx.args.size() >= 2) {
            if (auto s = parse_scope(ctx.args[1])) scope = *s;
        }
        auto rules = perm::get_rules(scope);
        std::string title;
        if (scope.has_value()) {
            title = std::format("Permission rules (scope={}):\n\n",
                                 scope_str(*scope));
        } else {
            title = "All permission rules:\n\n";
        }
        return title + format_rules_list(std::span{rules});
    }

    /// show TOOL
    [[nodiscard]] std::string cmd_show(std::string_view tool) {
        auto effective = perm::get_effective_rules(tool);

        std::string out = std::format("Effective rules for tool '{}':\n\n", tool);
        if (effective.empty()) {
            out += "(no rules match — default policy: DENY, ask the user)";
            return out;
        }
        out += format_rules_list(std::span{effective});

        // Also show what a query would evaluate to
        perm::PermissionQuery query{
            .tool_name = std::string(tool),
            .operation = "execute",
            .paths = {},
            .command = std::nullopt,
            .working_directory = {},
        };
        auto res = perm::engine().evaluate(query);
        out += std::format(
            "\n\nEvaluated action for {}/execute: {} ({})",
            tool, action_str(res.action), res.explanation);
        return out;
    }

    /// allow TOOL [SCOPE]
    [[nodiscard]] Result<CommandResult> cmd_allow(const CommandContext& ctx) {
        return add_rule_cmd(ctx, perm::PermissionAction::Allow);
    }

    /// deny TOOL [SCOPE]
    [[nodiscard]] Result<CommandResult> cmd_deny(const CommandContext& ctx) {
        return add_rule_cmd(ctx, perm::PermissionAction::Deny);
    }

    /// reset TOOL — remove all rules whose tool_pattern matches the given tool
    [[nodiscard]] Result<CommandResult> cmd_reset(std::string_view tool) {
        auto all = perm::get_rules();
        std::size_t removed = 0;
        for (const auto& r : all) {
            if (perm::match_path_pattern(tool, r.tool_pattern, r.strategy) ||
                r.tool_pattern == tool) {
                if (perm::remove_rule(r.id)) ++removed;
            }
        }
        // Also do exact id matching for tool patterns that are not wildcards
        // (handled above already by exact match)
        if (removed == 0) {
            return CommandResult::success(std::format(
                "No rules found for tool '{}'.", tool));
        }
        return CommandResult::success(std::format(
            "Removed {} rule(s) matching '{}'.", removed, tool));
    }

    /// reset-all — clear every rule (all scopes)
    [[nodiscard]] Result<CommandResult> cmd_reset_all() {
        auto all = perm::get_rules();
        std::size_t total = all.size();
        std::size_t removed = 0;
        for (const auto& r : all) {
            if (perm::remove_rule(r.id)) ++removed;
        }
        // Also clear session rules just in case
        perm::clear_session_rules();
        perm::invalidate_cache();
        if (removed == 0) {
            return CommandResult::success("No permission rules to reset.");
        }
        return CommandResult::success(std::format(
            "Reset all permission rules ({} removed, {} skipped).",
            removed, total - removed));
    }

    // ---- shared helpers ----

    [[nodiscard]] Result<CommandResult> add_rule_cmd(
        const CommandContext& ctx, perm::PermissionAction action) {
        const std::string& tool = ctx.args[1];
        perm::PermissionScope scope = perm::PermissionScope::Session; // default
        if (ctx.args.size() >= 3) {
            if (auto s = parse_scope(ctx.args[2])) {
                scope = *s;
            } else {
                return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                    std::format("Unknown scope '{}'. "
                                "Valid: global | project | session | command",
                                ctx.args[2])));
            }
        }
        perm::PermissionRule rule;
        rule.id = make_rule_id(tool, scope, action);
        rule.tool_pattern = tool;
        rule.action = action;
        rule.scope = scope;
        rule.strategy = contains_wildcard(tool)
                            ? perm::MatchStrategy::Glob
                            : perm::MatchStrategy::Exact;
        rule.priority = default_priority_for(action);
        rule.created_at = std::chrono::system_clock::now();

        const std::string rule_id = rule.id; // capture before move
        auto result = perm::add_rule(std::move(rule));
        if (!result) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Failed to add rule: {}", result.error())));
        }
        perm::invalidate_cache();

        return CommandResult::success(std::format(
            "Added rule: {} tool='{}' scope={} id={}",
            action_str(action), tool, scope_str(scope), rule_id));
    }

    /// Check whether a tool pattern contains glob metacharacters.
    [[nodiscard]] static bool contains_wildcard(std::string_view s) {
        return s.find_first_of("*?[]") != std::string_view::npos;
    }

    /// Sensible default priority: higher for deny rules so they win over allows.
    [[nodiscard]] static int default_priority_for(perm::PermissionAction a) {
        switch (a) {
            case perm::PermissionAction::Deny:     return 100;
            case perm::PermissionAction::Allow:    return 50;
            case perm::PermissionAction::Ask:      return 10;
            case perm::PermissionAction::AskOnce:  return 10;
        }
        return 0;
    }

    /// Overall status summary (used when action is omitted).
    [[nodiscard]] static std::string format_status_summary() {
        auto rules = perm::get_rules();
        std::size_t n_allow = 0, n_deny = 0, n_ask = 0;
        std::size_t n_global = 0, n_project = 0, n_session = 0, n_command = 0;
        for (const auto& r : rules) {
            switch (r.action) {
                case perm::PermissionAction::Allow:    ++n_allow; break;
                case perm::PermissionAction::Deny:     ++n_deny; break;
                case perm::PermissionAction::Ask:
                case perm::PermissionAction::AskOnce:  ++n_ask; break;
            }
            switch (r.scope) {
                case perm::PermissionScope::Global:  ++n_global; break;
                case perm::PermissionScope::Project: ++n_project; break;
                case perm::PermissionScope::Session: ++n_session; break;
                case perm::PermissionScope::Command: ++n_command; break;
            }
        }
        auto cache = perm::get_cache_stats();

        std::ostringstream oss;
        oss << "Permission Status\n";
        oss << std::string(60, '=') << "\n\n";
        oss << std::format("Total rules    : {}\n", rules.size());
        oss << std::format("  by action    : ALLOW={}, DENY={}, ASK={}\n",
                            n_allow, n_deny, n_ask);
        oss << std::format("  by scope     : global={}, project={}, session={}, command={}\n",
                            n_global, n_project, n_session, n_command);
        oss << std::format("Permission cache: hits={}, misses={}\n\n",
                            cache.first, cache.second);
        oss << "Subcommands:\n";
        oss << "  /permissions list [scope]                  List all rules\n";
        oss << "  /permissions show TOOL                     Show effective rules for TOOL\n";
        oss << "  /permissions allow TOOL [scope]            Add ALLOW rule (default scope=session)\n";
        oss << "  /permissions deny  TOOL [scope]            Add DENY rule\n";
        oss << "  /permissions reset TOOL                    Remove all rules for TOOL\n";
        oss << "  /permissions reset-all                     Remove ALL rules (all scopes)\n";
        oss << "  /permissions dump                          Export rules as JSON array\n";
        oss << "\nScopes: global(g), project(p), session(s), command(c)";
        return oss.str();
    }
};

} // namespace cc::commands
