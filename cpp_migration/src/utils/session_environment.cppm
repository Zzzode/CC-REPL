module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.session_environment;

export namespace cc::utils {

namespace fs = std::filesystem;

struct SessionEnvironment {
    fs::path cwd;
    std::map<std::string, std::string> env;
    std::string shell;
    std::string model;
};

namespace detail {
    inline fs::path get_sessions_base_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }
} // namespace detail

// Capture the current environment state
SessionEnvironment capture_environment() {
    SessionEnvironment env;
    env.cwd = fs::current_path();

    // Capture relevant environment variables
    const char* shell = std::getenv("SHELL");
    env.shell = shell ? shell : "/bin/bash";

    const char* model = std::getenv("CLAUDE_MODEL");
    env.model = model ? model : "claude-sonnet-4-20250514";

    // Capture CLAUDE_* and ANTHROPIC_* env vars
    extern char** environ;
    for (char** e = environ; *e != nullptr; ++e) {
        std::string_view entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = entry.substr(0, eq);
        if (key.starts_with("CLAUDE_") || key.starts_with("ANTHROPIC_") || key == "PATH") {
            env.env[std::string(key)] = std::string(entry.substr(eq + 1));
        }
    }

    return env;
}

// Apply a saved environment to the current process
void apply_environment(const SessionEnvironment& env) {
    if (fs::exists(env.cwd)) {
        fs::current_path(env.cwd);
    }
    for (auto& [key, value] : env.env) {
        setenv(key.c_str(), value.c_str(), 1);
    }
}

// Save environment to session directory
void save_environment(std::string_view session_id, const SessionEnvironment& env) {
    auto dir = detail::get_sessions_base_dir() / std::string(session_id);
    fs::create_directories(dir);

    std::ofstream file(dir / "environment.json");
    if (!file.is_open()) return;

    file << "{\n";
    file << "  \"cwd\": \"" << env.cwd.string() << "\",\n";
    file << "  \"shell\": \"" << env.shell << "\",\n";
    file << "  \"model\": \"" << env.model << "\",\n";
    file << "  \"env\": {\n";
    bool first = true;
    for (auto& [k, v] : env.env) {
        if (!first) file << ",\n";
        first = false;
        file << "    \"" << k << "\": \"" << v << "\"";
    }
    file << "\n  }\n}\n";
}

// Load environment from session directory
std::optional<SessionEnvironment> load_environment(std::string_view session_id) {
    auto dir = detail::get_sessions_base_dir() / std::string(session_id);
    auto env_path = dir / "environment.json";

    if (!fs::exists(env_path)) return std::nullopt;

    std::ifstream file(env_path);
    if (!file.is_open()) return std::nullopt;

    SessionEnvironment env;
    std::string line;

    while (std::getline(file, line)) {
        // Simple key extraction
        auto find_value = [&line](std::string_view key) -> std::optional<std::string> {
            auto pos = line.find(key);
            if (pos == std::string::npos) return std::nullopt;
            auto val_start = line.find('"', pos + key.size() + 2);
            auto val_end = line.find('"', val_start + 1);
            if (val_start != std::string::npos && val_end != std::string::npos) {
                return line.substr(val_start + 1, val_end - val_start - 1);
            }
            return std::nullopt;
        };

        if (auto v = find_value("cwd")) env.cwd = *v;
        if (auto v = find_value("shell")) env.shell = *v;
        if (auto v = find_value("model")) env.model = *v;
    }

    return env;
}

} // namespace cc::utils
