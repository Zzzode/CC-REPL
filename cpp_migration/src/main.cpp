/**
 * CC-REPL Main Entry Point - C++23 Version
 *
 * Bootstraps the CLI application: parses arguments, initializes subsystems,
 * and launches the full interactive FTXUI-based UI.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

// Import our core modules
import cc.query.query_engine;
import cc.types.types;
import cc.tools.tool;
import cc.tools.agent_runtime;
import cc.tools.runtime_registry;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;
import cc.types.command;
import cc.commands.command;
import cc.commands.registry;
import cc.constants.product;
import cc.services.api.session_ingress;
import cc.utils.session_storage;
import cc.utils.json;
import cc.utils.http;
import cc.session.history;
import cc.daemon.daemon_server;
import cc.server.server_main;
import cc.cli.websocket_transport;
import cc.config.settings;

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
namespace fs = std::filesystem;

extern "C" [[nodiscard]] int cc_ui_run_app_bridge(
    cc::core::QueryEngine* engine,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
    cc::commands::AppCommandRegistry* cmd_registry,
    cc::utils::SessionStorage* storage,
    cc::hooks::ToolPermissionHook* permission_hook
);

// Application version constant
constexpr std::string_view kVersion = cc::constants::product::CC_REPL_VERSION;

/**
 * Parsed command-line options
 */
struct CliOptions {
    std::optional<std::string> model;
    bool show_version = false;
    bool show_help = false;
    bool debug = false;
    bool use_simple_ui = false;  // Fallback to simple text UI if FTXUI fails
    bool headless = false;
    bool permissions = false;    // Enable permission checking (disables auto-approve)
    bool list_runtime_tools = false;
    bool list_runtime_commands = false;
    bool server = false;
    bool bridge_daemon = false;
    bool bridge_daemon_once = false;
    bool dangerously_skip_permissions = false;
    bool plan_mode_required = false;
    // --settings <file-or-json>: parsed EARLY (before init) per TS semantics.
    // Accepts either a path to a JSON file or an inline JSON object string.
    std::optional<std::string> settings;
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 3000;
    std::optional<std::string> server_auth_token;
    std::optional<std::string> bridge_work_api_url;
    std::optional<std::string> bridge_environment_id;
    std::optional<std::string> bridge_environment_secret;
    std::optional<std::string> bridge_access_token;
    std::optional<std::string> bridge_session_binary;
    int bridge_daemon_wait_ms = 10000;
    std::string executable_path{"cc-repl"};
    std::optional<std::string> agents_json;
    std::optional<std::string> permission_mode;
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_name;
    std::optional<std::string> team_name;
    std::optional<std::string> agent_type;
    std::optional<std::string> agent_color;
    std::optional<std::string> parent_session_id;
    std::optional<std::string> session_id;
    std::optional<std::string> task_id;
    std::optional<std::uint32_t> task_budget;
    bool continue_session = false;
    std::optional<std::string> resume_session_id;
    std::optional<std::string> session_store_path;
    std::optional<std::string> input_format;
    std::optional<std::string> output_format;
    std::optional<std::string> sdk_url;
    bool replay_user_messages = false;
    std::optional<std::string> runtime_tool_name;
    std::optional<std::string> runtime_tool_input_json;
};

/**
 * Print usage/help text to stdout
 */
void print_help() {
    std::println(R"(CC-REPL: Claude REPL (C++23 Version)

Usage: cc-repl [options]

Options:
  --model <model>      Set the default model to use
  --version, -v        Print version and exit
  --help, -h           Show this help message
  --settings <file|json>
                       Load settings from a JSON file path or inline JSON.
                       Highest-priority source. Supports `env` (process env
                       vars, e.g. ANTHROPIC_API_KEY/ANTHROPIC_BASE_URL),
                       `apiKey`, `model`, and `statusLine`.
  --debug              Enable debug logging
  --simple-ui          Use simple text UI (not interactive)
  --headless           Run without UI for daemon/remote session work
  --input-format <format>
                       Headless input format: text or stream-json
  --output-format <format>
                       Headless output format: text, json, or stream-json
  --permissions        Enable permission checking for tool execution
  --list-runtime-tools Print registered runtime tool names and exit
  --list-runtime-commands
                       Print registered runtime command names and exit
  --run-runtime-tool <name>
                       Execute one runtime tool non-interactively and exit
  --runtime-tool-input <json>
                       JSON input for --run-runtime-tool (default: {{}})
  --server             Start the direct-connect HTTP/WebSocket server
  --server-host <host> Host for --server (default: 127.0.0.1)
  --server-port <port> Port for --server (default: 3000)
  --server-auth-token <token>
                       Require Bearer auth for --server requests
  --bridge-daemon      Run a long-lived bridge daemon for remote work
  --bridge-daemon-once Poll one remote bridge work item, spawn headless, and report completion
  --bridge-work-api-url <url>
                       Bridge/CCR API base URL for bridge daemon modes
  --bridge-environment-id <id>
                       Bridge environment id for work polling
  --bridge-environment-secret <secret>
                       Bridge environment secret for work polling
  --bridge-access-token <token>
                       Bridge OAuth access token for stop/archive
  --bridge-session-binary <path>
                       Headless child binary for daemon-spawned sessions
  --bridge-daemon-wait-ms <ms>
                       Max wait for a spawned headless session result (default: 10000)
  --task-budget <tokens>
                       API-side task budget in tokens
  --continue, -c       Continue the active persisted C++ conversation
  --resume [id]        Resume a persisted C++ conversation by ID, or active if omitted
  --session-store <path>
                       Persist/resume C++ conversation history at path
  --agents <json>      JSON object defining custom agents
  --permission-mode <mode>
                       Parent permission mode for this session
  --dangerously-skip-permissions
                       Bypass permission prompts for this session
  --agent-type <type>  Agent definition type for a spawned teammate session

Examples:
  cc-repl                                    # Start interactive mode
  cc-repl --model claude-3-5-sonnet-20241022 # Use specific model
  cc-repl --server --server-port 3000        # Start direct-connect server
)");
}

/**
 * Parse command-line arguments
 */
auto parse_args(int argc, const char* argv[]) -> std::expected<CliOptions, std::string> {
    CliOptions opts;
    if (argc > 0 && argv[0]) {
        opts.executable_path = argv[0];
    }

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        if (arg == "--version" || arg == "-v") {
            opts.show_version = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.show_help = true;
        } else if (arg == "--settings") {
            // --settings accepts a path to a JSON file OR an inline JSON object.
            // Parsed eagerly in main() before other subsystems start, mirroring
            // the TS loadSettingsFromFlag / eagerLoadSettings flow.
            if (++i >= argc) {
                return std::unexpected("--settings requires a value");
            }
            opts.settings = std::string(argv[i]);
        } else if (arg.starts_with("--settings=")) {
            opts.settings = std::string(arg.substr(std::string_view("--settings=").size()));
        } else if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--simple-ui") {
            opts.use_simple_ui = true;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--print" || arg == "-p") {
            opts.headless = true;
        } else if (arg == "--input-format") {
            if (++i >= argc) {
                return std::unexpected("--input-format requires a value");
            }
            opts.input_format = std::string(argv[i]);
        } else if (arg.starts_with("--input-format=")) {
            opts.input_format = std::string(arg.substr(std::string_view("--input-format=").size()));
        } else if (arg == "--output-format") {
            if (++i >= argc) {
                return std::unexpected("--output-format requires a value");
            }
            opts.output_format = std::string(argv[i]);
        } else if (arg.starts_with("--output-format=")) {
            opts.output_format = std::string(arg.substr(std::string_view("--output-format=").size()));
        } else if (arg == "--replay-user-messages") {
            opts.replay_user_messages = true;
        } else if (arg == "--sdk-url") {
            if (++i >= argc) {
                return std::unexpected("--sdk-url requires a value");
            }
            opts.sdk_url = std::string(argv[i]);
        } else if (arg.starts_with("--sdk-url=")) {
            opts.sdk_url = std::string(arg.substr(std::string_view("--sdk-url=").size()));
        } else if (arg == "--permissions") {
            opts.permissions = true;
        } else if (arg == "--list-runtime-tools") {
            opts.list_runtime_tools = true;
        } else if (arg == "--list-runtime-commands") {
            opts.list_runtime_commands = true;
        } else if (arg == "--run-runtime-tool") {
            if (++i >= argc) {
                return std::unexpected("--run-runtime-tool requires a value");
            }
            opts.runtime_tool_name = std::string(argv[i]);
        } else if (arg.starts_with("--run-runtime-tool=")) {
            opts.runtime_tool_name = std::string(arg.substr(std::string_view("--run-runtime-tool=").size()));
        } else if (arg == "--runtime-tool-input") {
            if (++i >= argc) {
                return std::unexpected("--runtime-tool-input requires a JSON value");
            }
            opts.runtime_tool_input_json = std::string(argv[i]);
        } else if (arg.starts_with("--runtime-tool-input=")) {
            opts.runtime_tool_input_json = std::string(arg.substr(std::string_view("--runtime-tool-input=").size()));
        } else if (arg == "--server") {
            opts.server = true;
        } else if (arg == "--bridge-daemon") {
            opts.bridge_daemon = true;
        } else if (arg == "--bridge-daemon-once") {
            opts.bridge_daemon_once = true;
        } else if (arg == "--dangerously-skip-permissions") {
            opts.dangerously_skip_permissions = true;
        } else if (arg == "--plan-mode-required") {
            opts.plan_mode_required = true;
        } else if (arg == "--model") {
            if (++i >= argc) {
                return std::unexpected("--model requires a value");
            }
            opts.model = std::string(argv[i]);
        } else if (arg == "--permission-mode") {
            if (++i >= argc) {
                return std::unexpected("--permission-mode requires a value");
            }
            opts.permission_mode = std::string(argv[i]);
        } else if (arg.starts_with("--permission-mode=")) {
            opts.permission_mode = std::string(arg.substr(std::string_view("--permission-mode=").size()));
        } else if (arg == "--agent-id") {
            if (++i >= argc) {
                return std::unexpected("--agent-id requires a value");
            }
            opts.agent_id = std::string(argv[i]);
        } else if (arg == "--agent-name") {
            if (++i >= argc) {
                return std::unexpected("--agent-name requires a value");
            }
            opts.agent_name = std::string(argv[i]);
        } else if (arg == "--team-name") {
            if (++i >= argc) {
                return std::unexpected("--team-name requires a value");
            }
            opts.team_name = std::string(argv[i]);
        } else if (arg == "--agent-type") {
            if (++i >= argc) {
                return std::unexpected("--agent-type requires a value");
            }
            opts.agent_type = std::string(argv[i]);
        } else if (arg == "--agent-color") {
            if (++i >= argc) {
                return std::unexpected("--agent-color requires a value");
            }
            opts.agent_color = std::string(argv[i]);
        } else if (arg == "--parent-session-id") {
            if (++i >= argc) {
                return std::unexpected("--parent-session-id requires a value");
            }
            opts.parent_session_id = std::string(argv[i]);
        } else if (arg == "--session-id") {
            if (++i >= argc) {
                return std::unexpected("--session-id requires a value");
            }
            opts.session_id = std::string(argv[i]);
        } else if (arg.starts_with("--session-id=")) {
            opts.session_id = std::string(arg.substr(std::string_view("--session-id=").size()));
        } else if (arg == "--task-id") {
            if (++i >= argc) {
                return std::unexpected("--task-id requires a value");
            }
            opts.task_id = std::string(argv[i]);
        } else if (arg.starts_with("--task-id=")) {
            opts.task_id = std::string(arg.substr(std::string_view("--task-id=").size()));
        } else if (arg == "--task-budget") {
            if (++i >= argc) {
                return std::unexpected("--task-budget requires a value");
            }
            try {
                const auto parsed = std::stoul(argv[i]);
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected("--task-budget must be a positive integer");
                }
                opts.task_budget = static_cast<std::uint32_t>(parsed);
            } catch (...) {
                return std::unexpected("--task-budget must be a positive integer");
            }
        } else if (arg.starts_with("--task-budget=")) {
            try {
                const auto value = std::string(arg.substr(std::string_view("--task-budget=").size()));
                const auto parsed = std::stoul(value);
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected("--task-budget must be a positive integer");
                }
                opts.task_budget = static_cast<std::uint32_t>(parsed);
            } catch (...) {
                return std::unexpected("--task-budget must be a positive integer");
            }
        } else if (arg == "--continue" || arg == "-c") {
            opts.continue_session = true;
        } else if (arg == "--resume" || arg == "-r") {
            if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with('-')) {
                opts.resume_session_id = std::string(argv[++i]);
            } else {
                opts.continue_session = true;
            }
        } else if (arg.starts_with("--resume=")) {
            auto id = std::string(arg.substr(std::string_view("--resume=").size()));
            if (id.empty()) {
                opts.continue_session = true;
            } else {
                opts.resume_session_id = std::move(id);
            }
        } else if (arg == "--session-store") {
            if (++i >= argc) {
                return std::unexpected("--session-store requires a value");
            }
            opts.session_store_path = std::string(argv[i]);
        } else if (arg.starts_with("--session-store=")) {
            opts.session_store_path = std::string(arg.substr(std::string_view("--session-store=").size()));
        } else if (arg == "--server-host") {
            if (++i >= argc) {
                return std::unexpected("--server-host requires a value");
            }
            opts.server_host = std::string(argv[i]);
        } else if (arg == "--server-port") {
            if (++i >= argc) {
                return std::unexpected("--server-port requires a value");
            }
            try {
                const auto parsed = std::stoul(argv[i]);
                if (parsed > 65535) {
                    return std::unexpected("--server-port must be between 0 and 65535");
                }
                opts.server_port = static_cast<uint16_t>(parsed);
            } catch (...) {
                return std::unexpected("--server-port must be a number");
            }
        } else if (arg == "--server-auth-token") {
            if (++i >= argc) {
                return std::unexpected("--server-auth-token requires a value");
            }
            opts.server_auth_token = std::string(argv[i]);
        } else if (arg == "--bridge-work-api-url") {
            if (++i >= argc) {
                return std::unexpected("--bridge-work-api-url requires a value");
            }
            opts.bridge_work_api_url = std::string(argv[i]);
        } else if (arg.starts_with("--bridge-work-api-url=")) {
            opts.bridge_work_api_url = std::string(arg.substr(std::string_view("--bridge-work-api-url=").size()));
        } else if (arg == "--bridge-environment-id") {
            if (++i >= argc) {
                return std::unexpected("--bridge-environment-id requires a value");
            }
            opts.bridge_environment_id = std::string(argv[i]);
        } else if (arg.starts_with("--bridge-environment-id=")) {
            opts.bridge_environment_id = std::string(arg.substr(std::string_view("--bridge-environment-id=").size()));
        } else if (arg == "--bridge-environment-secret") {
            if (++i >= argc) {
                return std::unexpected("--bridge-environment-secret requires a value");
            }
            opts.bridge_environment_secret = std::string(argv[i]);
        } else if (arg.starts_with("--bridge-environment-secret=")) {
            opts.bridge_environment_secret = std::string(arg.substr(std::string_view("--bridge-environment-secret=").size()));
        } else if (arg == "--bridge-access-token") {
            if (++i >= argc) {
                return std::unexpected("--bridge-access-token requires a value");
            }
            opts.bridge_access_token = std::string(argv[i]);
        } else if (arg.starts_with("--bridge-access-token=")) {
            opts.bridge_access_token = std::string(arg.substr(std::string_view("--bridge-access-token=").size()));
        } else if (arg == "--bridge-session-binary") {
            if (++i >= argc) {
                return std::unexpected("--bridge-session-binary requires a value");
            }
            opts.bridge_session_binary = std::string(argv[i]);
        } else if (arg.starts_with("--bridge-session-binary=")) {
            opts.bridge_session_binary = std::string(arg.substr(std::string_view("--bridge-session-binary=").size()));
        } else if (arg == "--bridge-daemon-wait-ms") {
            if (++i >= argc) {
                return std::unexpected("--bridge-daemon-wait-ms requires a value");
            }
            try {
                const auto parsed = std::stoul(argv[i]);
                if (parsed == 0 || parsed > 600000) {
                    return std::unexpected("--bridge-daemon-wait-ms must be between 1 and 600000");
                }
                opts.bridge_daemon_wait_ms = static_cast<int>(parsed);
            } catch (...) {
                return std::unexpected("--bridge-daemon-wait-ms must be a positive integer");
            }
        } else if (arg.starts_with("--bridge-daemon-wait-ms=")) {
            try {
                const auto value = std::string(arg.substr(std::string_view("--bridge-daemon-wait-ms=").size()));
                const auto parsed = std::stoul(value);
                if (parsed == 0 || parsed > 600000) {
                    return std::unexpected("--bridge-daemon-wait-ms must be between 1 and 600000");
                }
                opts.bridge_daemon_wait_ms = static_cast<int>(parsed);
            } catch (...) {
                return std::unexpected("--bridge-daemon-wait-ms must be a positive integer");
            }
        } else if (arg == "--agents") {
            if (++i >= argc) {
                return std::unexpected("--agents requires a JSON object");
            }
            opts.agents_json = std::string(argv[i]);
        } else if (arg.starts_with('-')) {
            return std::unexpected(std::format("Unknown option: {}", arg));
        }
    }

    return opts;
}

void set_env_value(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

void set_env_value_pair(const char* primary, const char* compatible, const std::optional<std::string>& value) {
    if (!value || value->empty()) return;
    set_env_value(primary, *value);
    set_env_value(compatible, *value);
}

void set_env_bool_pair(const char* primary, const char* compatible, bool value) {
    set_env_value(primary, value ? "1" : "0");
    set_env_value(compatible, value ? "1" : "0");
}

void apply_flag_status_line_environment(const cc::config::FlagStatusLineSettings& status_line) {
    const bool has_command = status_line.command && !status_line.command->empty();
    const bool type_allows_command = !status_line.type || *status_line.type == "command";
    const bool enabled = status_line.enabled.value_or(has_command && type_allows_command) &&
        has_command && type_allows_command;

    if (has_command) {
        set_env_value("CC_REPL_STATUS_LINE_COMMAND", *status_line.command);
        set_env_value("CLAUDE_CODE_STATUS_LINE_COMMAND", *status_line.command);
    }
    set_env_bool_pair("CC_REPL_STATUS_LINE_ENABLED", "CLAUDE_CODE_STATUS_LINE_ENABLED", enabled);

    if (status_line.padding) {
        const auto padding = std::to_string(*status_line.padding);
        set_env_value("CC_REPL_STATUS_LINE_PADDING", padding);
        set_env_value("CLAUDE_CODE_STATUS_LINE_PADDING", padding);
    }
}

/// Load and apply a --settings payload (path OR inline JSON) before the rest of
/// startup. Mirrors TS `loadSettingsFromFlag`:
///   - If the value parses as a JSON object, treat it as inline settings.
///   - Otherwise, treat it as a file path and read it.
/// On invalid JSON or unreadable file, prints the TS-faithful error and exits.
/// Returns the parsed result so the caller can override model/apiKey.
std::optional<cc::config::FlagSettingsResult> load_flag_settings(
    const std::string& settings_arg,
    const cc::config::EnvSetter& env_setter,
    std::optional<std::string>& out_model_override,
    std::optional<std::string>& out_api_key_override
) {
    auto trimmed = settings_arg;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
                                 trimmed.front() == '\n' || trimmed.front() == '\r')) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                                 trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }

    const bool looks_like_json = trimmed.starts_with('{') && trimmed.ends_with('}');

    std::string json_text;
    if (looks_like_json) {
        json_text = trimmed;
    } else {
        // Treat as a file path; resolve relative to CWD (TS uses safeResolvePath
        // + readFileSync). An unreadable/missing file is a hard error.
        std::ifstream file(trimmed);
        if (!file.is_open()) {
            std::println(stderr, "Error: Invalid JSON provided to --settings");
            return std::nullopt;
        }
        json_text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    auto parsed = cc::utils::json::parse(json_text);
    if (!parsed || !parsed->root().is_obj()) {
        // Faithful C++ equivalent of the TS error message.
        std::println(stderr, "Error: Invalid JSON provided to --settings");
        return std::nullopt;
    }

    auto result = cc::config::apply_flag_settings(parsed->root(), env_setter);
    if (result.model) out_model_override = result.model;
    if (result.api_key) out_api_key_override = result.api_key;
    return result;
}

void apply_teammate_environment(const CliOptions& opts) {
    set_env_value_pair("CC_REPL_AGENT_ID", "CLAUDE_CODE_AGENT_ID", opts.agent_id);
    set_env_value_pair("CC_REPL_AGENT_NAME", "CLAUDE_CODE_AGENT_NAME", opts.agent_name);
    set_env_value_pair("CC_REPL_TEAM_NAME", "CLAUDE_CODE_TEAM_NAME", opts.team_name);
    set_env_value_pair("CC_REPL_AGENT_TYPE", "CLAUDE_CODE_AGENT_TYPE", opts.agent_type);
    set_env_value_pair("CC_REPL_AGENT_COLOR", "CLAUDE_CODE_AGENT_COLOR", opts.agent_color);
    set_env_value_pair("CC_REPL_PARENT_SESSION_ID", "CLAUDE_CODE_PARENT_SESSION_ID", opts.parent_session_id);
    set_env_value_pair("CC_REPL_SESSION_ID", "CLAUDE_CODE_SESSION_ID", opts.session_id);
    set_env_value_pair("CC_REPL_TASK_ID", "CLAUDE_CODE_TASK_ID", opts.task_id);
    if (opts.plan_mode_required) {
        set_env_value("CC_REPL_PLAN_MODE_REQUIRED", "1");
        set_env_value("CLAUDE_CODE_PLAN_MODE_REQUIRED", "true");
    }
}

std::optional<std::string> parent_permission_mode_from_options(const CliOptions& opts) {
    if (opts.dangerously_skip_permissions) return "bypassPermissions";
    if (opts.permission_mode && !opts.permission_mode->empty()) return opts.permission_mode;
    if (opts.plan_mode_required) return "plan";
    return std::nullopt;
}

std::string tool_result_text(const cc::core::ToolResult& result) {
    std::string out;
    for (const auto& content : result.content) {
        if (!out.empty()) out += '\n';
        out += content.text;
    }
    return out;
}

cc::tools::AgentLivePermissionCheck check_agent_tool_permission(
    cc::hooks::ToolPermissionHook& permission_hook,
    std::string_view tool_name,
    std::string_view input_json,
    std::string_view tool_use_id
) {
    permission_hook.set_current_tool_use_id(tool_use_id);
    auto response = permission_hook.can_use_response(tool_name, input_json);
    permission_hook.clear_current_tool_use_id();

    cc::tools::AgentLivePermissionCheck check;
    check.allowed = response.decision == cc::hooks::PermissionDecision::allow ||
                    response.decision == cc::hooks::PermissionDecision::allow_once;
    check.updated_input_json = std::move(response.updated_input_json);
    check.message = std::move(response.message);
    return check;
}

int run_runtime_tool_once(const CliOptions& opts) {
    if (!opts.runtime_tool_name || opts.runtime_tool_name->empty()) {
        std::println(stderr, "--run-runtime-tool requires a tool name");
        return 1;
    }
    auto tool_registry = cc::core::ToolRegistry{};
    cc::tools::register_runtime_tools(tool_registry, cc::tools::RuntimeToolOptions{
        .parent_permission_mode = parent_permission_mode_from_options(opts),
    });
    auto result = tool_registry.execute(
        *opts.runtime_tool_name,
        cc::core::ToolInput::from_json(opts.runtime_tool_input_json.value_or("{}")));
    if (!result) {
        std::println(stderr, "{}", result.error().message);
        return 1;
    }

    auto text = tool_result_text(*result);
    if (result->is_error) {
        if (!text.empty()) {
            std::println(stderr, "{}", text);
        }
        return 1;
    }
    if (!text.empty()) {
        std::println("{}", text);
    }
    return 0;
}

/**
 * Load engine configuration from environment and defaults
 */
auto load_config() -> cc::core::QueryEngineConfig {
    cc::core::QueryEngineConfig config;

    // API key from environment (required for operation)
    if (const char* key = std::getenv("ANTHROPIC_API_KEY")) {
        config.api_key = key;
    }
    // OAuth/Pro/gateway bearer token (e.g. ANTHROPIC_AUTH_TOKEN supplied via --settings env).
    // When present it is sent as "Authorization: Bearer" and takes precedence over api_key.
    if (const char* token = std::getenv("ANTHROPIC_AUTH_TOKEN")) {
        config.auth_token = token;
    }

    // Base URL override (e.g. for proxies or custom endpoints)
    if (const char* url = std::getenv("ANTHROPIC_BASE_URL")) {
        config.base_url = url;
    }

    config.model_params.model = cc::config::resolve_default_model_from_environment(
        [](std::string_view name) -> std::optional<std::string> {
            const auto key = std::string(name);
            if (const char* value = std::getenv(key.c_str()); value && *value) {
                return std::string(value);
            }
            return std::nullopt;
        });

    config.max_budget_usd = 10.0;
    config.cwd = fs::current_path().string();
    config.model_params.max_tokens = 16384;
    config.retry_policy.max_retries = 3;
    config.context_window.max_context_tokens = 200000;

    return config;
}

std::vector<cc::core::Message> compact_runtime_messages(void* state) {
    auto* engine = static_cast<cc::core::QueryEngine*>(state);
    return engine ? engine->get_conversation() : std::vector<cc::core::Message>{};
}

cc::core::VoidResult compact_runtime_apply(void* state) {
    auto* engine = static_cast<cc::core::QueryEngine*>(state);
    if (!engine) {
        return std::unexpected(cc::core::Error::make(
            cc::core::ErrorCode::InternalError,
            "No active query engine is available for compaction"));
    }
    auto compacted = engine->compact_conversation();
    if (!compacted) {
        return std::unexpected(cc::core::Error::make(
            cc::core::ErrorCode::InternalError,
            compacted.error().format()));
    }
    return cc::core::VoidResult{};
}

cc::core::CommandContext command_context_for_engine(cc::core::QueryEngine* engine) {
    return cc::core::CommandContext{
        .args = {},
        .raw_input = {},
        .cwd = engine ? engine->working_directory() : fs::current_path().string(),
        .runtime_state = engine,
        .compact_message_provider = compact_runtime_messages,
        .compact_applier = compact_runtime_apply,
    };
}

// Global for signal handling
static std::atomic<bool> g_should_exit{false};

void handle_signal(int) {
    g_should_exit.store(true);
}

std::optional<std::string> env_string(const char* name) {
    if (const char* value = std::getenv(name); value && *value) return std::string(value);
    return std::nullopt;
}

std::optional<std::string> bridge_work_id_from_environment() {
    if (auto value = env_string("CLAUDE_CODE_BRIDGE_WORK_ID")) return value;
    return env_string("CC_REPL_BRIDGE_WORK_ID");
}

std::optional<std::string_view> optional_view(const std::optional<std::string>& value) {
    if (!value || value->empty()) return std::nullopt;
    return std::string_view{*value};
}

struct SessionIngressLifecycleGuard {
    bool active = false;
    std::optional<std::string> bridge_work_id;

    ~SessionIngressLifecycleGuard() {
        if (!active) return;
        (void)cc::services::api::send_ingress_lifecycle_event("stopped", optional_view(bridge_work_id));
        cc::services::api::close_ingress();
    }
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string make_headless_message_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::format("msg_{}_{}", now, counter.fetch_add(1));
}

std::string text_from_assistant_message(const cc::core::AssistantMessage& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* tb = std::get_if<cc::core::TextBlock>(&block)) {
            if (!text.empty()) text += '\n';
            text += tb->text;
        }
    }
    return text;
}

std::string headless_session_id(const CliOptions& opts) {
    if (opts.session_id && !opts.session_id->empty()) return *opts.session_id;
    if (opts.resume_session_id && !opts.resume_session_id->empty()) return *opts.resume_session_id;
    if (auto value = env_string("CC_REMOTE_SESSION_ID")) return *value;
    if (auto value = env_string("CLAUDE_CODE_REMOTE_SESSION_ID")) return *value;
    if (auto value = env_string("CC_REPL_SESSION_ID")) return *value;
    if (auto value = env_string("CLAUDE_CODE_SESSION_ID")) return *value;
    return "headless-session";
}

std::string headless_session_id(
    const CliOptions& opts,
    cc::core::ConversationStore* conversation_store
) {
    if (opts.continue_session && conversation_store) {
        if (auto active = conversation_store->active_conversation_id(); active && !active->empty()) {
            return *active;
        }
    }
    return headless_session_id(opts);
}

fs::path conversation_store_path(const CliOptions& opts) {
    if (opts.session_store_path && !opts.session_store_path->empty()) {
        return fs::path{*opts.session_store_path};
    }
    if (auto value = env_string("CC_REPL_SESSION_STORE")) return fs::path{*value};
    if (auto value = env_string("CLAUDE_CODE_SESSION_STORE")) return fs::path{*value};
    if (auto home = env_string("HOME")) {
        return fs::path{*home} / ".config" / "claude" / "cpp-conversations.json";
    }
    return fs::current_path() / ".claude" / "cpp-conversations.json";
}

std::string title_from_messages(const std::vector<cc::core::Message>& messages) {
    for (const auto& message : messages) {
        const auto* user = std::get_if<cc::core::UserMessage>(&message);
        if (!user) continue;
        for (const auto& block : user->content) {
            const auto* text = std::get_if<cc::core::TextBlock>(&block);
            if (!text || text->text.empty()) continue;
            auto title = text->text.substr(0, 50);
            if (text->text.size() > 50) title += "...";
            return title;
        }
    }
    return "Headless Session";
}

cc::core::VoidResult save_engine_conversation(
    cc::core::QueryEngine& engine,
    cc::core::ConversationStore& store,
    std::string_view conversation_id
) {
    auto messages = engine.get_conversation();
    auto* conversation = store.get_or_create_conversation(std::string(conversation_id));
    conversation->clear();
    conversation->set_title(title_from_messages(messages));
    for (auto& message : messages) {
        conversation->add_message(std::move(message));
    }
    return store.save_all();
}

cc::core::VoidResult restore_engine_conversation(
    cc::core::QueryEngine& engine,
    cc::core::ConversationStore& store,
    const CliOptions& opts
) {
    if (!opts.continue_session && !opts.resume_session_id) return {};

    auto loaded = store.load_all();
    if (!loaded) return std::unexpected(loaded.error());
    if (opts.resume_session_id && !store.switch_conversation(*opts.resume_session_id)) {
        return std::unexpected(cc::core::Error::make(
            cc::core::ErrorCode::SessionNotFound,
            std::format("Persisted conversation not found: {}", *opts.resume_session_id)));
    }

    auto* conversation = store.get_active_conversation();
    engine.restore_conversation(conversation->get_messages());
    return {};
}

std::string sdk_user_event(
    std::string_view session_id,
    std::string_view user_text,
    std::string_view message_id
) {
    std::ostringstream out;
    out << R"({"type":"user","message":{"role":"user","content":")"
        << json_escape(user_text)
        << R"("},"parent_tool_use_id":null,"uuid":")" << json_escape(message_id)
        << R"(","session_id":")" << json_escape(session_id) << "\"}";
    return out.str();
}

std::string sdk_assistant_event(
    std::string_view session_id,
    std::string_view assistant_id,
    std::string_view content,
    std::string_view model
) {
    std::ostringstream out;
    out << R"({"type":"assistant","message":{"id":")" << json_escape(assistant_id)
        << R"(","role":"assistant","model":")" << json_escape(model)
        << R"(","content":[{"type":"text","text":")" << json_escape(content)
        << R"("}]},"parent_tool_use_id":null,"uuid":")" << json_escape(assistant_id)
        << R"(","session_id":")" << json_escape(session_id) << "\"}";
    return out.str();
}

std::string sdk_result_event(
    std::string_view session_id,
    std::string_view assistant_id,
    std::string_view content,
    std::string_view model,
    const cc::core::QueryResponse& response
) {
    std::ostringstream out;
    out << R"({"type":"result","subtype":"success","duration_ms":)" << response.elapsed.count()
        << R"(,"duration_api_ms":)" << response.elapsed.count()
        << R"(,"is_error":false,"num_turns":1,"result":")" << json_escape(content)
        << R"(","stop_reason":")" << json_escape(response.message.stop_reason.value_or("end_turn"))
        << R"(","total_cost_usd":0)"
        << R"(,"usage":{"input_tokens":)" << response.total_usage.input_tokens
        << R"(,"output_tokens":)" << response.total_usage.output_tokens
        << R"(,"cache_creation_input_tokens":0,"cache_read_input_tokens":0)"
        << R"(,"server_tool_use":{"web_search_requests":0}})"
        << R"(,"modelUsage":{},"permission_denials":[],"tool_rounds":)" << response.tool_rounds
        << R"(,"model":")" << json_escape(model)
        << R"(","uuid":"result_)" << json_escape(assistant_id)
        << R"(","session_id":")" << json_escape(session_id) << "\"}";
    return out.str();
}

std::string sdk_error_result_event(
    std::string_view session_id,
    std::string_view message_id,
    std::string_view error
) {
    std::ostringstream out;
    out << R"({"type":"result","subtype":"error","duration_ms":0,"duration_api_ms":0)"
        << R"(,"is_error":true,"num_turns":0,"result":")" << json_escape(error)
        << R"(","stop_reason":"error","total_cost_usd":0)"
        << R"(,"usage":{"input_tokens":0,"output_tokens":0,"cache_creation_input_tokens":0,"cache_read_input_tokens":0)"
        << R"(,"server_tool_use":{"web_search_requests":0}})"
        << R"(,"modelUsage":{},"permission_denials":[],"tool_rounds":0)"
        << R"(,"uuid":"result_)" << json_escape(message_id)
        << R"(","session_id":")" << json_escape(session_id) << "\"}";
    return out.str();
}

void write_headless_event(std::string_view event_json) {
    std::cout << event_json << '\n';
    std::cout.flush();
    if (cc::services::api::is_ingress_active()) {
        (void)cc::services::api::send_ingress_message(event_json);
    }
}

std::optional<std::string> text_from_sdk_content(cc::utils::json::JsonVal content) {
    if (content.is_str()) return std::string(content.as_str());
    if (!content.is_arr()) return std::nullopt;

    std::string text;
    content.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            if (!text.empty()) text += '\n';
            text += item.as_str();
            return;
        }
        if (!item.is_obj()) return;
        auto type = item.get("type");
        auto block_text = item.get("text");
        if (block_text.is_str() && (!type.is_str() || type.as_str() == "text")) {
            if (!text.empty()) text += '\n';
            text += block_text.as_str();
        }
    });
    if (text.empty()) return std::nullopt;
    return text;
}

std::optional<std::string> extract_headless_user_text(std::string_view line) {
    auto parsed = cc::utils::json::parse(line);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto type = root.get("type");
    if (!type.is_str()) return std::nullopt;
    const auto type_text = type.as_str();
    if (type_text == "keep_alive" || type_text == "control_response") {
        return std::nullopt;
    }
    if (type_text == "update_environment_variables") {
        auto variables = root.get("variables");
        if (variables.is_obj()) {
            variables.iter_obj([](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
                if (!key.is_str() || !value.is_str()) return;
                set_env_value(std::string(key.as_str()).c_str(), std::string(value.as_str()));
            });
        }
        return std::nullopt;
    }
    if (type_text != "user") return std::nullopt;

    auto message = root.get("message");
    if (!message.is_obj()) return std::nullopt;
    auto role = message.get("role");
    if (role.is_str() && role.as_str() != "user") return std::nullopt;
    return text_from_sdk_content(message.get("content"));
}

bool process_headless_stream_json_line(
    cc::core::QueryEngine& engine,
    const CliOptions& opts,
    std::string_view session_id,
    std::string_view line,
    cc::core::ConversationStore* conversation_store = nullptr
) {
    if (line.empty()) return false;

    auto user_text = extract_headless_user_text(line);
    if (!user_text || user_text->empty()) return false;

    auto user_message_id = make_headless_message_id();
    if (opts.replay_user_messages) {
        write_headless_event(sdk_user_event(session_id, *user_text, user_message_id));
    }

    auto query_result = engine.query(*user_text);
    auto assistant_id = make_headless_message_id();
    if (!query_result) {
        write_headless_event(sdk_error_result_event(session_id, assistant_id, query_result.error().format()));
        if (conversation_store) {
            if (auto saved = save_engine_conversation(engine, *conversation_store, session_id); !saved && opts.debug) {
                std::println(stderr, "Failed to save headless conversation: {}", saved.error().format());
            }
        }
        return true;
    }

    auto content = text_from_assistant_message(query_result->message);
    auto model = query_result->message.model.value_or("unknown");
    write_headless_event(sdk_assistant_event(session_id, assistant_id, content, model));
    write_headless_event(sdk_result_event(session_id, assistant_id, content, model, *query_result));
    if (conversation_store) {
        if (auto saved = save_engine_conversation(engine, *conversation_store, session_id); !saved && opts.debug) {
            std::println(stderr, "Failed to save headless conversation: {}", saved.error().format());
        }
    }
    return true;
}

std::string strip_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::optional<std::string> headless_sse_url_from_sdk_url(std::string_view sdk_url) {
    if (sdk_url.empty()) return std::nullopt;
    std::string url(sdk_url);
    if (url.starts_with("wss://")) {
        url = "https://" + url.substr(6);
    } else if (url.starts_with("ws://")) {
        url = "http://" + url.substr(5);
    }
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return std::nullopt;
    }
    url = strip_trailing_slashes(std::move(url));
    if (url.ends_with("/worker/events/stream")) return url;
    return url + "/worker/events/stream";
}

bool is_headless_websocket_sdk_url(std::string_view sdk_url) {
    return (sdk_url.starts_with("ws://") || sdk_url.starts_with("wss://")) &&
        sdk_url.find("/session_ingress/ws/") != std::string_view::npos;
}

struct HeadlessSsePayload {
    std::string payload_json;
    std::optional<std::string> event_id;
};

std::optional<HeadlessSsePayload> payload_from_headless_sse_event(const cc::utils::SseEvent& event) {
    auto parsed = cc::utils::json::parse(event.data);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    std::optional<std::string> event_id;
    auto event_id_node = root.get("event_id");
    if (event_id_node.is_str()) event_id = std::string(event_id_node.as_str());

    if (event.event == "client_event") {
        auto payload = root.get("payload");
        if (payload.is_obj() && payload.get("type").is_str()) {
            return HeadlessSsePayload{
                .payload_json = payload.to_string(),
                .event_id = std::move(event_id),
            };
        }
        return std::nullopt;
    }

    auto payload = root.get("payload");
    if (payload.is_obj() && payload.get("type").is_str()) {
        return HeadlessSsePayload{
            .payload_json = payload.to_string(),
            .event_id = std::move(event_id),
        };
    }
    if (root.get("type").is_str()) {
        return HeadlessSsePayload{
            .payload_json = root.to_string(),
            .event_id = std::move(event_id),
        };
    }
    return std::nullopt;
}

bool is_headless_remote_auth_failure(std::string_view message) {
    return message.find("401") != std::string_view::npos ||
        message.find("403") != std::string_view::npos ||
        message.find("Unauthorized") != std::string_view::npos ||
        message.find("unauthorized") != std::string_view::npos ||
        message.find("Forbidden") != std::string_view::npos ||
        message.find("forbidden") != std::string_view::npos;
}

int run_headless_stream_json_sse(
    cc::core::QueryEngine& engine,
    const CliOptions& opts,
    std::string_view session_id,
    cc::core::ConversationStore* conversation_store
) {
    auto sse_url = headless_sse_url_from_sdk_url(*opts.sdk_url);
    if (!sse_url) {
        std::println(stderr, "Error: --sdk-url must be http(s), ws, or wss for headless remote stream-json input.");
        return 1;
    }

    std::unordered_map<std::string, std::string> headers{
        {"Accept", "text/event-stream"},
        {"anthropic-version", "2023-06-01"},
    };
    if (auto token = env_string("CLAUDE_CODE_SESSION_ACCESS_TOKEN")) {
        headers["Authorization"] = "Bearer " + *token;
    }
    if (auto version = env_string("CLAUDE_CODE_ENVIRONMENT_RUNNER_VERSION")) {
        headers["x-environment-runner-version"] = *version;
    }

    if (cc::services::api::is_worker_lifecycle_active()) {
        (void)cc::services::api::send_worker_state("idle", true);
        (void)cc::services::api::send_worker_heartbeat();
    }

    cc::utils::HttpConfig http_config;
    http_config.timeout_ms = 1'000;
    cc::utils::HttpClient http(http_config);
    auto last_worker_heartbeat = std::chrono::steady_clock::now();
    std::optional<std::string> last_sse_event_id;
    while (!g_should_exit.load()) {
        if (last_sse_event_id && !last_sse_event_id->empty()) {
            headers["Last-Event-ID"] = *last_sse_event_id;
        } else {
            headers.erase("Last-Event-ID");
        }
        auto streamed = http.stream_sse(
            *sse_url,
            headers,
            [&](const cc::utils::SseEvent& event) {
                if (!event.id.empty()) {
                    last_sse_event_id = event.id;
                }
                auto payload = payload_from_headless_sse_event(event);
                if (!payload) return;
                if (payload->event_id) {
                    (void)cc::services::api::send_worker_delivery(*payload->event_id, "received");
                }
                (void)cc::services::api::send_worker_state("running");
                (void)process_headless_stream_json_line(
                    engine,
                    opts,
                    session_id,
                    payload->payload_json,
                    conversation_store);
                (void)cc::services::api::send_worker_state("idle");
                if (payload->event_id) {
                    (void)cc::services::api::send_worker_delivery(*payload->event_id, "processed");
                }
            });
        if (!streamed) {
            if (is_headless_remote_auth_failure(streamed.error().message)) {
                std::println(stderr, "Error: headless --sdk-url SSE authentication failed: {}", streamed.error().message);
                return 1;
            }
            if (opts.debug) {
                std::println(stderr, "headless --sdk-url SSE stream error: {}", streamed.error().message);
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (cc::services::api::is_worker_lifecycle_active() &&
            now - last_worker_heartbeat >= std::chrono::seconds{20}) {
            (void)cc::services::api::send_worker_heartbeat();
            last_worker_heartbeat = now;
        }
        if (!g_should_exit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
    }
    return 0;
}

int run_headless_stream_json_websocket(
    cc::core::QueryEngine& engine,
    const CliOptions& opts,
    std::string_view session_id,
    cc::core::ConversationStore* conversation_store
) {
    auto token = env_string("CLAUDE_CODE_SESSION_ACCESS_TOKEN");
    std::optional<std::string_view> bearer_token;
    if (token && !token->empty()) bearer_token = std::string_view(*token);

    while (!g_should_exit.load()) {
        cc::cli::WebSocketTransport transport;
        transport.on_message([&](std::string_view message) {
            (void)process_headless_stream_json_line(engine, opts, session_id, message, conversation_store);
        });

        auto connected = transport.connect(*opts.sdk_url, bearer_token);
        if (!connected) {
            if (is_headless_remote_auth_failure(connected.error())) {
                std::println(stderr, "Error: headless --sdk-url WebSocket authentication failed: {}", connected.error());
                return 1;
            }
            if (opts.debug) {
                std::println(stderr, "headless --sdk-url WebSocket connect error: {}", connected.error());
            }
            if (!g_should_exit.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
            }
            continue;
        }

        while (!g_should_exit.load() && transport.is_connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        transport.close();
        if (!g_should_exit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }
    return 0;
}

int run_headless_stream_json(
    cc::core::QueryEngine& engine,
    const CliOptions& opts,
    cc::core::ConversationStore* conversation_store
) {
    const auto session_id = headless_session_id(opts, conversation_store);
    if (opts.sdk_url && !opts.sdk_url->empty()) {
        if (is_headless_websocket_sdk_url(*opts.sdk_url)) {
            return run_headless_stream_json_websocket(engine, opts, session_id, conversation_store);
        }
        return run_headless_stream_json_sse(engine, opts, session_id, conversation_store);
    }

    std::string line;
    while (!g_should_exit.load() && std::getline(std::cin, line)) {
        (void)process_headless_stream_json_line(engine, opts, session_id, line, conversation_store);
    }
    return 0;
}

int run_direct_connect_server(const CliOptions& opts) {
    auto auth_token = opts.server_auth_token;
    if (!auth_token) {
        if (const char* token = std::getenv("CC_REPL_SERVER_AUTH_TOKEN"); token && *token) {
            auth_token = std::string(token);
        }
    }

    auto server = cc::server::HttpServer{};
    auto started = server.start(cc::server::ServerConfig{
        .port = opts.server_port,
        .host = opts.server_host,
        .cors = true,
        .auth_token = auth_token,
    });
    if (!started.has_value()) {
        std::println(stderr, "Failed to start direct-connect server: {}", started.error());
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::println("CC-REPL direct-connect server listening on {}", server.get_url());
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}

std::optional<std::string> option_or_env(
    const std::optional<std::string>& value,
    std::initializer_list<const char*> names
) {
    if (value && !value->empty()) return value;
    for (const auto* name : names) {
        if (auto env = env_string(name)) return env;
    }
    return std::nullopt;
}

std::string bridge_session_binary_path(const CliOptions& opts) {
    if (opts.bridge_session_binary && !opts.bridge_session_binary->empty()) {
        return *opts.bridge_session_binary;
    }
    auto path = fs::path{opts.executable_path};
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return fs::weakly_canonical(path, ec).string();
    }
    return opts.executable_path.empty() ? std::string{"cc-repl"} : opts.executable_path;
}

struct BridgeDaemonSettings {
    std::string work_api_url;
    std::string environment_id;
    std::string environment_secret;
    std::string access_token;
};

std::expected<BridgeDaemonSettings, std::string> bridge_daemon_settings_from_options(
    const CliOptions& opts,
    std::string_view flag_name
) {
    auto work_api_url = option_or_env(opts.bridge_work_api_url, {
        "CC_REPL_BRIDGE_WORK_API_URL",
        "CLAUDE_CODE_BRIDGE_WORK_API_URL",
        "CLAUDE_BRIDGE_BASE_URL",
    });
    auto environment_id = option_or_env(opts.bridge_environment_id, {
        "CC_REPL_BRIDGE_ENVIRONMENT_ID",
        "CLAUDE_CODE_BRIDGE_ENVIRONMENT_ID",
    });
    auto environment_secret = option_or_env(opts.bridge_environment_secret, {
        "CC_REPL_BRIDGE_ENVIRONMENT_SECRET",
        "CLAUDE_CODE_BRIDGE_ENVIRONMENT_SECRET",
    });
    auto access_token = option_or_env(opts.bridge_access_token, {
        "CC_REPL_BRIDGE_ACCESS_TOKEN",
        "CLAUDE_CODE_BRIDGE_ACCESS_TOKEN",
        "CLAUDE_BRIDGE_OAUTH_TOKEN",
    });

    if (!work_api_url || !environment_id || !environment_secret || !access_token) {
        return std::unexpected(std::format(
            "{} requires --bridge-work-api-url, --bridge-environment-id, "
            "--bridge-environment-secret, and --bridge-access-token.",
            flag_name));
    }

    return BridgeDaemonSettings{
        .work_api_url = *work_api_url,
        .environment_id = *environment_id,
        .environment_secret = *environment_secret,
        .access_token = *access_token,
    };
}

cc::daemon::DaemonConfig bridge_daemon_config_from_settings(
    const CliOptions& opts,
    const BridgeDaemonSettings& settings
) {
    return cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = {},
        .port_file = {},
        .poll_interval = std::chrono::seconds{1},
        .heartbeat_interval = std::chrono::seconds{1},
        .max_sessions = 1,
        .work_api_url = settings.work_api_url,
        .bridge_environment_id = settings.environment_id,
        .bridge_environment_secret = settings.environment_secret,
        .bridge_access_token = settings.access_token,
        .bridge_runner_version = std::string(kVersion),
        .trusted_device_token = std::nullopt,
        .session_binary = bridge_session_binary_path(opts),
    };
}

bool daemon_session_has_result(
    cc::daemon::DaemonServer& daemon,
    std::string_view session_id
) {
    for (const auto& line : daemon.session_stdout_lines(session_id)) {
        if (line.find(R"("type":"result")") != std::string::npos) return true;
    }
    return false;
}

std::optional<cc::daemon::DaemonSession> daemon_session_by_id(
    cc::daemon::DaemonServer& daemon,
    std::string_view session_id
) {
    auto sessions = daemon.sessions();
    auto it = std::ranges::find_if(sessions, [&](const auto& session) {
        return session.id == session_id;
    });
    if (it == sessions.end()) return std::nullopt;
    return *it;
}

void request_daemon_child_shutdown(
    cc::daemon::DaemonServer& daemon,
    std::string_view session_id
) {
#ifndef _WIN32
    auto session = daemon_session_by_id(daemon, session_id);
    if (session && session->pid > 0) {
        (void)::kill(session->pid, SIGTERM);
    }
#else
    (void)daemon;
    (void)session_id;
#endif
}

std::size_t running_daemon_session_count(cc::daemon::DaemonServer& daemon) {
    std::size_t running = 0;
    for (const auto& session : daemon.sessions()) {
        if (session.status == "running") ++running;
    }
    return running;
}

void request_all_daemon_children_shutdown(cc::daemon::DaemonServer& daemon) {
    for (const auto& session : daemon.sessions()) {
        if (session.status == "running") {
            request_daemon_child_shutdown(daemon, session.id);
        }
    }
}

void wait_for_daemon_children_to_finish(
    cc::daemon::DaemonServer& daemon,
    std::chrono::milliseconds timeout
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        daemon.reap_sessions();
        if (running_daemon_session_count(daemon) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

int run_bridge_daemon_once(const CliOptions& opts) {
    auto settings = bridge_daemon_settings_from_options(opts, "--bridge-daemon-once");
    if (!settings) {
        std::println(stderr, "Error: {}", settings.error());
        return 1;
    }

    cc::daemon::DaemonServer daemon(bridge_daemon_config_from_settings(opts, *settings));

    auto spawned = daemon.poll_for_work_once();
    if (!spawned) {
        std::println(stderr, "Bridge daemon poll failed: {}", spawned.error());
        return 1;
    }
    if (!*spawned) {
        std::println("No bridge work available.");
        return 0;
    }

    const auto session_id = **spawned;
    std::println("Bridge daemon spawned session {}", session_id);
    (void)daemon.heartbeat_sessions_once();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{opts.bridge_daemon_wait_ms};
    bool saw_result = false;
    while (std::chrono::steady_clock::now() < deadline) {
        daemon.reap_sessions();
        if (daemon_session_has_result(daemon, session_id)) {
            saw_result = true;
            break;
        }
        if (auto session = daemon_session_by_id(daemon, session_id);
            session && session->status != "running") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    if (!saw_result) {
        std::println(stderr, "Bridge daemon session did not emit a result before timeout.");
        for (const auto& line : daemon.session_stdout_lines(session_id)) {
            std::println(stderr, "{}", line);
        }
        request_daemon_child_shutdown(daemon, session_id);
        for (int i = 0; i < 100; ++i) {
            daemon.reap_sessions();
            if (auto session = daemon_session_by_id(daemon, session_id);
                session && session->status != "running") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
        return 1;
    }

    request_daemon_child_shutdown(daemon, session_id);
    std::string final_status = "running";
    for (int i = 0; i < 200; ++i) {
        daemon.reap_sessions();
        if (auto session = daemon_session_by_id(daemon, session_id)) {
            final_status = session->status;
            if (session->status != "running") break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    std::println("Bridge daemon session {} completed with status {}", session_id, final_status);
    return final_status == "completed" ? 0 : 1;
}

int run_bridge_daemon(const CliOptions& opts) {
    auto settings = bridge_daemon_settings_from_options(opts, "--bridge-daemon");
    if (!settings) {
        std::println(stderr, "Error: {}", settings.error());
        return 1;
    }

    g_should_exit.store(false);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    cc::daemon::DaemonServer daemon(bridge_daemon_config_from_settings(opts, *settings));
    auto started = daemon.start();
    if (!started) {
        std::println(stderr, "Failed to start bridge daemon: {}", started.error());
        return 1;
    }

    std::println("CC-REPL bridge daemon listening on 127.0.0.1:{}", *started);
    std::fflush(stdout);

    std::unordered_set<std::string> announced_sessions;
    auto immediate = daemon.poll_for_work_once();
    if (!immediate) {
        std::println(stderr, "Bridge daemon poll failed: {}", immediate.error());
    } else if (*immediate) {
        announced_sessions.insert(**immediate);
        std::println("Bridge daemon spawned session {}", **immediate);
        std::fflush(stdout);
    }

    while (!g_should_exit.load()) {
        daemon.reap_sessions();
        for (const auto& session : daemon.sessions()) {
            if (announced_sessions.insert(session.id).second) {
                std::println("Bridge daemon spawned session {}", session.id);
                std::fflush(stdout);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    request_all_daemon_children_shutdown(daemon);
    wait_for_daemon_children_to_finish(daemon, std::chrono::milliseconds{5000});
    daemon.stop();
    std::println("CC-REPL bridge daemon stopped");
    std::fflush(stdout);
    return 0;
}

/**
 * Simple fallback UI if FTXUI fails
 */
auto run_simple_ui(
    cc::core::QueryEngine* engine,
    cc::commands::AppCommandRegistry& cmd_registry
) -> int {
    std::println("╭─────────────────────────────────────────╮");
    std::println("│      CC-REPL (C++ Migration) v{}        │", kVersion);
    std::println("│  Type /help for available commands      │");
    std::println("│  Type your query and press Enter        │");
    std::println("╰─────────────────────────────────────────╯");

    while (!g_should_exit.load()) {
        std::string input;
        std::print("\n▶ ");
        std::getline(std::cin, input);

        if (input.empty()) continue;

        if (input.starts_with('/')) {
            auto result = cmd_registry.execute(input, command_context_for_engine(engine));
            if (!result) {
                std::println("Error: {}", result.error().message);
                continue;
            }

            if (result->status == cc::core::CommandStatus::Failed) {
                std::println("Error: {}", result->message);
            } else {
                std::println("{}", result->message);
            }

            if (result->metadata == "EXIT") {
                std::println("Goodbye!");
                break;
            }
            continue;
        }

        if (engine == nullptr) {
            std::println("Error: ANTHROPIC_API_KEY is required for model queries. Slash commands remain available.");
            continue;
        }

        std::println("\n🔄 Processing...\n");

        auto query_result = engine->query(input);
        if (!query_result.has_value()) {
            std::println("❌ Error: {}", query_result.error().format());
            continue;
        }

        auto& response = *query_result;
        if (!response.success) {
            std::println("❌ Query failed");
            for (const auto& err : response.errors) {
                std::println("  - {}", err);
            }
            continue;
        }

        // Print response
        std::println("🤖 Assistant:");
        for (const auto& block : response.message.content) {
            if (const auto* text = std::get_if<cc::core::TextBlock>(&block)) {
                std::println("{}", text->text);
            }
        }

        // Print cost info
        auto cost = engine->get_usage();
        std::println("\n📊 Usage: {} in / {} out tokens",
            cost.input_tokens, cost.output_tokens);
    }

    return 0;
}

int main(int argc, const char* argv[]) {
    auto opts_result = parse_args(argc, argv);
    if (!opts_result.has_value()) {
        std::println(stderr, "Error: {}", opts_result.error());
        std::println(stderr, "Run with --help for usage.");
        return 1;
    }
    auto opts = std::move(opts_result.value());

    // Apply --settings EARLY, before --version/--help and every other subsystem.
    // This mirrors the TS eagerLoadSettings() flow: a malformed payload is a
    // hard error that surfaces before any other processing, while a valid
    // payload populates the process env (ANTHROPIC_API_KEY / ANTHROPIC_BASE_URL
    // / ...) and records model/apiKey overrides for later application.
    std::optional<std::string> settings_model_override;
    std::optional<std::string> settings_api_key_override;
    if (opts.settings && !opts.settings->empty()) {
        auto applied = load_flag_settings(
            *opts.settings,
            /*env_setter=*/[](std::string_view name, std::string_view value) {
                set_env_value(std::string(name).c_str(), std::string(value));
            },
            settings_model_override,
            settings_api_key_override);
        if (!applied) {
            // load_flag_settings already printed the faithful error message.
            return 1;
        }
        // `apiKey` from settings mirrors a sibling `env.ANTHROPIC_API_KEY`:
        // apply it to the process env BEFORE load_config() reads it, so the API
        // client picks it up without an explicit `env` block.
        if (settings_api_key_override && !settings_api_key_override->empty()) {
            set_env_value("ANTHROPIC_API_KEY", *settings_api_key_override);
        }
        if (applied->status_line) {
            apply_flag_status_line_environment(*applied->status_line);
        }
        if (opts.debug) {
            for (const auto& key : applied->applied_env_keys) {
                std::println(stderr, "[settings] applied env: {}", key);
            }
            if (applied->model) {
                std::println(stderr, "[settings] model override: {}", *applied->model);
            }
            if (applied->api_key) {
                std::println(stderr, "[settings] apiKey override provided");
            }
            if (applied->status_line) {
                std::println(stderr, "[settings] statusLine override: {}",
                    applied->status_line->command.value_or("<none>"));
            }
            for (const auto& key : applied->deferred_keys) {
                std::println(stderr, "[settings] deferred key (not yet applied): {}", key);
            }
        }
    }

    if (opts.show_version) {
        std::println("cc-repl {}", kVersion);
        return 0;
    }
    if (opts.show_help) {
        print_help();
        return 0;
    }

    if (opts.agents_json && !opts.agents_json->empty()) {
#ifdef _WIN32
        _putenv_s("CC_REPL_AGENTS_JSON", opts.agents_json->c_str());
#else
        setenv("CC_REPL_AGENTS_JSON", opts.agents_json->c_str(), 1);
#endif
    }
    apply_teammate_environment(opts);

    if (opts.list_runtime_commands) {
        auto cmd_registry = cc::commands::AppCommandRegistry{};
        auto names = cmd_registry.command_names();
        std::ranges::sort(names);
        for (const auto& name : names) {
            std::println("{}", name);
        }
        return 0;
    }

    if (opts.list_runtime_tools) {
        auto tool_registry = cc::core::ToolRegistry{};
        cc::tools::register_runtime_tools(tool_registry, cc::tools::RuntimeToolOptions{
            .parent_permission_mode = parent_permission_mode_from_options(opts),
        });
        std::vector<std::string> names;
        for (auto name : tool_registry.tool_names()) {
            names.emplace_back(name);
        }
        std::ranges::sort(names);
        for (const auto& name : names) {
            std::println("{}", name);
        }
        return 0;
    }

    if (opts.runtime_tool_name) {
        return run_runtime_tool_once(opts);
    }

    if (opts.server) {
        return run_direct_connect_server(opts);
    }
    if (opts.bridge_daemon) {
        return run_bridge_daemon(opts);
    }
    if (opts.bridge_daemon_once) {
        return run_bridge_daemon_once(opts);
    }

    auto config = load_config();
    // Priority: explicit --model flag > --settings `model` > env > default.
    // If the user passed --model on the command line, it wins; otherwise let a
    // settings-provided model override the default/env-derived value.
    if (opts.model.has_value()) {
        config.model_params.model = opts.model.value();
    } else if (settings_model_override && !settings_model_override->empty()) {
        config.model_params.model = *settings_model_override;
    }
    if (opts.task_budget) {
        config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
            .total = *opts.task_budget,
            .remaining = std::nullopt,
        };
    }
    config.append_system_prompt = cc::tools::agent_runtime::build_teammate_append_system_prompt(
        std::move(config.append_system_prompt),
        config.cwd ? std::optional<fs::path>{fs::path{*config.cwd}} : std::nullopt);

    // Validate API key is present for model queries. Local slash commands in
    // simple UI mode do not need API access.
    if (config.api_key.empty() && config.auth_token.empty() && opts.use_simple_ui) {
        auto cmd_registry = cc::commands::AppCommandRegistry{};
        return run_simple_ui(nullptr, cmd_registry);
    }
    if (config.api_key.empty() && config.auth_token.empty()) {
        std::println(stderr, "Error: no API credentials found (set ANTHROPIC_API_KEY or ANTHROPIC_AUTH_TOKEN,");
        std::println(stderr, "or provide them via --settings <file>).");
        return 1;
    }

    if (opts.headless) {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
    }

    SessionIngressLifecycleGuard ingress_guard;
    if (auto ingress = cc::services::api::create_ingress_from_environment(); ingress && *ingress) {
        ingress_guard.active = true;
        ingress_guard.bridge_work_id = bridge_work_id_from_environment();
        (void)cc::services::api::send_ingress_lifecycle_event("started", optional_view(ingress_guard.bridge_work_id));
    } else if (!ingress && opts.debug) {
        std::println(stderr, "Session ingress initialization skipped: {}", ingress.error());
    }

    // Initialize permission hook before tools so Agent can reuse the live policy.
    auto permission_hook = cc::hooks::ToolPermissionHook{};
    permission_hook.set_auto_approve(!opts.permissions);
    permission_hook.set_working_dir(config.cwd.value_or(fs::current_path().string()));

    // Initialize tool registry and register all built-in tools
    auto tool_registry = cc::core::ToolRegistry{};
    cc::tools::register_runtime_tools(tool_registry, cc::tools::RuntimeToolOptions{
        .parent_permission_mode = parent_permission_mode_from_options(opts),
        .permission_check = [&permission_hook](
            std::string_view tool_name,
            std::string_view input_json,
            std::string_view tool_use_id
        ) {
            return check_agent_tool_permission(permission_hook, tool_name, input_json, tool_use_id);
        },
        .permission_hook_valid_for_background = true,
    });

    // Populate config.tools with definitions for the API request body
    config.tools = tool_registry.get_visible_definitions();

    // Initialize command registry with all migrated commands
    auto cmd_registry = cc::commands::AppCommandRegistry{};

    // Initialize session storage
    auto storage = cc::utils::SessionStorage{};
    auto conversation_store = cc::core::ConversationStore(conversation_store_path(opts).string());

    // Initialize lifecycle hooks for pre/post tool execution events
    auto lifecycle_hooks = cc::hooks::LifecycleHookRegistry{};

    // Initialize query engine
    auto engine = cc::core::QueryEngine{std::move(config), tool_registry};
    engine.set_permission_hook(&permission_hook);
    engine.set_lifecycle_hooks(&lifecycle_hooks);

    if (auto restored = restore_engine_conversation(engine, conversation_store, opts); !restored) {
        std::println(stderr, "Error: {}", restored.error().format());
        return 1;
    }

    if (opts.headless) {
        if (opts.input_format == "stream-json" || opts.output_format == "stream-json") {
            if (opts.input_format != "stream-json" || opts.output_format != "stream-json") {
                std::println(stderr, "Error: stream-json headless mode requires both --input-format=stream-json and --output-format=stream-json.");
                return 1;
            }
            return run_headless_stream_json(engine, opts, &conversation_store);
        }
        while (!g_should_exit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    }

    // Try to run with full UI, fall back to simple if needed
    try {
        if (opts.use_simple_ui) {
            return run_simple_ui(&engine, cmd_registry);
        } else {
            return cc_ui_run_app_bridge(&engine, &lifecycle_hooks, &cmd_registry, &storage, &permission_hook);
        }
    } catch (const std::exception& e) {
        std::println(stderr, "UI startup failed: {}", e.what());
        std::println(stderr, "Falling back to simple mode...");
        return run_simple_ui(&engine, cmd_registry);
    }
}
