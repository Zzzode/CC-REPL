module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <httplib.h>

export module cc.cli.update;

namespace cc::cli::detail {

struct ParsedUrl {
    std::string scheme;
    std::string authority;
    std::string target;
};

std::expected<ParsedUrl, std::string> parse_http_url(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected("Download URL must include a scheme");
    }

    ParsedUrl parsed;
    parsed.scheme = std::string(url.substr(0, scheme_end));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        return std::unexpected("Unsupported download URL scheme: " + parsed.scheme);
    }

    auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
        parsed.authority = std::string(url.substr(authority_start));
        parsed.target = "/";
    } else {
        parsed.authority = std::string(url.substr(authority_start, path_start - authority_start));
        parsed.target = std::string(url.substr(path_start));
    }

    if (parsed.authority.empty()) {
        return std::unexpected("Download URL host cannot be empty");
    }
    return parsed;
}

std::filesystem::path make_download_path(std::string_view url) {
    auto hash = std::hash<std::string_view>{}(url);
    return std::filesystem::temp_directory_path() /
        ("claude-code-update-" + std::to_string(hash));
}

std::expected<std::filesystem::path, std::string> copy_file_url(std::string_view url) {
    auto source = std::filesystem::path(std::string(url.substr(std::string_view("file://").size())));
    if (!std::filesystem::exists(source)) {
        return std::unexpected("Update source file does not exist: " + source.string());
    }
    if (!std::filesystem::is_regular_file(source)) {
        return std::unexpected("Update source is not a regular file: " + source.string());
    }

    auto download_path = make_download_path(url);
    std::filesystem::copy_file(source, download_path, std::filesystem::copy_options::overwrite_existing);
    if (std::filesystem::file_size(download_path) == 0) {
        std::filesystem::remove(download_path);
        return std::unexpected("Downloaded update is empty");
    }
    return download_path;
}

std::expected<std::filesystem::path, std::string> download_http_url(std::string_view url) {
    auto parsed = parse_http_url(url);
    if (!parsed) return std::unexpected(parsed.error());

    httplib::Client client(parsed->scheme + "://" + parsed->authority);
    client.set_follow_location(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);

    auto response = client.Get(parsed->target);
    if (!response) {
        return std::unexpected("Failed to download update: HTTP request failed");
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected("Failed to download update: HTTP status " +
            std::to_string(response->status));
    }
    if (response->body.empty()) {
        return std::unexpected("Downloaded update is empty");
    }

    auto download_path = make_download_path(url);
    std::ofstream output(download_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return std::unexpected("Failed to create download file at " + download_path.string());
    }
    output.write(response->body.data(), static_cast<std::streamsize>(response->body.size()));
    if (!output.good()) {
        std::filesystem::remove(download_path);
        return std::unexpected("Failed to write download file at " + download_path.string());
    }
    return download_path;
}

/// Compare two semver strings. Returns <0 if a<b, 0 if equal, >0 if a>b.
int compare_semver(const std::string& a, const std::string& b) {
    auto parse_parts = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::string_view sv(v);
        // Strip leading 'v' if present
        if (!sv.empty() && sv[0] == 'v') sv.remove_prefix(1);
        size_t pos = 0;
        while (pos < sv.size()) {
            auto dot = sv.find('.', pos);
            if (dot == std::string_view::npos) dot = sv.size();
            auto segment = sv.substr(pos, dot - pos);
            // Stop at hyphen (pre-release)
            auto hyphen = segment.find('-');
            if (hyphen != std::string_view::npos) segment = segment.substr(0, hyphen);
            int val = 0;
            for (char c : segment) {
                if (c >= '0' && c <= '9') val = val * 10 + (c - '0');
                else break;
            }
            parts.push_back(val);
            pos = dot + 1;
        }
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    };

    auto pa = parse_parts(a);
    auto pb = parse_parts(b);

    for (size_t i = 0; i < 3; ++i) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

/// Simple JSON value extraction from a flat JSON object
std::string extract_json_string(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return {};
    auto q1 = json.find('"', colon);
    if (q1 == std::string::npos) return {};
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

/// Get the timestamp of last update check
std::optional<std::chrono::system_clock::time_point> get_last_check_time() {
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;
    auto path = std::filesystem::path(home) / ".config" / "claude-code" / ".last-update-check";
    if (!std::filesystem::exists(path)) return std::nullopt;
    std::ifstream ifs(path);
    long long ts = 0;
    ifs >> ts;
    if (ts == 0) return std::nullopt;
    return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(ts));
}

/// Record the current time as last update check
void record_check_time() {
    const char* home = std::getenv("HOME");
    if (!home) return;
    auto dir = std::filesystem::path(home) / ".config" / "claude-code";
    std::filesystem::create_directories(dir);
    auto path = dir / ".last-update-check";
    std::ofstream ofs(path);
    auto now = std::chrono::system_clock::now();
    ofs << std::chrono::system_clock::to_time_t(now);
}

} // namespace cc::cli::detail

export namespace cc::cli {

// Update information when a new version is available
struct UpdateInfo {
    std::string current;
    std::string latest;
    std::string url;
    std::string changelog;
    bool is_security_update{false};
    bool is_breaking{false};
};

std::string get_current_version();
std::string get_update_channel();
std::string get_config_path();
std::string get_manifest_url(const std::string& channel);

// Check if a newer version is available
std::optional<UpdateInfo> check_for_update() {
    // Rate-limit checks to once per hour
    auto last_check = detail::get_last_check_time();
    if (last_check) {
        auto elapsed = std::chrono::system_clock::now() - *last_check;
        if (elapsed < std::chrono::hours(1)) {
            return std::nullopt; // Too soon to check again
        }
    }

    std::string current_version = get_current_version();
    std::string channel = get_update_channel();
    std::string manifest_url = get_manifest_url(channel);

    // Fetch the update manifest
    auto parsed = detail::parse_http_url(manifest_url);
    if (!parsed) return std::nullopt;

    httplib::Client client(parsed->scheme + "://" + parsed->authority);
    client.set_follow_location(true);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);

    auto response = client.Get(parsed->target);
    if (!response || response->status != 200 || response->body.empty()) {
        return std::nullopt; // Silently fail — updates are best-effort
    }

    detail::record_check_time();

    // Parse manifest JSON for version info
    std::string latest_version = detail::extract_json_string(response->body, "version");
    std::string download_url = detail::extract_json_string(response->body, "url");
    std::string changelog = detail::extract_json_string(response->body, "changelog");
    std::string security = detail::extract_json_string(response->body, "security");
    std::string breaking = detail::extract_json_string(response->body, "breaking");

    if (latest_version.empty()) return std::nullopt;

    // Compare versions
    if (detail::compare_semver(latest_version, current_version) <= 0) {
        return std::nullopt; // Already up to date
    }

    return UpdateInfo{
        .current = current_version,
        .latest = latest_version,
        .url = download_url.empty() ? manifest_url : download_url,
        .changelog = changelog,
        .is_security_update = (security == "true"),
        .is_breaking = (breaking == "true")
    };
}

// Download an update from the given URL to a temporary location
std::expected<std::filesystem::path, std::string> download_update(std::string_view url) {
    if (url.empty()) {
        return std::unexpected("Download URL cannot be empty");
    }

    if (url.starts_with("file://")) {
        return detail::copy_file_url(url);
    }
    return detail::download_http_url(url);
}

// Apply a downloaded update by replacing the current binary
std::expected<void, std::string> apply_update(std::filesystem::path update_file) {
    namespace fs = std::filesystem;

    if (!fs::exists(update_file)) {
        return std::unexpected("Update file does not exist: " + update_file.string());
    }

    auto file_size = fs::file_size(update_file);
    if (file_size == 0) {
        return std::unexpected("Update file is empty");
    }

    // Determine the path of the currently running binary
    #ifdef __APPLE__
    // On macOS, use _NSGetExecutablePath or /proc/self/exe equivalent
    std::array<char, 4096> path_buf{};
    uint32_t buf_size = path_buf.size();
    std::string current_binary;
    // Use /proc/self/exe on Linux, realpath on argv[0] as fallback
    if (fs::exists("/proc/self/exe")) {
        current_binary = fs::read_symlink("/proc/self/exe").string();
    } else {
        // Fallback: check common installation paths
        const char* home = std::getenv("HOME");
        if (home) {
            auto local_bin = fs::path(home) / ".local" / "bin" / "claude-code";
            if (fs::exists(local_bin)) {
                current_binary = local_bin.string();
            }
        }
        if (current_binary.empty()) {
            current_binary = "/usr/local/bin/claude-code";
        }
    }
    #elif __linux__
    std::string current_binary;
    if (fs::exists("/proc/self/exe")) {
        current_binary = fs::read_symlink("/proc/self/exe").string();
    } else {
        current_binary = "/usr/local/bin/claude-code";
    }
    #else
    std::string current_binary = "claude-code.exe";
    #endif

    if (current_binary.empty()) {
        return std::unexpected("Could not determine current binary path");
    }

    // Backup the current binary
    auto backup_path = fs::path(current_binary + ".bak");
    std::error_code ec;
    if (fs::exists(current_binary)) {
        fs::copy_file(current_binary, backup_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("Failed to backup current binary: " + ec.message());
        }
    }

    // Replace with new binary
    fs::copy_file(update_file, current_binary, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Attempt to restore backup
        if (fs::exists(backup_path)) {
            fs::copy_file(backup_path, current_binary, fs::copy_options::overwrite_existing);
        }
        return std::unexpected("Failed to replace binary: " + ec.message());
    }

    // Set executable permissions (Unix only)
    #ifndef _WIN32
    fs::permissions(current_binary,
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
        fs::perm_options::add, ec);
    #endif

    // Clean up
    fs::remove(backup_path, ec);
    fs::remove(update_file, ec);

    return {};
}

// Get the current update channel
std::string get_update_channel() {
    // Check environment variable first
    const char* channel_env = std::getenv("CLAUDE_UPDATE_CHANNEL");
    if (channel_env && channel_env[0] != '\0') {
        std::string channel(channel_env);
        if (channel == "stable" || channel == "beta" || channel == "nightly") {
            return channel;
        }
    }

    // Check config file
    std::string config_path = get_config_path();
    if (std::filesystem::exists(config_path)) {
        std::ifstream ifs(config_path);
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find("update_channel") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string value = line.substr(pos + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '"')) {
                        value.erase(value.begin());
                    }
                    while (!value.empty() && (value.back() == ' ' || value.back() == '"' ||
                                              value.back() == ',')) {
                        value.pop_back();
                    }
                    if (value == "stable" || value == "beta" || value == "nightly") {
                        return value;
                    }
                }
            }
        }
    }

    return "stable";
}

// Set the update channel preference
void set_update_channel(std::string_view channel) {
    std::string channel_str(channel);
    if (channel_str != "stable" && channel_str != "beta" && channel_str != "nightly") {
        return;
    }

    std::string config_path = get_config_path();
    std::filesystem::create_directories(std::filesystem::path(config_path).parent_path());

    // Read existing config if present, update the channel field
    std::string existing_content;
    if (std::filesystem::exists(config_path)) {
        std::ifstream ifs(config_path);
        existing_content = std::string(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
    }

    // Simple JSON write/merge
    if (existing_content.empty() || existing_content.find('{') == std::string::npos) {
        std::ofstream ofs(config_path);
        ofs << "{\"update_channel\":\"" << channel_str << "\"}\n";
    } else {
        // Replace existing update_channel value
        auto pos = existing_content.find("\"update_channel\"");
        if (pos != std::string::npos) {
            auto q1 = existing_content.find('"', existing_content.find(':', pos));
            auto q2 = existing_content.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                existing_content.replace(q1 + 1, q2 - q1 - 1, channel_str);
            }
        } else {
            // Insert before closing brace
            auto brace = existing_content.rfind('}');
            if (brace != std::string::npos) {
                std::string insert = ",\"update_channel\":\"" + channel_str + "\"";
                existing_content.insert(brace, insert);
            }
        }
        std::ofstream ofs(config_path);
        ofs << existing_content;
    }
}

// Internal helpers
inline std::string get_current_version() {
    // Compiled-in version constant
    return "1.0.0";
}

inline std::string get_config_path() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/claude-code/update.json";
    }
    #ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "/claude-code/update.json";
    }
    #endif
    return "/tmp/claude-code-update.json";
}

inline std::string get_manifest_url(const std::string& channel) {
    if (channel == "beta") {
        return "https://updates.anthropic.com/claude-code/beta/manifest.json";
    }
    if (channel == "nightly") {
        return "https://updates.anthropic.com/claude-code/nightly/manifest.json";
    }
    return "https://updates.anthropic.com/claude-code/stable/manifest.json";
}

// Format a user-facing update notification message
inline std::string format_update_notification(const UpdateInfo& info) {
    std::string msg = "A new version of Claude Code is available: v" + info.latest +
        " (current: v" + info.current + ")\n";
    if (info.is_security_update) {
        msg += "  [SECURITY] This update contains security fixes.\n";
    }
    if (info.is_breaking) {
        msg += "  [BREAKING] This update contains breaking changes.\n";
    }
    if (!info.changelog.empty()) {
        msg += "  Changelog: " + info.changelog + "\n";
    }
    msg += "  Run 'claude-code upgrade' to update.\n";
    return msg;
}

} // namespace cc::cli
