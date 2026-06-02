module;

#include <string>
#include <optional>
#include <expected>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>

export module cc.utils.auth_utils;

export namespace cc::utils {

namespace fs = std::filesystem;
using namespace std::chrono;

// Authentication token with expiry information
struct AuthToken {
    std::string access_token;
    std::optional<std::string> refresh_token;
    system_clock::time_point expires_at;
};

// Check if a token has expired (with 5-minute buffer for safety)
inline bool is_token_expired(const AuthToken& token) {
    auto now = system_clock::now();
    auto buffer = minutes(5);
    return now >= (token.expires_at - buffer);
}

namespace detail {

// Get the auth token storage path
inline fs::path get_token_path() {
    const char* home = std::getenv("HOME");
    if (!home) return fs::path{};
    return fs::path(home) / ".claude" / "auth_token.json";
}

} // namespace detail

// Load stored authentication token from disk
inline std::optional<AuthToken> load_auth_token() {
    auto path = detail::get_token_path();
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    // Simple JSON-like parsing for the token file
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

    AuthToken token;

    // Extract access_token
    auto extract_field = [&](const std::string& key) -> std::optional<std::string> {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return std::nullopt;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return std::nullopt;
        auto quote_start = content.find('"', colon);
        if (quote_start == std::string::npos) return std::nullopt;
        auto quote_end = content.find('"', quote_start + 1);
        if (quote_end == std::string::npos) return std::nullopt;
        return content.substr(quote_start + 1, quote_end - quote_start - 1);
    };

    auto access = extract_field("access_token");
    if (!access) return std::nullopt;
    token.access_token = *access;

    token.refresh_token = extract_field("refresh_token");

    // Parse expires_at as epoch seconds
    auto expires_str = extract_field("expires_at");
    if (expires_str) {
        try {
            long long epoch = std::stoll(*expires_str);
            token.expires_at = system_clock::time_point(seconds(epoch));
        } catch (...) {
            token.expires_at = system_clock::now() + hours(1); // Default 1 hour
        }
    } else {
        token.expires_at = system_clock::now() + hours(1);
    }

    return token;
}

// Save authentication token to disk
inline std::expected<void, std::string> save_auth_token(const AuthToken& token) {
    auto path = detail::get_token_path();

    // Ensure directory exists
    auto dir = path.parent_path();
    if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return std::unexpected("Cannot create auth directory: " + ec.message());
    }

    // Serialize as simple JSON
    auto epoch = duration_cast<seconds>(token.expires_at.time_since_epoch()).count();

    std::ostringstream json;
    json << "{\n";
    json << "  \"access_token\": \"" << token.access_token << "\",\n";
    if (token.refresh_token) {
        json << "  \"refresh_token\": \"" << *token.refresh_token << "\",\n";
    }
    json << "  \"expires_at\": \"" << epoch << "\"\n";
    json << "}\n";

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("Cannot write auth token file: " + path.string());
    }
    file << json.str();
    file.close();

    // Set restrictive permissions (owner read/write only)
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                   fs::perm_options::replace);

    return {};
}

// Clear stored authentication token
inline bool clear_auth_token() {
    auto path = detail::get_token_path();
    if (!fs::exists(path)) return true;

    std::error_code ec;
    return fs::remove(path, ec);
}

} // namespace cc::utils
