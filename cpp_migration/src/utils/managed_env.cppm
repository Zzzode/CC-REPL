module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

export module cc.utils.managed_env;

export namespace cc::utils {

namespace fs = std::filesystem;

struct ManagedEnvConfig {
    std::map<std::string, std::string> vars;
    std::string source;
};

// Load managed environment from a .env-style file
std::optional<ManagedEnvConfig> load_managed_env() {
    // Look for managed env file in standard locations
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;

    fs::path managed_path = fs::path(home) / ".config" / "claude-code" / "managed.env";
    if (!fs::exists(managed_path)) {
        // Try alternative path
        managed_path = fs::path(home) / ".claude" / "managed.env";
        if (!fs::exists(managed_path)) return std::nullopt;
    }

    std::ifstream file(managed_path);
    if (!file.is_open()) return std::nullopt;

    ManagedEnvConfig config;
    config.source = managed_path.string();

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Strip surrounding quotes from value
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        config.vars[key] = value;
    }

    if (config.vars.empty()) return std::nullopt;
    return config;
}

// Apply managed environment variables to current process
void apply_managed_env(const ManagedEnvConfig& config) {
    for (auto& [key, value] : config.vars) {
        setenv(key.c_str(), value.c_str(), 0); // Don't override existing vars
    }
}

// Check if running in a managed environment (MDM, enterprise, etc.)
bool is_managed_environment() {
    // Check common managed environment indicators
    if (std::getenv("CLAUDE_MANAGED") != nullptr) return true;
    if (std::getenv("CLAUDE_ENTERPRISE") != nullptr) return true;

    auto config = load_managed_env();
    return config.has_value();
}

} // namespace cc::utils
