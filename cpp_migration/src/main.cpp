/**
 * CC-REPL Main Entry Point - C++23 Version
 *
 * Bootstraps the CLI application: parses arguments, initializes subsystems,
 * and launches the full interactive FTXUI-based UI.
 */

#include <iostream>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <memory>
#include <expected>
#include <functional>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <print>
#include <variant>

// Import our core modules
import cc.query.query_engine;
import cc.types.types;
import cc.tools.tool;
import cc.tools.runtime_registry;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;
import cc.types.command;
import cc.commands.command;
import cc.commands.registry;
import cc.constants.product;
import cc.utils.session_storage;
import cc.ui.app;

namespace fs = std::filesystem;

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
    bool permissions = false;    // Enable permission checking (disables auto-approve)
    bool list_runtime_tools = false;
    bool list_runtime_commands = false;
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
  --debug              Enable debug logging
  --simple-ui          Use simple text UI (not interactive)
  --permissions        Enable permission checking for tool execution
  --list-runtime-tools Print registered runtime tool names and exit
  --list-runtime-commands
                       Print registered runtime command names and exit

Examples:
  cc-repl                                    # Start interactive mode
  cc-repl --model claude-3-5-sonnet-20241022 # Use specific model
)");
}

/**
 * Parse command-line arguments
 */
auto parse_args(int argc, const char* argv[]) -> std::expected<CliOptions, std::string> {
    CliOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        if (arg == "--version" || arg == "-v") {
            opts.show_version = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.show_help = true;
        } else if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--simple-ui") {
            opts.use_simple_ui = true;
        } else if (arg == "--permissions") {
            opts.permissions = true;
        } else if (arg == "--list-runtime-tools") {
            opts.list_runtime_tools = true;
        } else if (arg == "--list-runtime-commands") {
            opts.list_runtime_commands = true;
        } else if (arg == "--model") {
            if (++i >= argc) {
                return std::unexpected("--model requires a value");
            }
            opts.model = std::string(argv[i]);
        } else if (arg.starts_with('-')) {
            return std::unexpected(std::format("Unknown option: {}", arg));
        }
    }

    return opts;
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

    // Base URL override (e.g. for proxies or custom endpoints)
    if (const char* url = std::getenv("ANTHROPIC_BASE_URL")) {
        config.base_url = url;
    }

    // Model override from environment
    if (const char* model = std::getenv("ANTHROPIC_MODEL")) {
        config.model_params.model = model;
    } else {
        config.model_params.model = "claude-sonnet-4-20250514";
    }

    config.max_budget_usd = 10.0;
    config.cwd = fs::current_path().string();
    config.model_params.max_tokens = 16384;
    config.retry_policy.max_retries = 3;
    config.context_window.max_context_tokens = 200000;

    return config;
}

// Global for signal handling
static std::atomic<bool> g_should_exit{false};

/**
 * Simple fallback UI if FTXUI fails
 */
auto run_simple_ui(
    cc::core::QueryEngine* engine,
    cc::commands::AppCommandRegistry& cmd_registry
) -> int {
    std::println("╭─────────────────────────────────────────╮");
    std::println("│      CC-REPL (C++ Migration) v{}       │", kVersion);
    std::println("│  Type /help for available commands      │");
    std::println("│  Type your query and press Enter        │");
    std::println("╰─────────────────────────────────────────╯");

    while (!g_should_exit.load()) {
        std::string input;
        std::print("\n▶ ");
        std::getline(std::cin, input);

        if (input.empty()) continue;

        if (input.starts_with('/')) {
            auto result = cmd_registry.execute(input, cc::core::CommandContext{});
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

    if (opts.show_version) {
        std::println("cc-repl {}", kVersion);
        return 0;
    }
    if (opts.show_help) {
        print_help();
        return 0;
    }

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
        cc::tools::register_runtime_tools(tool_registry);
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

    auto config = load_config();
    if (opts.model.has_value()) {
        config.model_params.model = opts.model.value();
    }

    // Validate API key is present for model queries. Local slash commands in
    // simple UI mode do not need API access.
    if (config.api_key.empty() && opts.use_simple_ui) {
        auto cmd_registry = cc::commands::AppCommandRegistry{};
        return run_simple_ui(nullptr, cmd_registry);
    }
    if (config.api_key.empty()) {
        std::println(stderr, "Error: ANTHROPIC_API_KEY environment variable is not set.");
        std::println(stderr, "Set it with: export ANTHROPIC_API_KEY=\"your-key-here\"");
        return 1;
    }

    // Initialize tool registry and register all built-in tools
    auto tool_registry = cc::core::ToolRegistry{};
    cc::tools::register_runtime_tools(tool_registry);

    // Populate config.tools with definitions for the API request body
    config.tools = tool_registry.get_visible_definitions();

    // Initialize command registry with all migrated commands
    auto cmd_registry = cc::commands::AppCommandRegistry{};

    // Initialize session storage
    auto storage = cc::utils::SessionStorage{};

    // Initialize permission hook (auto-approve unless --permissions flag is set)
    auto permission_hook = cc::hooks::ToolPermissionHook{};
    permission_hook.set_auto_approve(!opts.permissions);
    permission_hook.set_working_dir(config.cwd.value_or(fs::current_path().string()));

    // Initialize lifecycle hooks for pre/post tool execution events
    auto lifecycle_hooks = cc::hooks::LifecycleHookRegistry{};

    // Initialize query engine
    auto engine = cc::core::QueryEngine{std::move(config), tool_registry};
    engine.set_permission_hook(&permission_hook);
    engine.set_lifecycle_hooks(&lifecycle_hooks);

    // Try to run with full UI, fall back to simple if needed
    try {
        if (opts.use_simple_ui) {
            return run_simple_ui(&engine, cmd_registry);
        } else {
            return cc::ui::RunApp(engine, cmd_registry, storage, &permission_hook);
        }
    } catch (const std::exception& e) {
        std::println(stderr, "UI startup failed: {}", e.what());
        std::println(stderr, "Falling back to simple mode...");
        return run_simple_ui(&engine, cmd_registry);
    }
}
