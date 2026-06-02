module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <map>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <optional>
#include <chrono>
#include <csignal>

export module cc.cli.handlers.mcp_handler;

export namespace cc::cli::handlers {

// MCP server transport type
enum class McpTransport {
    Stdio,
    Sse,
    Unknown
};

// Configuration for a single MCP server
struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    McpTransport transport{McpTransport::Stdio};
    std::optional<std::string> url; // For SSE transport
};

// Runtime state of an MCP server
struct McpServerState {
    std::string name;
    McpTransport transport;
    bool running{false};
    int pid{0};
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::string> error;
};

namespace detail {

inline std::filesystem::path get_mcp_config_path() {
    // Check project-local config first
    auto cwd = std::filesystem::current_path();
    auto dir = cwd;
    while (true) {
        auto config = dir / ".claude" / "mcp_servers.json";
        if (std::filesystem::exists(config)) return config;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    // Global config
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "claude-code" / "mcp_servers.json";
    }
    return std::filesystem::temp_directory_path() / "claude-code-mcp.json";
}

inline std::string exec_command(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

/// Simple JSON string extraction
inline std::string json_extract(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return {};
    // Skip whitespace
    auto val_start = colon + 1;
    while (val_start < json.size() && json[val_start] == ' ') ++val_start;
    if (val_start >= json.size()) return {};
    if (json[val_start] == '"') {
        auto q2 = json.find('"', val_start + 1);
        if (q2 == std::string::npos) return {};
        return json.substr(val_start + 1, q2 - val_start - 1);
    }
    // Non-string value
    auto end = json.find_first_of(",}\n", val_start);
    if (end == std::string::npos) end = json.size();
    return json.substr(val_start, end - val_start);
}

/// Parse MCP server configurations from the config file
inline std::vector<McpServerConfig> load_config() {
    std::vector<McpServerConfig> configs;
    auto config_path = get_mcp_config_path();
    if (!std::filesystem::exists(config_path)) return configs;

    std::ifstream ifs(config_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());

    // Parse JSON object where each key is a server name
    // Simple parser: find "name": { "command": "...", "args": [...] } blocks
    size_t pos = 0;
    while (pos < content.size()) {
        // Find next key
        auto q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        auto q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string name = content.substr(q1 + 1, q2 - q1 - 1);

        // Skip past the colon to the value object
        auto brace = content.find('{', q2);
        if (brace == std::string::npos) break;

        // Find matching closing brace (simple depth counting)
        int depth = 1;
        size_t obj_start = brace + 1;
        size_t obj_end = brace + 1;
        while (obj_end < content.size() && depth > 0) {
            if (content[obj_end] == '{') ++depth;
            else if (content[obj_end] == '}') --depth;
            if (depth > 0) ++obj_end;
        }

        if (depth == 0) {
            std::string obj = content.substr(obj_start, obj_end - obj_start);

            McpServerConfig cfg;
            cfg.name = name;
            cfg.command = json_extract(obj, "command");

            // Parse transport
            std::string transport_str = json_extract(obj, "transport");
            if (transport_str == "sse") {
                cfg.transport = McpTransport::Sse;
                cfg.url = json_extract(obj, "url");
            } else {
                cfg.transport = McpTransport::Stdio;
            }

            // Parse args (simple: find array between [ and ])
            auto arr_start = obj.find('[');
            auto arr_end = obj.find(']', arr_start);
            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string arr = obj.substr(arr_start + 1, arr_end - arr_start - 1);
                size_t a_pos = 0;
                while (a_pos < arr.size()) {
                    auto aq1 = arr.find('"', a_pos);
                    if (aq1 == std::string::npos) break;
                    auto aq2 = arr.find('"', aq1 + 1);
                    if (aq2 == std::string::npos) break;
                    cfg.args.push_back(arr.substr(aq1 + 1, aq2 - aq1 - 1));
                    a_pos = aq2 + 1;
                }
            }

            if (!cfg.command.empty() || cfg.url) {
                configs.push_back(std::move(cfg));
            }
        }

        pos = obj_end + 1;
    }

    return configs;
}

/// Check if a process is running by PID
inline bool is_process_running(int pid) {
    if (pid <= 0) return false;
    #ifndef _WIN32
    return kill(pid, 0) == 0;
    #else
    return false;
    #endif
}

/// PID file management
inline std::filesystem::path get_pid_file(const std::string& server_name) {
    const char* home = std::getenv("HOME");
    std::filesystem::path dir;
    if (home) {
        dir = std::filesystem::path(home) / ".config" / "claude-code" / "mcp-pids";
    } else {
        dir = std::filesystem::temp_directory_path() / "claude-code-mcp-pids";
    }
    std::filesystem::create_directories(dir);
    return dir / (server_name + ".pid");
}

inline void write_pid_file(const std::string& server_name, int pid) {
    auto path = get_pid_file(server_name);
    std::ofstream ofs(path);
    ofs << pid;
}

inline int read_pid_file(const std::string& server_name) {
    auto path = get_pid_file(server_name);
    if (!std::filesystem::exists(path)) return 0;
    std::ifstream ifs(path);
    int pid = 0;
    ifs >> pid;
    return pid;
}

inline void remove_pid_file(const std::string& server_name) {
    auto path = get_pid_file(server_name);
    std::filesystem::remove(path);
}

} // namespace detail

// Forward declarations
std::expected<void, std::string> start_mcp_server(std::string_view name);
std::expected<void, std::string> stop_mcp_server(std::string_view name);
std::string list_mcp_servers();

// Handle MCP (Model Context Protocol) server management commands
std::expected<std::string, std::string> handle_mcp_command(std::span<std::string> args) {
    if (args.empty()) {
        return std::string(list_mcp_servers());
    }

    std::string subcommand(args[0]);

    if (subcommand == "list" || subcommand == "ls") {
        return std::string(list_mcp_servers());
    }

    if (subcommand == "start") {
        if (args.size() < 2) {
            return std::unexpected("Usage: mcp start <server_name>");
        }
        auto result = start_mcp_server(args[1]);
        if (result.has_value()) {
            return std::string("MCP server '" + std::string(args[1]) + "' started.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "stop") {
        if (args.size() < 2) {
            return std::unexpected("Usage: mcp stop <server_name>");
        }
        auto result = stop_mcp_server(args[1]);
        if (result.has_value()) {
            return std::string("MCP server '" + std::string(args[1]) + "' stopped.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "restart") {
        if (args.size() < 2) {
            return std::unexpected("Usage: mcp restart <server_name>");
        }
        stop_mcp_server(args[1]); // Ignore errors on stop
        auto result = start_mcp_server(args[1]);
        if (result.has_value()) {
            return std::string("MCP server '" + std::string(args[1]) + "' restarted.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "config") {
        auto config_path = detail::get_mcp_config_path();
        return std::string("MCP config path: " + config_path.string());
    }

    return std::unexpected("Unknown MCP subcommand: " + subcommand +
        "\nAvailable: list, start, stop, restart, config");
}

// Start an MCP server by name (from configuration)
std::expected<void, std::string> start_mcp_server(std::string_view name) {
    if (name.empty()) {
        return std::unexpected("Server name cannot be empty");
    }

    // Load configuration
    auto configs = detail::load_config();
    std::string name_str(name);

    // Find the server config
    const McpServerConfig* found = nullptr;
    for (const auto& cfg : configs) {
        if (cfg.name == name_str) {
            found = &cfg;
            break;
        }
    }
    if (!found) {
        return std::unexpected("MCP server '" + name_str + "' not found in configuration.\n"
            "Config path: " + detail::get_mcp_config_path().string());
    }

    // Check if already running
    int existing_pid = detail::read_pid_file(name_str);
    if (existing_pid > 0 && detail::is_process_running(existing_pid)) {
        return std::unexpected("MCP server '" + name_str + "' is already running (PID " +
            std::to_string(existing_pid) + ")");
    }

    // Build the command to spawn
    std::string cmd = found->command;
    for (const auto& arg : found->args) {
        cmd += " " + arg;
    }

    // Build environment string
    std::string env_prefix;
    for (const auto& [key, value] : found->env) {
        env_prefix += key + "=" + value + " ";
    }

    // Launch the process in the background
    std::string full_cmd = env_prefix + cmd + " >/dev/null 2>&1 & echo $!";
    std::string pid_output = detail::exec_command(full_cmd);

    if (pid_output.empty()) {
        return std::unexpected("Failed to start MCP server '" + name_str + "': no PID returned");
    }

    int pid = 0;
    try { pid = std::stoi(pid_output); } catch (...) {
        return std::unexpected("Failed to parse PID for MCP server '" + name_str + "'");
    }

    if (pid <= 0) {
        return std::unexpected("Failed to start MCP server '" + name_str + "'");
    }

    // Verify the process is actually running
    #ifndef _WIN32
    // Brief sleep to let the process initialize
    struct timespec ts = {0, 100000000}; // 100ms
    nanosleep(&ts, nullptr);
    #endif

    if (!detail::is_process_running(pid)) {
        return std::unexpected("MCP server '" + name_str +
            "' exited immediately after starting. Check the command: " + cmd);
    }

    detail::write_pid_file(name_str, pid);
    return {};
}

// Stop a running MCP server
std::expected<void, std::string> stop_mcp_server(std::string_view name) {
    if (name.empty()) {
        return std::unexpected("Server name cannot be empty");
    }

    std::string name_str(name);
    int pid = detail::read_pid_file(name_str);

    if (pid <= 0) {
        return std::unexpected("MCP server '" + name_str + "' is not running (no PID file)");
    }

    if (!detail::is_process_running(pid)) {
        detail::remove_pid_file(name_str);
        return std::unexpected("MCP server '" + name_str +
            "' is not running (stale PID " + std::to_string(pid) + ")");
    }

    // Send SIGTERM for graceful shutdown
    #ifndef _WIN32
    kill(pid, SIGTERM);

    // Wait up to 5 seconds for graceful shutdown
    for (int i = 0; i < 50; ++i) {
        struct timespec ts = {0, 100000000}; // 100ms
        nanosleep(&ts, nullptr);
        if (!detail::is_process_running(pid)) {
            detail::remove_pid_file(name_str);
            return {};
        }
    }

    // Force kill if still running
    kill(pid, SIGKILL);
    #endif

    detail::remove_pid_file(name_str);
    return {};
}

// List all configured and running MCP servers
std::string list_mcp_servers() {
    auto configs = detail::load_config();

    if (configs.empty()) {
        return "MCP Servers: (none configured)\n\n"
               "To add a server, create or edit:\n"
               "  " + detail::get_mcp_config_path().string() + "\n\n"
               "Example config:\n"
               "{\n"
               "  \"filesystem\": {\n"
               "    \"command\": \"npx\",\n"
               "    \"args\": [\"-y\", \"@modelcontextprotocol/server-filesystem\", \"/\"]\n"
               "  }\n"
               "}\n";
    }

    std::string output = "MCP Servers:\n";
    for (const auto& cfg : configs) {
        int pid = detail::read_pid_file(cfg.name);
        bool running = (pid > 0) && detail::is_process_running(pid);

        output += "  " + cfg.name;
        if (running) {
            output += " [running, PID " + std::to_string(pid) + "]";
        } else {
            output += " [stopped]";
        }
        output += "\n";
        output += "    Command: " + cfg.command;
        for (const auto& arg : cfg.args) {
            output += " " + arg;
        }
        output += "\n";
        if (cfg.transport == McpTransport::Sse && cfg.url) {
            output += "    Transport: SSE (" + *cfg.url + ")\n";
        } else {
            output += "    Transport: stdio\n";
        }
    }

    return output;
}

} // namespace cc::cli::handlers
