module;

#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <array>

export module cc.utils.auth_portable;
import cc.utils.bash_execution;

export namespace cc::utils {

namespace fs = std::filesystem;

namespace detail {

// Get the API key file path
inline fs::path get_api_key_file() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) / ".claude" / "api_key";
}

// Try to read API key from system keychain (macOS)
inline std::optional<std::string> read_from_keychain() {
#ifdef __APPLE__
    std::string cmd = "security find-generic-password -s 'claude-code' -a 'api_key' -w 2>/dev/null";
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) return std::nullopt;

    std::array<char, 256> buffer{};
    std::string result;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
    }
    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0 || result.empty()) return std::nullopt;

    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result.empty() ? std::nullopt : std::optional(result);
#else
    return std::nullopt;
#endif
}

// Store API key in system keychain (macOS)
inline bool write_to_keychain(std::string_view key) {
#ifdef __APPLE__
    // Shell-quote the key before splicing into the command string so a value
    // containing a single quote (or any other shell metacharacter) cannot
    // break out of the -w argument. Standard POSIX single-quote escaping.
    std::string cmd = "security add-generic-password -U -s 'claude-code' -a 'api_key' -w "
                    + cc::utils::bash::escape_shell_arg(key) + " 2>/dev/null";
    return system(cmd.c_str()) == 0;
#else
    (void)key;
    return false;
#endif
}

} // namespace detail

// Get the API key from the highest-priority source available
// Priority: ANTHROPIC_API_KEY env > ~/.claude/api_key file > system keychain
inline std::optional<std::string> get_api_key() {
    // 1. Environment variable (highest priority)
    if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
        std::string key(env_key);
        if (!key.empty()) return key;
    }

    // 2. File-based storage
    auto key_file = detail::get_api_key_file();
    if (!key_file.empty() && fs::exists(key_file)) {
        std::ifstream file(key_file);
        if (file.is_open()) {
            std::string key;
            std::getline(file, key);
            // Trim whitespace
            while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' '))
                key.pop_back();
            if (!key.empty()) return key;
        }
    }

    // 3. System keychain (lowest priority)
    return detail::read_from_keychain();
}

// Set the API key (stores in file by default, with keychain as backup)
inline std::expected<void, std::string> set_api_key(std::string_view key) {
    auto key_file = detail::get_api_key_file();
    if (key_file.empty()) {
        return std::unexpected("Cannot determine API key storage path");
    }

    // Ensure directory exists
    auto dir = key_file.parent_path();
    if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return std::unexpected("Cannot create directory: " + ec.message());
        // Set directory to 0700
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
    }

    // Write to file with restricted permissions
    std::ofstream file(key_file, std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("Cannot write API key file: " + key_file.string());
    }
    file << key;
    file.close();

    // Set file permissions to 0600
    fs::permissions(key_file, fs::perms::owner_read | fs::perms::owner_write,
                   fs::perm_options::replace);

    // Also try to store in keychain as backup
    detail::write_to_keychain(key);

    return {};
}

// Get a description of where the API key was found
inline std::string get_auth_source() {
    if (const char* env = std::getenv("ANTHROPIC_API_KEY")) {
        if (std::string_view(env).size() > 0) {
            return "environment variable (ANTHROPIC_API_KEY)";
        }
    }

    auto key_file = detail::get_api_key_file();
    if (!key_file.empty() && fs::exists(key_file)) {
        return "file (" + key_file.string() + ")";
    }

    if (detail::read_from_keychain()) {
        return "system keychain";
    }

    return "not found";
}

} // namespace cc::utils
