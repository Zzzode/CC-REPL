module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <cstdlib>
#include <fstream>
#include <filesystem>

export module cc.cli.handlers.auth;

export namespace cc::cli::handlers {

std::expected<void, std::string> login_interactive();
std::expected<void, std::string> login_with_key(std::string_view key);
std::expected<void, std::string> logout();
std::string show_auth_status();
std::string get_config_directory();

// Handle the 'auth' CLI command (login, logout, status)
std::expected<std::string, std::string> handle_auth_command(std::span<std::string> args) {
    if (args.empty()) {
        return std::string(show_auth_status());
    }

    std::string subcommand(args[0]);

    if (subcommand == "login") {
        if (args.size() >= 2 && args[1] == "--key" && args.size() >= 3) {
            auto result = login_with_key(args[2]);
            if (result.has_value()) {
                return std::string("Successfully authenticated with API key.");
            }
            return std::unexpected(result.error());
        }
        // Interactive login
        auto result = login_interactive();
        if (result.has_value()) {
            return std::string("Successfully authenticated.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "logout") {
        auto result = logout();
        if (result.has_value()) {
            return std::string("Successfully logged out.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "status") {
        return std::string(show_auth_status());
    }

    return std::unexpected("Unknown auth subcommand: " + subcommand + ". Available: login, logout, status");
}

// Interactive login flow reads environment credentials in non-interactive native mode.
std::expected<void, std::string> login_interactive() {
    std::string config_dir = get_config_directory();
    if (config_dir.empty()) {
        return std::unexpected("Cannot determine config directory");
    }

    if (const char* token = std::getenv("ANTHROPIC_AUTH_TOKEN")) {
        if (*token == '\0') return std::unexpected("ANTHROPIC_AUTH_TOKEN is empty");
        std::filesystem::create_directories(config_dir);
        std::ofstream ofs(config_dir + "/credentials.json");
        if (!ofs.is_open()) return std::unexpected("Failed to write credentials file");
        ofs << "{\"auth_token\":\"" << token << "\",\"type\":\"oauth\"}";
        return {};
    }
    return std::unexpected("No interactive OAuth callback is available. Set ANTHROPIC_AUTH_TOKEN or pass auth login --key <key>.");
}

// Login with a pre-existing API key
std::expected<void, std::string> login_with_key(std::string_view key) {
    if (key.empty()) {
        return std::unexpected("API key cannot be empty");
    }

    // Validate key format (Anthropic keys start with "sk-ant-")
    std::string key_str(key);
    if (key_str.substr(0, 7) != "sk-ant-" && key_str.substr(0, 3) != "sk-") {
        return std::unexpected("Invalid API key format");
    }

    std::string config_dir = get_config_directory();
    std::filesystem::create_directories(config_dir);

    std::string credentials_path = config_dir + "/credentials.json";
    std::ofstream ofs(credentials_path);
    if (!ofs.is_open()) {
        return std::unexpected("Failed to write credentials file");
    }

    ofs << "{\"api_key\":\"" << key_str << "\",\"type\":\"api_key\"}";
    ofs.close();

    // Set restrictive permissions
    std::filesystem::permissions(credentials_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    return {};
}

// Logout and clear stored credentials
std::expected<void, std::string> logout() {
    std::string config_dir = get_config_directory();
    std::string credentials_path = config_dir + "/credentials.json";

    if (std::filesystem::exists(credentials_path)) {
        std::error_code ec;
        std::filesystem::remove(credentials_path, ec);
        if (ec) {
            return std::unexpected("Failed to remove credentials: " + ec.message());
        }
    }

    return {};
}

// Show current authentication status
std::string show_auth_status() {
    std::string config_dir = get_config_directory();
    std::string credentials_path = config_dir + "/credentials.json";

    if (!std::filesystem::exists(credentials_path)) {
        return "Not authenticated. Run 'auth login' to authenticate.";
    }

    // Read credentials to show summary (never show the full key)
    std::ifstream ifs(credentials_path);
    if (!ifs.is_open()) {
        return "Authentication status: unknown (cannot read credentials)";
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    // Simple check for key presence
    if (content.find("api_key") != std::string::npos) {
        return "Authenticated with API key (sk-****)";
    }
    if (content.find("oauth") != std::string::npos) {
        return "Authenticated via OAuth";
    }

    return "Authentication status: configured";
}

// Get the configuration directory path
std::string get_config_directory() {
    // Follow XDG Base Directory spec on Unix
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] != '\0') {
        return std::string(xdg_config) + "/cc-repl";
    }

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/cc-repl";
    }

    return "";
}

} // namespace cc::cli::handlers
