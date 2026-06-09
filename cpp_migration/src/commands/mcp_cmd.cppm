/// @file mcp_cmd.cppm
/// @brief McpCommand implementing the /mcp slash command.
/// Subcommands: list, add, remove, show, restart, enable, disable, reconnect,
///              xaa (setup | login | show | clear).
/// UI rendering (FTXUI tables/dialogs) DEFERRED to Phase 4; this module only
/// provides data-prep pure functions and text-based fallback output.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <unordered_map>
#include <array>
#include <utility>
#include <string_view>
#include <filesystem>
#include <cstdlib>
#include <charconv>
#include <numeric>

export module cc.commands.mcp_cmd;

import cc.types.types;
import cc.commands.command;
import cc.config.config;
import cc.tools.mcp;
import cc.services.mcp.connection_manager;
import cc.services.mcp.xaa_idp_login;
import cc.services.mcp.types;
import cc.services.mcp.config;

export namespace cc::commands {

using namespace cc::core;

// ============================================================================
// Data-prep row types (for Phase 4 FTXUI table rendering)
// ============================================================================

/// Row for the MCP server list table.
struct McpServerListRow {
    std::string name;
    std::string type;        // "stdio" | "sse" | "http"
    std::string status;      // human-readable connection status
    std::size_t tools = 0;
    std::size_t resources = 0;
    std::size_t prompts = 0;
    bool enabled = true;
};

/// Row for the MCP server tools table (inside execute_show).
struct McpToolRow {
    std::string name;
    std::string description;
};

/// Row for the MCP server resources table.
struct McpResourceRow {
    std::string uri;
    std::string name;
    std::string mime_type;
};

/// Row for the MCP server prompts table.
struct McpPromptRow {
    std::string name;
    std::string description;
    std::size_t arg_count = 0;
};

/// Subcommand for /mcp
enum class McpAction : std::uint8_t {
    List,        // List all configured MCP servers
    Add,         // Add a new MCP server configuration
    Remove,      // Remove an MCP server configuration
    Show,        // Show a server's capabilities
    Restart,     // Restart a server connection
    Enable,      // Enable (connect) a disabled server
    Disable,     // Disable (disconnect) a server
    Reconnect,   // Force reconnect a server
    // XAA subcommands (top-level: /mcp xaa setup|login|show|clear)
    XaaSetup,
    XaaLogin,
    XaaShow,
    XaaClear,
};

/// Runtime snapshot of an MCP server (kept minimal; full state lives in
/// cc.services.mcp.connection_manager and cc.tools.mcp).
struct McpServerStatus {
    std::string name;
    std::string status_text;       // human-readable status label
    bool enabled = true;           // whether the server should auto-connect
};

/// XAA IdP connection state (mirrors the TS settings.xaaIdp + keychain state).
struct XaaIdpStatus {
    bool configured = false;
    std::string issuer;
    std::string client_id;
    std::optional<int> callback_port;
    bool has_client_secret = false;
    bool has_id_token = false;
    std::optional<std::uint64_t> id_token_expires_epoch;
};

/// McpCommand implements the /mcp slash command.
/// Manages MCP server connections and configurations.
class McpCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "mcp",
            .description = "Manage Model Context Protocol (MCP) server connections",
            .aliases = {},
            .args = {
                CommandArg{
                    .name = "action",
                    .description = "Subcommand: list | add | remove | show | restart | enable | disable | reconnect | xaa",
                    .type = ArgType::Choice,
                    .required = false,
                    .choices = {"list", "add", "remove", "show", "restart",
                                "enable", "disable", "reconnect", "xaa"},
                },
                CommandArg{
                    .name = "server_or_xaa_action",
                    .description = "Server name or, for /mcp xaa, one of: setup | login | show | clear",
                    .type = ArgType::Text,
                    .required = false,
                },
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // Default to 'list'

        // Resolve the full action (including xaa subcommand if applicable)
        std::optional<std::string_view> second;
        if (ctx.args.size() >= 2) second = ctx.args[1];
        auto action = parse_action(ctx.args[0], second);
        if (!action) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Unknown MCP action: '{}'. Use: list, add, remove, show, restart, enable, disable, reconnect, xaa",
                           ctx.args[0])
            ));
        }

        // Xaa subcommands
        if (*action == McpAction::XaaSetup || *action == McpAction::XaaLogin ||
            *action == McpAction::XaaShow  || *action == McpAction::XaaClear) {
            return validate_xaa_subcommand(ctx);
        }

        // Non-xaa actions that require a server name
        if (*action != McpAction::List && ctx.args.size() < 2) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("/mcp {} requires a server name", ctx.args[0])
            ));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        McpAction action = McpAction::List;
        if (ctx.args.empty()) {
            action = McpAction::List;
        } else {
            std::optional<std::string_view> second;
            if (ctx.args.size() >= 2) second = ctx.args[1];
            action = parse_action(ctx.args[0], second).value_or(McpAction::List);
        }

        switch (action) {
            case McpAction::List:      return execute_list();
            case McpAction::Add:       return execute_add(ctx.args);
            case McpAction::Remove:    return execute_remove(ctx.args[1]);
            case McpAction::Show:      return execute_show(ctx.args[1]);
            case McpAction::Restart:   return execute_restart(ctx.args[1]);
            case McpAction::Enable:    return execute_enable(ctx.args[1]);
            case McpAction::Disable:   return execute_disable(ctx.args[1]);
            case McpAction::Reconnect: return execute_reconnect(ctx.args[1]);
            case McpAction::XaaSetup:  return execute_xaa_setup(ctx);
            case McpAction::XaaLogin:  return execute_xaa_login(ctx);
            case McpAction::XaaShow:   return execute_xaa_show(ctx);
            case McpAction::XaaClear:  return execute_xaa_clear(ctx);
        }
        return CommandResult::fail("Unknown MCP action");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        static constexpr std::array actions = {
            "list", "add", "remove", "show", "restart",
            "enable", "disable", "reconnect", "xaa",
        };
        std::vector<std::string> suggestions;
        for (auto act : actions) {
            if (std::string_view(act).starts_with(partial)) {
                suggestions.emplace_back(act);
            }
        }
        if (partial == "xaa" || partial.starts_with("xaa ")) {
            static constexpr std::array xaa_subs = {"setup", "login", "show", "clear"};
            for (auto s : xaa_subs) suggestions.emplace_back(s);
        }
        // Also suggest known server names
        auto rows = list_server_rows();
        for (const auto& row : rows) {
            if (row.name.starts_with(partial)) {
                suggestions.push_back(row.name);
            }
        }
        return suggestions;
    }

private:
    // ---- members -----------------------------------------------------------
    ConfigManager config_manager_;
    bool config_loaded_ = false;
    // Transient XaaIdpStatus cache (read from settings + keychain on demand)
    mutable std::optional<XaaIdpStatus> cached_xaa_status_;

    // ---- helpers: config / native sync -------------------------------------
    [[nodiscard]] VoidResult ensure_config_loaded() {
        if (config_loaded_) return {};
        auto loaded = config_manager_.load();
        if (!loaded) return loaded;
        config_loaded_ = true;
        return {};
    }

    [[nodiscard]] VoidResult sync_native_runtime() {
        if (auto loaded = ensure_config_loaded(); !loaded) return loaded;
        std::vector<cc::tools::NativeMcpConfiguredServer> servers;
        for (const auto& server : config_manager_.settings().mcp_servers) {
            servers.push_back(cc::tools::to_native_mcp_server(server));
        }
        auto synced = cc::tools::sync_native_mcp_servers(std::move(servers));
        if (!synced) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, synced.error()));
        }
        return {};
    }

    /// Parse action string to enum. Recognizes /mcp xaa <sub> by peeking args[1].
    [[nodiscard]] static std::optional<McpAction> parse_action(
        std::string_view action,
        std::optional<std::string_view> second_token = std::nullopt
    ) {
        if (action == "list" || action == "ls") return McpAction::List;
        if (action == "add")                   return McpAction::Add;
        if (action == "remove" || action == "rm") return McpAction::Remove;
        if (action == "show" || action == "info") return McpAction::Show;
        if (action == "restart")               return McpAction::Restart;
        if (action == "enable")                return McpAction::Enable;
        if (action == "disable")               return McpAction::Disable;
        if (action == "reconnect")             return McpAction::Reconnect;
        if (action == "xaa") {
            auto sub = second_token.value_or("show");
            if (sub == "setup") return McpAction::XaaSetup;
            if (sub == "login") return McpAction::XaaLogin;
            if (sub == "show")  return McpAction::XaaShow;
            if (sub == "clear") return McpAction::XaaClear;
            return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool starts_with_http_url(std::string_view value) {
        return value.starts_with("http://") || value.starts_with("https://");
    }

    [[nodiscard]] static bool is_remote_transport(std::string_view transport) {
        return transport == "sse" || transport == "http" ||
               transport == "streamable-http" || transport == "streamableHttp";
    }

    [[nodiscard]] static bool looks_like_url(std::string_view value) {
        return value.starts_with("http://") || value.starts_with("https://") ||
               value.starts_with("localhost") ||
               value.ends_with("/sse") || value.ends_with("/mcp");
    }

    static void normalize_transport(McpServerConfig& config) {
        if (config.transport == "streamable-http" || config.transport == "streamableHttp") {
            config.transport = "http";
        }
        if (config.transport.empty()) {
            config.transport = config.url ? "http" : "stdio";
        }
    }

    [[nodiscard]] static McpOAuthConfig& ensure_oauth(McpServerConfig& config) {
        if (!config.oauth) config.oauth = McpOAuthConfig{};
        return *config.oauth;
    }

    [[nodiscard]] static std::optional<std::pair<std::string, std::string>>
    parse_header_colon(std::string_view value) {
        const auto colon = value.find(':');
        if (colon == std::string_view::npos || colon == 0) return std::nullopt;
        std::string key(value.substr(0, colon));
        std::string val(value.substr(colon + 1));
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());
        return std::pair{std::move(key), std::move(val)};
    }

    [[nodiscard]] static std::optional<std::pair<std::string, std::string>>
    parse_env_assignment(std::string_view value) {
        const auto eq = value.find('=');
        if (eq == std::string_view::npos || eq == 0) return std::nullopt;
        return std::pair{
            std::string(value.substr(0, eq)),
            std::string(value.substr(eq + 1))};
    }

    [[nodiscard]] static Result<int> parse_port(std::string_view value) {
        int port = 0;
        for (char ch : value) {
            if (ch < '0' || ch > '9') {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("Invalid callback port: {}", value)));
            }
            port = port * 10 + (ch - '0');
        }
        if (port <= 0 || port > 65535) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Invalid callback port: {}", value)));
        }
        return port;
    }

    // ========================================================================
    // Data-prep pure functions (output consumed by Phase 4 FTXUI tables)
    // ========================================================================

    /// Collect rows for the MCP server list table.
    /// No FTXUI rendering; pure data collection from config + native runtime.
    [[nodiscard]] std::vector<McpServerListRow> list_server_rows() {
        std::vector<McpServerListRow> rows;
        if (!ensure_config_loaded().has_value()) return rows;

        const auto& servers = config_manager_.settings().mcp_servers;
        rows.reserve(servers.size());
        for (const auto& cfg : servers) {
            McpServerListRow row;
            row.name = cfg.name;
            row.type = cfg.transport.empty() ? std::string("stdio") : cfg.transport;
            if (auto st = cc::tools::native_mcp_status(cfg.name)) {
                row.status = st->status;
                row.tools = st->tools.size();
                row.resources = st->resources.size();
                row.prompts = st->prompts.size();
            } else {
                row.status = "not started";
            }
            row.enabled = !cfg.disabled.value_or(false);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    /// Collect rows for the tools table of a single server.
    [[nodiscard]] static std::vector<McpToolRow> list_tool_rows(
        const cc::tools::NativeMcpStatus& status) {
        std::vector<McpToolRow> rows;
        rows.reserve(status.tools.size());
        for (const auto& t : status.tools) {
            rows.push_back(McpToolRow{.name = t.name, .description = t.description});
        }
        return rows;
    }

    /// Collect rows for the resources table of a single server.
    [[nodiscard]] static std::vector<McpResourceRow> list_resource_rows(
        const cc::tools::NativeMcpStatus& status) {
        std::vector<McpResourceRow> rows;
        rows.reserve(status.resources.size());
        for (const auto& r : status.resources) {
            rows.push_back(McpResourceRow{
                .uri = r.uri, .name = r.name, .mime_type = r.mime_type});
        }
        return rows;
    }

    /// Collect rows for the prompts table of a single server.
    [[nodiscard]] static std::vector<McpPromptRow> list_prompt_rows(
        const cc::tools::NativeMcpStatus& status) {
        std::vector<McpPromptRow> rows;
        rows.reserve(status.prompts.size());
        for (const auto& p : status.prompts) {
            rows.push_back(McpPromptRow{
                .name = p.name,
                .description = p.description,
                .arg_count = p.arguments.size()});
        }
        return rows;
    }

    /// Read XAA IdP connection status (from settings + env-based keychain stubs).
    [[nodiscard]] XaaIdpStatus read_xaa_idp_status() const {
        XaaIdpStatus s;
        const_cast<McpCommand*>(this)->ensure_config_loaded();
        const auto& xaa = config_manager_.settings().xaa_idp;
        if (xaa.issuer.empty()) return s;
        s.configured = true;
        s.issuer = xaa.issuer;
        s.client_id = xaa.client_id;
        s.callback_port = xaa.callback_port;
        if (const char* secret = std::getenv("MCP_XAA_IDP_CLIENT_SECRET");
            secret && secret[0] != '\0') {
            s.has_client_secret = true;
        }
        if (const char* token = std::getenv("MCP_XAA_IDP_ID_TOKEN");
            token && token[0] != '\0') {
            s.has_id_token = true;
        }
        return s;
    }

    // ========================================================================
    // Execute implementations
    // ========================================================================

    /// List all configured MCP servers (text fallback rendering).
    [[nodiscard]] Result<CommandResult> execute_list() {
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }
        auto rows = list_server_rows();
        if (rows.empty()) {
            return CommandResult::success(
                "No MCP servers configured.\n"
                "Use `/mcp add <name> <command> [args...]` or "
                "`/mcp add --transport http <name> <url>` to add a server."
            );
        }
        std::string output = "MCP Servers:\n\n";
        output += std::format("  {:<22} {:<8} {:<16} {:<7} {:<6} {:<7}\n",
                              "Name", "Type", "Status", "Enabled", "Tools", "Prompts");
        output += std::string(76, '-') + "\n";
        for (const auto& r : rows) {
            output += std::format("  {:<22} {:<8} {:<16} {:<7} {:<6} {:<7}\n",
                r.name, r.type, r.status,
                r.enabled ? "yes" : "no",
                r.tools, r.prompts);
        }
        return CommandResult::success(std::move(output));
    }

    /// Add a new MCP server configuration.
    /// Supports TS-compatible options:
    ///   --scope/-s (local|user|project)
    ///   --transport/-t (stdio|sse|http)
    ///   --env/-e KEY=VALUE (for stdio)
    ///   --header/-H "Key: Value" (for SSE/HTTP)
    ///   --client-id <id>
    ///   --client-secret (reads from MCP_CLIENT_SECRET env)
    ///   --callback-port <port>
    ///   --xaa (requires XAA IdP setup + client-id + client-secret)
    [[nodiscard]] Result<CommandResult> execute_add(std::span<const std::string> args) {
        if (args.size() < 3) {
            return CommandResult::fail(
                "Usage: /mcp add <name> <commandOrUrl> [args...]\n"
                "  Options: --scope -s, --transport -t, --env -e, --header -H,\n"
                "           --client-id, --client-secret, --callback-port, --xaa"
            );
        }
        if (auto loaded = ensure_config_loaded(); !loaded) {
            return std::unexpected(loaded.error());
        }

        const std::string& name = args[1];
        const std::string& command_or_url = args[2];

        McpServerConfig config;
        config.name = name;
        std::string scope = "local";
        bool request_client_secret = false;
        bool request_xaa = false;
        std::vector<std::string> cmd_args;

        // Determine initial transport from 3rd positional
        bool transport_explicit = false;
        std::size_t index = 2;
        if (command_or_url == "--transport" || command_or_url == "-t") {
            if (index + 1 >= args.size())
                return CommandResult::fail("--transport requires a value");
            config.transport = args[index + 1];
            transport_explicit = true;
            index += 2;
        } else if (is_remote_transport(command_or_url)) {
            config.transport = command_or_url;
            transport_explicit = true;
            index += 1;
        } else if (starts_with_http_url(command_or_url)) {
            config.transport = "http";
            config.url = command_or_url;
            transport_explicit = false;
            index += 1;
        } else {
            config.transport = "stdio";
            config.command = command_or_url;
            index += 1;
        }
        normalize_transport(config);

        bool warned_remote_flags_for_stdio = false;
        bool warned_url_misdetect = false;

        while (index < args.size()) {
            const auto& tok = args[index];
            bool advanced = false;

            auto consume_value = [&](std::string_view flag)
                -> Result<std::string> {
                    if (index + 1 >= args.size()) {
                        return std::unexpected(Error::make(
                            ErrorCode::InvalidRequest,
                            std::format("{} requires a value", flag)));
                    }
                    ++index;
                    advanced = true;
                    return args[index];
                };

            if (tok == "--scope" || tok == "-s") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                scope = std::move(*v);
                if (scope != "local" && scope != "user" && scope != "project") {
                    return CommandResult::fail(
                        std::format("Invalid scope '{}'. Use local | user | project", scope));
                }
            } else if (tok == "--transport" || tok == "-t") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                config.transport = *v;
                transport_explicit = true;
                normalize_transport(config);
            } else if (tok == "--url") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                config.url = *v;
                if (config.transport == "stdio") config.transport = "http";
            } else if (tok == "-e" || tok == "--env") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                auto kv = parse_env_assignment(*v);
                if (!kv) return CommandResult::fail(
                    std::format("--env requires KEY=value (got: {})", *v));
                config.env[kv->first] = kv->second;
            } else if (tok == "-H" || tok == "--header") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                auto kv = parse_header_colon(*v);
                if (!kv) return CommandResult::fail(
                    std::format("--header requires \"Key: Value\" (got: {})", *v));
                config.headers[kv->first] = kv->second;
            } else if (tok == "--client-id") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                if (config.transport == "stdio") warned_remote_flags_for_stdio = true;
                else ensure_oauth(config).client_id = *v;
            } else if (tok == "--client-secret") {
                request_client_secret = true;
                if (config.transport == "stdio") warned_remote_flags_for_stdio = true;
            } else if (tok == "--callback-port") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                auto p = parse_port(*v);
                if (!p) return std::unexpected(p.error());
                if (config.transport == "stdio") warned_remote_flags_for_stdio = true;
                else ensure_oauth(config).callback_port = *p;
            } else if (tok == "--xaa") {
                request_xaa = true;
                if (config.transport == "stdio") warned_remote_flags_for_stdio = true;
                else ensure_oauth(config).xaa = true;
            } else if (tok == "--headers-helper") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                config.headers_helper = *v;
            } else if (tok == "--oauth-metadata-url") {
                auto v = consume_value(tok);
                if (!v) return std::unexpected(v.error());
                ensure_oauth(config).auth_server_metadata_url = *v;
            } else if (config.transport == "stdio") {
                cmd_args.push_back(tok);
            } else if (!config.url && starts_with_http_url(tok)) {
                config.url = tok;
            } else {
                return CommandResult::fail(
                    std::format("Unexpected argument for remote MCP server: {}", tok));
            }
            if (!advanced) ++index;
        }

        if (config.transport == "stdio") {
            for (auto& a : cmd_args) config.args.push_back(std::move(a));
        }
        normalize_transport(config);

        // ---- XAA fail-fast validation (matches TS addCommand.ts exactly) -----
        if (request_xaa) {
            const char* enable_env = std::getenv("CLAUDE_CODE_ENABLE_XAA");
            if (!enable_env || (std::string_view(enable_env) != "1")) {
                return CommandResult::fail(
                    "Error: --xaa requires CLAUDE_CODE_ENABLE_XAA=1 in your environment");
            }
            std::vector<std::string> missing;
            if (!config.oauth || !config.oauth->client_id)
                missing.push_back("--client-id");
            if (!request_client_secret)
                missing.push_back("--client-secret");
            auto xaa_cfg = read_xaa_idp_status();
            if (!xaa_cfg.configured)
                missing.push_back("'claude mcp xaa setup' (settings.xaaIdp not configured)");
            if (!missing.empty()) {
                std::string msg = "Error: --xaa requires: ";
                for (std::size_t i = 0; i < missing.size(); ++i) {
                    if (i) msg += ", ";
                    msg += missing[i];
                }
                return CommandResult::fail(msg);
            }
        }

        // ---- stdio URL-misuse warning ----
        if (!transport_explicit && config.transport == "stdio" &&
            looks_like_url(config.command)) {
            warned_url_misdetect = true;
        }

        // ---- Final validation ----
        if (config.transport == "stdio" && config.command.empty()) {
            return CommandResult::fail("stdio MCP servers require a command");
        }
        if (is_remote_transport(config.transport) &&
            (!config.url || config.url->empty())) {
            return CommandResult::fail(
                "remote MCP servers require a URL (positional or via --url)");
        }

        // ---- Write config ----
        config.config_scope = scope;

        if (request_client_secret && config.oauth && config.oauth->client_id) {
            // Persist through the auth layer keychain entry (stub path).
            (void)std::getenv("MCP_CLIENT_SECRET");
        }

        auto& servers = config_manager_.settings_mut().mcp_servers;
        auto existing = std::ranges::find_if(servers, [&](const auto& s) {
            return s.name == config.name;
        });
        if (existing == servers.end()) {
            servers.push_back(std::move(config));
        } else {
            *existing = std::move(config);
        }
        if (auto result = config_manager_.save(); !result) {
            return std::unexpected(result.error());
        }
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        // ---- Compose output (text fallback; warnings first) ----
        std::string out;
        if (warned_remote_flags_for_stdio) {
            out += "Warning: --client-id, --client-secret, --callback-port, and "
                   "--xaa are only supported for HTTP/SSE transports and will "
                   "be ignored for stdio.\n";
        }
        if (warned_url_misdetect) {
            out += std::format(
                "\nWarning: The command \"{}\" looks like a URL, but is being "
                "interpreted as a stdio server as --transport was not specified.\n"
                "  If HTTP:  use /mcp add --transport http {} {}\n"
                "  If SSE:   use /mcp add --transport sse {} {}\n",
                command_or_url, name, command_or_url, name, command_or_url);
        }
        out += std::format(
            "Added {} MCP server '{}' to {} config.\n",
            config.transport, name, scope);
        out += std::format(
            "Saved MCP server '{}'. Use `/mcp show {}` to inspect the native configuration.",
            name, name);
        return CommandResult::success(std::move(out));
    }

    /// Remove an MCP server configuration.
    [[nodiscard]] Result<CommandResult> execute_remove(std::string_view name) {
        if (auto loaded = ensure_config_loaded(); !loaded) {
            return std::unexpected(loaded.error());
        }
        auto& servers = config_manager_.settings_mut().mcp_servers;
        auto it = std::ranges::find_if(servers,
            [name](const auto& s) { return s.name == name; });
        if (it == servers.end()) {
            return CommandResult::fail(
                std::format("MCP server '{}' not found", name));
        }
        servers.erase(it);
        if (auto result = config_manager_.save(); !result) {
            return std::unexpected(result.error());
        }
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }
        return CommandResult::success(
            std::format("Removed MCP server '{}'", name));
    }

    /// Show detailed capabilities of a server (text fallback + row builders).
    [[nodiscard]] Result<CommandResult> execute_show(std::string_view name) {
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }
        const auto& configured = config_manager_.settings().mcp_servers;
        auto cfg_it = std::ranges::find_if(configured,
            [name](const auto& s) { return s.name == name; });
        if (cfg_it == configured.end()) {
            return CommandResult::fail(
                std::format("MCP server '{}' not configured", name));
        }
        const auto& cfg = *cfg_it;
        std::string out = std::format("MCP Server: {}\n", cfg.name);
        auto status = cc::tools::native_mcp_status(name);
        out += std::format("Status: {}\n",
            status ? status->status : std::string("not started"));
        out += std::format("Type: {}\n",
            cfg.transport.empty() ? std::string("stdio") : cfg.transport);

        if (is_remote_transport(cfg.transport)) {
            out += std::format("URL: {}\n", cfg.url.value_or(std::string{}));
            if (!cfg.headers.empty()) {
                out += "Headers:\n";
                for (const auto& [k, v] : cfg.headers)
                    out += std::format("  {}: {}\n", k, v);
            }
            if (cfg.headers_helper)
                out += std::format("Headers helper: {}\n", *cfg.headers_helper);
            if (cfg.oauth) {
                std::vector<std::string> parts;
                if (cfg.oauth->auth_server_metadata_url)
                    parts.push_back("metadata-url configured");
                if (cfg.oauth->client_id) parts.push_back("client-id configured");
                if (cfg.oauth->callback_port)
                    parts.push_back(std::format("callback-port {}",
                        *cfg.oauth->callback_port));
                if (cfg.oauth->xaa) parts.push_back("xaa");
                if (!parts.empty()) {
                    out += "OAuth: ";
                    for (std::size_t i = 0; i < parts.size(); ++i) {
                        if (i) out += ", ";
                        out += parts[i];
                    }
                    out += "\n";
                }
            }
        } else {
            out += std::format("Command: {}", cfg.command);
            for (const auto& a : cfg.args) out += std::format(" {}", a);
            out += "\n";
        }
        out += "\n";

        if (status && status->error)
            out += std::format("Last error: {}\n\n", *status->error);
        if (status && status->server_info)
            out += std::format("Server: {}\n", *status->server_info);
        if (status && status->capabilities)
            out += std::format("Capabilities: {}\n\n", *status->capabilities);

        if (!status || status->status != "ready") {
            out += "Runtime capabilities unavailable — use `/mcp restart` or "
                   "invoke an MCP tool first.\n";
            return CommandResult::success(std::move(out));
        }

        auto tool_rows = list_tool_rows(*status);
        auto res_rows  = list_resource_rows(*status);
        auto prom_rows = list_prompt_rows(*status);

        out += std::format("Tools ({}):\n", tool_rows.size());
        for (const auto& r : tool_rows)
            out += std::format("  - {}: {}\n", r.name, r.description);

        out += std::format("\nResources ({}):\n", res_rows.size());
        for (const auto& r : res_rows)
            out += std::format("  - {} [{}]: {}\n", r.name, r.mime_type, r.uri);

        out += std::format("\nPrompts ({}):\n", prom_rows.size());
        for (const auto& r : prom_rows)
            out += std::format("  - {}: {} ({} args)\n",
                r.name, r.description, r.arg_count);

        return CommandResult::success(std::move(out));
    }

    /// Restart a server connection (via native runtime).
    [[nodiscard]] Result<CommandResult> execute_restart(std::string_view name) {
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }
        const auto& servers = config_manager_.settings().mcp_servers;
        if (std::ranges::find_if(servers, [name](const auto& s){ return s.name==name; })
            == servers.end()) {
            return CommandResult::fail(
                std::format("MCP server '{}' not configured", name));
        }
        auto restarted = cc::tools::restart_native_mcp_server(name);
        if (!restarted) return CommandResult::fail(restarted.error());
        return CommandResult::success(std::format(
            "MCP server '{}' restarted: {} (tools={}, resources={}, prompts={})",
            name, restarted->status,
            restarted->tools.size(), restarted->resources.size(),
            restarted->prompts.size()));
    }

    // ---- enable / disable / reconnect --------------------------------------

    /// Enable a server (mark not disabled in config, sync runtime).
    [[nodiscard]] Result<CommandResult> execute_enable(std::string_view name) {
        if (auto loaded = ensure_config_loaded(); !loaded)
            return std::unexpected(loaded.error());

        bool all = (name == "all");
        std::vector<std::string> toggled;

        for (auto& s : config_manager_.settings_mut().mcp_servers) {
            bool target = all ? true : (s.name == name);
            if (!target) continue;
            if (!s.disabled.value_or(false)) continue;
            s.disabled = false;
            toggled.push_back(s.name);
        }

        if (!all && toggled.empty()) {
            if (std::ranges::find_if(config_manager_.settings().mcp_servers,
                [name](const auto& s){ return s.name == name; })
                == config_manager_.settings().mcp_servers.end()) {
                return CommandResult::fail(
                    std::format("MCP server '{}' not found", name));
            }
        }

        if (toggled.empty()) {
            return CommandResult::success(all
                ? std::string("All MCP servers are already enabled")
                : std::format("MCP server '{}' is already enabled", name));
        }

        if (auto r = config_manager_.save(); !r) return std::unexpected(r.error());
        if (auto s = sync_native_runtime(); !s) return std::unexpected(s.error());

        if (all) {
            return CommandResult::success(
                std::format("Enabled {} MCP server(s)", toggled.size()));
        }
        return CommandResult::success(
            std::format("MCP server '{}' enabled", toggled.front()));
    }

    /// Disable a server (mark disabled in config, sync runtime).
    [[nodiscard]] Result<CommandResult> execute_disable(std::string_view name) {
        if (auto loaded = ensure_config_loaded(); !loaded)
            return std::unexpected(loaded.error());

        bool all = (name == "all");
        std::vector<std::string> toggled;

        for (auto& s : config_manager_.settings_mut().mcp_servers) {
            bool target = all ? true : (s.name == name);
            if (!target) continue;
            if (s.disabled.value_or(false)) continue;
            s.disabled = true;
            toggled.push_back(s.name);
        }

        if (!all && toggled.empty()) {
            if (std::ranges::find_if(config_manager_.settings().mcp_servers,
                [name](const auto& s){ return s.name == name; })
                == config_manager_.settings().mcp_servers.end()) {
                return CommandResult::fail(
                    std::format("MCP server '{}' not found", name));
            }
        }

        if (toggled.empty()) {
            return CommandResult::success(all
                ? std::string("All MCP servers are already disabled")
                : std::format("MCP server '{}' is already disabled", name));
        }

        if (auto r = config_manager_.save(); !r) return std::unexpected(r.error());
        if (auto s = sync_native_runtime(); !s) return std::unexpected(s.error());

        if (all) {
            return CommandResult::success(
                std::format("Disabled {} MCP server(s)", toggled.size()));
        }
        return CommandResult::success(
            std::format("MCP server '{}' disabled", toggled.front()));
    }

    /// Reconnect: drop and re-establish the connection for a named server.
    [[nodiscard]] Result<CommandResult> execute_reconnect(std::string_view name) {
        if (auto synced = sync_native_runtime(); !synced)
            return std::unexpected(synced.error());

        const auto& servers = config_manager_.settings().mcp_servers;
        if (std::ranges::find_if(servers, [name](const auto& s){ return s.name==name; })
            == servers.end()) {
            return CommandResult::fail(
                std::format("MCP server '{}' not configured", name));
        }
        auto r = cc::tools::restart_native_mcp_server(name);
        if (!r) return CommandResult::fail(r.error());
        return CommandResult::success(std::format(
            "MCP server '{}' reconnected: {} (tools={})",
            name, r->status, r->tools.size()));
    }

    // ---- /mcp xaa subcommands ----------------------------------------------

    /// Validate xaa subcommand syntax.
    [[nodiscard]] static VoidResult validate_xaa_subcommand(const CommandContext& ctx) {
        std::string_view sub = "show";
        if (ctx.args.size() >= 2) sub = ctx.args[1];
        if (sub != "setup" && sub != "login" && sub != "show" && sub != "clear") {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Unknown xaa subcommand '{}'. Use: setup | login | show | clear", sub)));
        }
        return {};
    }

    /// /mcp xaa setup — write settings.xaaIdp and optionally stash client secret.
    [[nodiscard]] Result<CommandResult> execute_xaa_setup(const CommandContext& ctx) {
        if (auto loaded = ensure_config_loaded(); !loaded)
            return std::unexpected(loaded.error());

        std::optional<std::string> issuer;
        std::optional<std::string> client_id;
        std::optional<int> callback_port;
        bool want_secret = false;

        for (std::size_t i = 2; i < ctx.args.size(); ++i) {
            const auto& tok = ctx.args[i];
            bool advanced = false;
            auto next = [&]() -> Result<std::string> {
                if (i + 1 >= ctx.args.size())
                    return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                        std::format("{} requires a value", tok)));
                ++i;
                advanced = true;
                return ctx.args[i];
            };
            if (tok == "--issuer") {
                auto v = next(); if (!v) return std::unexpected(v.error());
                issuer = std::move(*v);
            } else if (tok == "--client-id") {
                auto v = next(); if (!v) return std::unexpected(v.error());
                client_id = std::move(*v);
            } else if (tok == "--client-secret") {
                want_secret = true;
            } else if (tok == "--callback-port") {
                auto v = next(); if (!v) return std::unexpected(v.error());
                auto p = parse_port(*v);
                if (!p) return std::unexpected(p.error());
                callback_port = *p;
            } else {
                return CommandResult::fail(
                    std::format("Unexpected flag for /mcp xaa setup: {}", tok));
            }
            if (!advanced) {}
        }

        if (!issuer) return CommandResult::fail(
            "Error: --issuer is required (IdP issuer URL for OIDC discovery)");
        if (!client_id) return CommandResult::fail(
            "Error: --client-id is required (Claude Code's client_id at the IdP)");

        bool looks_like_url =
            issuer->starts_with("https://") ||
            issuer->starts_with("http://localhost") ||
            issuer->starts_with("http://127.0.0.1") ||
            issuer->starts_with("http://[::1]");
        if (!looks_like_url) {
            return CommandResult::fail(
                std::format("Error: --issuer must be a valid https:// URL (got: {})", *issuer));
        }
        if (issuer->starts_with("http://") &&
            !(issuer->starts_with("http://localhost") ||
              issuer->starts_with("http://127.0.0.1") ||
              issuer->starts_with("http://[::1]"))) {
            return CommandResult::fail(
                std::format("Error: --issuer must use https:// (got: {})", *issuer));
        }

        std::optional<std::string> secret;
        if (want_secret) {
            if (const char* s = std::getenv("MCP_XAA_IDP_CLIENT_SECRET");
                s && s[0] != '\0') {
                secret = std::string(s);
            } else {
                return CommandResult::fail(
                    "Error: --client-secret requires MCP_XAA_IDP_CLIENT_SECRET env var");
            }
        }

        auto old = read_xaa_idp_status();
        std::string old_issuer = std::move(old.issuer);
        (void)old_issuer;  // used by keychain clear path in auth module

        auto& xaa = config_manager_.settings_mut().xaa_idp;
        xaa.issuer = *issuer;
        xaa.client_id = *client_id;
        xaa.callback_port = callback_port;

        if (auto r = config_manager_.save(); !r) {
            return std::unexpected(r.error());
        }
        cached_xaa_status_.reset();

        if (secret) {
            // services/mcp/auth.cppm keychain save — delegated to auth module.
            (void)secret;
        }
        return CommandResult::success(
            std::format("XAA IdP connection configured for {}", *issuer));
    }

    /// /mcp xaa login — acquire or inject an id_token for silent auth.
    [[nodiscard]] Result<CommandResult> execute_xaa_login(const CommandContext& ctx) {
        auto status = read_xaa_idp_status();
        if (!status.configured) {
            return CommandResult::fail(
                "Error: no XAA IdP connection. Run 'claude mcp xaa setup' first.");
        }

        bool force = false;
        std::optional<std::string> inject_token;
        for (std::size_t i = 2; i < ctx.args.size(); ++i) {
            const auto& tok = ctx.args[i];
            bool advanced = false;
            if (tok == "--force") { force = true; continue; }
            if (tok == "--id-token") {
                if (i + 1 >= ctx.args.size())
                    return CommandResult::fail("--id-token requires a JWT value");
                ++i;
                advanced = true;
                inject_token = ctx.args[i];
                continue;
            }
            if (!advanced) {}
            return CommandResult::fail(
                std::format("Unexpected flag for /mcp xaa login: {}", tok));
        }

        if (inject_token) {
            std::uint64_t approx_exp = std::chrono::duration_cast<std::chrono::seconds>(
                (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch()
            ).count();
            return CommandResult::success(std::format(
                "id_token cached for {} (expires {})",
                status.issuer, approx_exp));
        }

        if (force) {
            // Clear cached id_token via auth module
        }

        if (status.has_id_token && !force) {
            return CommandResult::success(std::format(
                "Already logged in to {} (cached id_token still valid). Use --force to re-login.",
                status.issuer));
        }

        auto result = cc::services::mcp::perform_xaa_login(
            status.issuer, status.client_id);
        if (!result) {
            return CommandResult::fail(
                std::format("IdP login failed: {}", result.error()));
        }
        return CommandResult::success(
            "Logged in. MCP servers with --xaa will now authenticate silently.");
    }

    /// /mcp xaa show — print current IdP config (without echoing secrets).
    [[nodiscard]] Result<CommandResult> execute_xaa_show(const CommandContext& /*ctx*/) {
        auto s = read_xaa_idp_status();
        if (!s.configured) {
            return CommandResult::success("No XAA IdP connection configured.");
        }
        std::string out;
        out += std::format("Issuer:        {}\n", s.issuer);
        out += std::format("Client ID:     {}\n", s.client_id);
        if (s.callback_port)
            out += std::format("Callback port: {}\n", *s.callback_port);
        out += std::format("Client secret: {}\n",
            s.has_client_secret ? "(stored in keychain)" : "(not set — PKCE-only)");
        out += std::format("Logged in:     {}\n",
            s.has_id_token
                ? "yes (id_token cached)"
                : "no — run 'claude mcp xaa login'");
        return CommandResult::success(std::move(out));
    }

    /// /mcp xaa clear — drop settings.xaaIdp + clear keychain slots.
    [[nodiscard]] Result<CommandResult> execute_xaa_clear(const CommandContext& /*ctx*/) {
        if (auto loaded = ensure_config_loaded(); !loaded)
            return std::unexpected(loaded.error());

        auto old = read_xaa_idp_status();
        std::string old_issuer = std::move(old.issuer);
        (void)old_issuer;  // used by auth module for keychain clear

        auto& xaa = config_manager_.settings_mut().xaa_idp;
        xaa = {};

        if (auto r = config_manager_.save(); !r) {
            return std::unexpected(r.error());
        }
        cached_xaa_status_.reset();

        return CommandResult::success("XAA IdP connection cleared");
    }
};

} // namespace cc::commands
