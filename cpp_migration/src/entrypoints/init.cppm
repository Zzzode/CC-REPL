/// @file init.cppm
/// @brief CLI initialization and entrypoint logic.
/// Migrated from src/entrypoints/init.ts, cli.tsx
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <filesystem>

export module cc.entrypoints.init;

export namespace cc::entrypoints {

/// CLI launch options parsed from argv
struct LaunchOptions {
    std::optional<std::string> prompt;           // -p/--prompt
    std::optional<std::string> model;            // --model
    std::optional<std::filesystem::path> cwd;    // --cwd
    bool no_input = false;                       // --no-input
    bool verbose = false;                        // --verbose
    bool dangerously_skip_permissions = false;   // --dangerously-skip-permissions
    std::vector<std::string> allowed_tools;      // --allowedTools
    std::vector<std::string> disallowed_tools;   // --disallowedTools
    std::optional<std::string> system_prompt;    // --system-prompt
    std::optional<std::string> append_system_prompt;  // --append-system-prompt
    std::optional<std::string> permission_mode;  // --permission-mode
    bool resume = false;                         // --resume
    std::optional<std::string> session_id;       // --session-id
    bool mcp = false;                            // --mcp
    bool print_version = false;                  // --version
    bool print_help = false;                     // --help
};

/// Parse CLI arguments into LaunchOptions
[[nodiscard]] inline LaunchOptions parse_launch_options(int argc, const char* argv[]) {
    LaunchOptions opts;
    
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        
        if (arg == "--version" || arg == "-v") {
            opts.print_version = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.print_help = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--no-input") {
            opts.no_input = true;
        } else if (arg == "--mcp") {
            opts.mcp = true;
        } else if (arg == "--resume") {
            opts.resume = true;
        } else if (arg == "--dangerously-skip-permissions") {
            opts.dangerously_skip_permissions = true;
        } else if ((arg == "-p" || arg == "--prompt") && i + 1 < argc) {
            opts.prompt = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            opts.model = argv[++i];
        } else if (arg == "--cwd" && i + 1 < argc) {
            opts.cwd = argv[++i];
        } else if (arg == "--session-id" && i + 1 < argc) {
            opts.session_id = argv[++i];
        } else if (arg == "--system-prompt" && i + 1 < argc) {
            opts.system_prompt = argv[++i];
        } else if (arg == "--append-system-prompt" && i + 1 < argc) {
            opts.append_system_prompt = argv[++i];
        } else if (arg == "--permission-mode" && i + 1 < argc) {
            opts.permission_mode = argv[++i];
        } else if (arg == "--allowedTools" && i + 1 < argc) {
            opts.allowed_tools.emplace_back(argv[++i]);
        } else if (arg == "--disallowedTools" && i + 1 < argc) {
            opts.disallowed_tools.emplace_back(argv[++i]);
        }
    }
    
    return opts;
}

/// Fast-path check: should we skip full initialization?
[[nodiscard]] inline bool is_fast_path(const LaunchOptions& opts) {
    return opts.print_version || opts.print_help;
}

} // namespace cc::entrypoints
