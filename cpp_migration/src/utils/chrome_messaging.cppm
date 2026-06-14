/// @file chrome_messaging.cppm
/// @brief Chrome native messaging host utilities for "Claude in Chrome" integration

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <system_error>

export module cc.utils.chrome_messaging;

export namespace cc::utils {

/// Manifest descriptor for a Chrome native messaging host.
struct NativeHostManifest {
    std::string name;
    std::string description;
    std::string path;
    std::vector<std::string> allowed_origins;
};

[[nodiscard]] inline auto manifest_to_json(const NativeHostManifest& manifest)
    -> std::string;

inline auto write_manifest(const NativeHostManifest& manifest,
                           const std::filesystem::path& path)
    -> std::expected<void, std::string>;

/// Chrome native messaging host registration utilities.
/// Manages the lifecycle of the native messaging host manifest used by
/// "Claude in Chrome" to communicate with the locally installed CLI.
class ChromeNativeHost {
public:
    /// Returns the platform-specific directory where Chrome looks for
    /// native messaging host manifests. On Linux this is
    /// ~/.config/google-chrome/NativeMessagingHosts/.
    [[nodiscard]] static auto manifest_directory() -> std::filesystem::path {
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            return std::filesystem::path(home) /
                   ".config" / "google-chrome" / "NativeMessagingHosts";
        }
        return std::filesystem::path("/tmp") /
               "google-chrome" / "NativeMessagingHosts";
    }

    /// Returns the full path to the manifest JSON file.
    [[nodiscard]] static auto manifest_path() -> std::filesystem::path {
        return manifest_directory() / "com.anthropic.claude_code.json";
    }

    /// Registers the native messaging host by writing the manifest to
    /// Chrome's expected location. Creates the directory if it does not exist.
    auto register_host() -> std::expected<void, std::string> {
        auto dir = manifest_directory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::unexpected(
                std::format("Failed to create manifest directory: {}", ec.message()));
        }

        auto manifest = build_default_manifest();
        auto result = write_manifest(manifest, manifest_path());
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        return {};
    }

    /// Removes the native messaging host manifest, effectively unregistering.
    auto unregister_host() -> std::expected<void, std::string> {
        auto path = manifest_path();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return {};
        }
        std::filesystem::remove(path, ec);
        if (ec) {
            return std::unexpected(
                std::format("Failed to remove manifest: {}", ec.message()));
        }
        return {};
    }

    /// Checks whether the native messaging host manifest is currently registered.
    [[nodiscard]] auto is_registered() const -> bool {
        std::error_code ec;
        return std::filesystem::exists(manifest_path(), ec);
    }

    /// Builds a default manifest with the standard extension origins.
    [[nodiscard]] static auto build_default_manifest() -> NativeHostManifest {
        return NativeHostManifest{
            .name          = "com.anthropic.claude_code",
            .description   = "Claude Code native messaging host for Chrome integration",
            .path          = "/usr/local/bin/claude", // Default install path
            .allowed_origins = {
                "chrome-extension://anthropic.claude.code/",
            },
        };
    }
};

/// MCP server endpoint for Chrome communication.
/// Listens for messages from the Chrome extension via the native messaging
/// protocol and dispatches them through the MCP layer.
class ChromeMcpServer {
public:
    /// Starts the MCP server, opening the communication channel.
    auto start() -> std::expected<void, std::string> {
        if (running_) {
            return std::unexpected("ChromeMcpServer is already running");
        }
        running_ = true;
        return {};
    }

    /// Stops the MCP server and closes the communication channel.
    auto stop() -> std::expected<void, std::string> {
        if (!running_) {
            return std::unexpected("ChromeMcpServer is not running");
        }
        running_ = false;
        return {};
    }

    /// Processes a single incoming message from Chrome and returns the
    /// response payload.
    [[nodiscard]] auto handle_message(std::string_view data) -> std::string {
        // Deferred: wire into the MCP message loop once the full MCP server
        //       infrastructure is in place.
        (void)data;
        return R"({"status":"ok"})";
    }

    /// Returns whether the server is currently active.
    [[nodiscard]] auto is_running() const -> bool {
        return running_;
    }

private:
    bool running_ = false;
};

/// Serialises a NativeHostManifest to the Chrome-format JSON string.
[[nodiscard]] inline auto manifest_to_json(const NativeHostManifest& manifest)
    -> std::string
{
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"name\": \"" << manifest.name << "\",\n";
    oss << "  \"description\": \"" << manifest.description << "\",\n";
    oss << "  \"path\": \"" << manifest.path << "\",\n";
    oss << "  \"type\": \"stdio\",\n";
    oss << "  \"allowed_origins\": [";
    for (size_t i = 0; i < manifest.allowed_origins.size(); ++i) {
        oss << "\"" << manifest.allowed_origins[i] << "\"";
        if (i + 1 < manifest.allowed_origins.size()) {
            oss << ", ";
        }
    }
    oss << "]\n";
    oss << "}\n";
    return oss.str();
}

/// Writes a NativeHostManifest to the given file path.
inline auto write_manifest(const NativeHostManifest& manifest,
                           const std::filesystem::path& path)
    -> std::expected<void, std::string>
{
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        return std::unexpected(
            std::format("Failed to open manifest file for writing: {}",
                        path.string()));
    }
    ofs << manifest_to_json(manifest);
    ofs.close();
    if (ofs.fail()) {
        return std::unexpected("Failed to write manifest file");
    }
    return {};
}

/// Reads and parses a NativeHostManifest from the given file path.
/// Returns a basic parse of the manifest fields. Full JSON parsing
/// depends on cc.utils.json integration.
inline auto read_manifest(const std::filesystem::path& path)
    -> std::expected<NativeHostManifest, std::string>
{
    if (!std::filesystem::exists(path)) {
        return std::unexpected("Manifest file does not exist");
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return std::unexpected("Failed to open manifest file for reading");
    }
    std::ostringstream content;
    content << ifs.rdbuf();

    // Minimal field extraction for the manifest format.
    // Production code should delegate to cc.utils.json.
    auto extract_string = [](std::string_view json,
                             std::string_view key) -> std::optional<std::string> {
        auto search = std::format("\"{}\"", key);
        auto pos = json.find(search);
        if (pos == std::string::npos) return std::nullopt;
        auto colon = json.find(':', pos + search.size());
        if (colon == std::string::npos) return std::nullopt;
        auto open_quote = json.find('"', colon + 1);
        if (open_quote == std::string::npos) return std::nullopt;
        auto close_quote = json.find('"', open_quote + 1);
        if (close_quote == std::string::npos) return std::nullopt;
        return std::string(json.substr(open_quote + 1,
                                       close_quote - open_quote - 1));
    };

    NativeHostManifest manifest;
    auto str = content.str();

    if (auto name = extract_string(str, "name")) {
        manifest.name = *name;
    }
    if (auto desc = extract_string(str, "description")) {
        manifest.description = *desc;
    }
    if (auto p = extract_string(str, "path")) {
        manifest.path = *p;
    }

    return manifest;
}

} // namespace cc::utils
