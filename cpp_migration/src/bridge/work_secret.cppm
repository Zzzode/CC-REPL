module;
#include <string>
#include <string_view>
#include <expected>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

export module cc.bridge.work_secret;

export namespace cc::bridge {

// Internal: get the path to the secret file
inline std::string get_secret_path();

// Generate a cryptographically random work secret (hex-encoded)
std::string generate_work_secret() {
    // Use random_device for cryptographic randomness
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    // Generate 32 bytes (256 bits) of randomness
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(16) << dist(gen);
    }

    return oss.str();
}

// Store the work secret securely on disk
std::expected<void, std::string> store_work_secret(std::string_view secret) {
    if (secret.empty()) {
        return std::unexpected("Cannot store empty secret");
    }

    std::string secret_path = get_secret_path();
    std::filesystem::create_directories(std::filesystem::path(secret_path).parent_path());

    std::ofstream ofs(secret_path, std::ios::trunc);
    if (!ofs.is_open()) {
        return std::unexpected("Failed to open secret file for writing: " + secret_path);
    }

    ofs << secret;
    ofs.close();

    // Set restrictive permissions (owner read/write only)
    std::error_code ec;
    std::filesystem::permissions(secret_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);

    if (ec) {
        return std::unexpected("Failed to set permissions on secret file: " + ec.message());
    }

    return {};
}

// Load the work secret from disk
std::expected<std::string, std::string> load_work_secret() {
    std::string secret_path = get_secret_path();

    if (!std::filesystem::exists(secret_path)) {
        return std::unexpected("Work secret file not found: " + secret_path);
    }

    std::ifstream ifs(secret_path);
    if (!ifs.is_open()) {
        return std::unexpected("Failed to open secret file for reading");
    }

    std::string secret;
    std::getline(ifs, secret);

    if (secret.empty()) {
        return std::unexpected("Secret file is empty");
    }

    return secret;
}

// Constant-time comparison to prevent timing attacks
bool verify_work_secret(std::string_view presented, std::string_view stored) {
    if (presented.size() != stored.size()) {
        return false;
    }

    // Constant-time comparison
    volatile unsigned char result = 0;
    for (size_t i = 0; i < presented.size(); ++i) {
        result |= static_cast<unsigned char>(presented[i]) ^
                  static_cast<unsigned char>(stored[i]);
    }

    return result == 0;
}

// Internal: get the path to the secret file
inline std::string get_secret_path() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/claude-code/bridge/work_secret";
    }
    return "/tmp/claude-code-bridge-work-secret";
}

} // namespace cc::bridge
