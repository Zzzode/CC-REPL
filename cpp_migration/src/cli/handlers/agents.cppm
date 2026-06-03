module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <csignal>

export module cc.cli.handlers.agents;

export namespace cc::cli::handlers {

// Agent status information
struct AgentInfo {
    std::string id;
    std::string name;
    std::string status;
    int pid = 0;
};

std::vector<AgentInfo> list_running_agents();
std::expected<void, std::string> stop_agent(std::string_view id);

// Handle the 'agents' CLI command with subcommands (list, stop, etc.)
std::expected<std::string, std::string> handle_agents_command(std::span<std::string> args) {
    if (args.empty()) {
        // Default: list all running agents
        auto agents = list_running_agents();
        if (agents.empty()) {
            return std::string("No agents currently running.");
        }

        std::string output = "Running agents:\n";
        for (const auto& agent : agents) {
            output += "  " + agent.id + " | " + agent.name + " | " + agent.status + "\n";
        }
        return output;
    }

    std::string subcommand(args[0]);

    if (subcommand == "list") {
        auto agents = list_running_agents();
        if (agents.empty()) {
            return std::string("No agents currently running.");
        }

        std::string output = "Running agents:\n";
        for (const auto& agent : agents) {
            output += "  " + agent.id + " | " + agent.name + " | " + agent.status + "\n";
        }
        return output;
    }

    if (subcommand == "stop") {
        if (args.size() < 2) {
            return std::unexpected("Usage: agents stop <agent_id>");
        }
        auto result = stop_agent(args[1]);
        if (result.has_value()) {
            return std::string("Agent " + std::string(args[1]) + " stopped.");
        }
        return std::unexpected(result.error());
    }

    return std::unexpected("Unknown subcommand: " + subcommand + ". Available: list, stop");
}

/// Get the PID file directory for agents
inline std::filesystem::path get_pid_dir() {
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path(home) / ".cc-repl" / "agents";
}

/// Check if a process with the given PID is alive
inline bool is_process_alive(int pid) {
    // kill with signal 0 checks existence without sending a signal
    return ::kill(pid, 0) == 0;
}

// List all currently running agents by scanning PID files
std::vector<AgentInfo> list_running_agents() {
    namespace fs = std::filesystem;
    std::vector<AgentInfo> agents;

    auto pid_dir = get_pid_dir();
    if (!fs::exists(pid_dir)) {
        return agents;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(pid_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".pid") continue;

        // Read PID file: first line is PID, second line is agent name
        std::ifstream ifs(path);
        if (!ifs) continue;

        std::string pid_str, name;
        std::getline(ifs, pid_str);
        std::getline(ifs, name);

        int pid = 0;
        try { pid = std::stoi(pid_str); } catch (...) { continue; }

        if (pid <= 0) continue;

        // Check if process is still alive
        if (!is_process_alive(pid)) {
            // Stale PID file — remove it
            fs::remove(path, ec);
            continue;
        }

        AgentInfo info;
        info.id = path.stem().string();
        info.name = name.empty() ? "agent" : name;
        info.pid = pid;
        info.status = "running";
        agents.push_back(std::move(info));
    }

    return agents;
}

// Stop a running agent by its ID — sends SIGTERM then verifies
std::expected<void, std::string> stop_agent(std::string_view id) {
    if (id.empty()) {
        return std::unexpected("Agent ID cannot be empty");
    }

    auto agents = list_running_agents();
    for (const auto& agent : agents) {
        if (agent.id == id) {
            // Send SIGTERM to gracefully stop the agent
            if (::kill(agent.pid, SIGTERM) != 0) {
                return std::unexpected("Failed to send SIGTERM to agent PID " +
                                       std::to_string(agent.pid));
            }

            // Remove the PID file
            namespace fs = std::filesystem;
            std::error_code ec;
            auto pid_file = get_pid_dir() / (std::string(id) + ".pid");
            fs::remove(pid_file, ec);

            return {};
        }
    }

    return std::unexpected("Agent not found: " + std::string(id));
}

} // namespace cc::cli::handlers
