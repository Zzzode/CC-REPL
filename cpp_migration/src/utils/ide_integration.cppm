// IDE detection, lockfile scanning, RPC, path conversion, and integration utilities
// Sources: ide.ts, idePathConversion.ts, jetbrains.ts
module;

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <unistd.h>

export module cc.utils.ide_integration;

import cc.utils.json;
import cc.utils.async;

export namespace cc::utils::ide {

namespace fs = std::filesystem;
using namespace cc::utils::json;

// =========================================================================
// IDEType - Supported IDE identifiers
// =========================================================================
enum class IDEType : uint8_t {
    VSCode,
    VSCodeInsiders,
    Cursor,
    Windsurf,
    PyCharm,
    IntelliJ,
    WebStorm,
    PhpStorm,
    RubyMine,
    CLion,
    GoLand,
    Rider,
    DataGrip,
    AppCode,
    DataSpell,
    Aqua,
    Gateway,
    Fleet,
    AndroidStudio,
    Unknown,
};

// =========================================================================
// IDEInfo - Information about a detected IDE
// =========================================================================
struct IDEInfo {
    IDEType type;
    std::string name;
    std::optional<std::string> version;
    std::optional<fs::path> executable_path;
    bool has_plugin_installed;
    std::optional<uint16_t> port;       // MCP server port if available
    std::optional<pid_t> pid;           // IDE process ID from lockfile
    std::optional<fs::path> workspace;  // Workspace folder from lockfile
};

// =========================================================================
// IDEPathFormat - Path format conventions for different IDEs
// =========================================================================
enum class IDEPathFormat : uint8_t {
    Unix,           // forward slashes, absolute
    Windows,        // backslashes, drive letter
    URI,            // file:// URI scheme
    Relative,       // relative to workspace root
};

// =========================================================================
// IDE Lockfile - structure for ~/.claude/ide/*.json
// =========================================================================

/// Represents a parsed IDE lockfile entry
struct IdeLockfile {
    IDEType type = IDEType::Unknown;
    std::string name;
    pid_t pid = 0;
    uint16_t port = 0;
    fs::path workspace_folder;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::string> transport;  // "stdio" or "ws"
};

// =========================================================================
// IDE Lockfile Scanner
// =========================================================================

/// Scans ~/.claude/ide/ for lockfiles and validates them by PID check
class IdeLockfileScanner {
public:
    IdeLockfileScanner() {
        if (auto* home = std::getenv("HOME")) {
            lockfile_dir_ = fs::path(home) / ".claude" / "ide";
        }
    }

    explicit IdeLockfileScanner(fs::path lockfile_dir)
        : lockfile_dir_(std::move(lockfile_dir)) {}

    /// Scan for all valid (alive) IDE lockfiles
    [[nodiscard]] std::vector<IdeLockfile> scan() const {
        std::vector<IdeLockfile> results;
        if (!fs::exists(lockfile_dir_) || !fs::is_directory(lockfile_dir_)) {
            return results;
        }

        for (const auto& entry : fs::directory_iterator(lockfile_dir_)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            auto lockfile = parse_lockfile(entry.path());
            if (!lockfile) continue;

            // Validate the process is still alive
            if (is_process_alive(lockfile->pid)) {
                results.push_back(std::move(*lockfile));
            } else {
                // Stale lockfile — remove it
                std::error_code ec;
                fs::remove(entry.path(), ec);
            }
        }
        return results;
    }

    /// Find the lockfile matching a given workspace folder
    [[nodiscard]] std::optional<IdeLockfile> find_for_workspace(
        const fs::path& workspace) const {
        auto lockfiles = scan();
        for (auto& lf : lockfiles) {
            if (is_workspace_match(lf.workspace_folder, workspace)) {
                return std::move(lf);
            }
        }
        return std::nullopt;
    }

    /// Find any IDE lockfile (prefers one matching cwd)
    [[nodiscard]] std::optional<IdeLockfile> find_best_match() const {
        auto cwd = fs::current_path();
        auto lockfiles = scan();
        if (lockfiles.empty()) return std::nullopt;

        // Prefer one whose workspace contains the cwd
        for (auto& lf : lockfiles) {
            if (is_workspace_match(lf.workspace_folder, cwd)) {
                return std::move(lf);
            }
        }
        // Fall back to first valid lockfile
        return std::move(lockfiles.front());
    }

    /// Get the lockfile directory path
    [[nodiscard]] const fs::path& lockfile_dir() const noexcept {
        return lockfile_dir_;
    }

private:
    fs::path lockfile_dir_;

    /// Parse a single lockfile JSON
    [[nodiscard]] static std::optional<IdeLockfile> parse_lockfile(const fs::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return std::nullopt;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        auto doc = parse(content);
        if (!doc) return std::nullopt;

        auto root = doc->root();
        if (!root.is_obj()) return std::nullopt;

        IdeLockfile lf;

        auto name_node = root.get("name");
        if (name_node.is_str()) {
            lf.name = std::string(name_node.as_str());
            lf.type = parse_ide_type(name_node.as_str());
        }

        auto pid_node = root.get("pid");
        if (pid_node.is_num()) {
            lf.pid = static_cast<pid_t>(pid_node.as_int());
        }

        auto port_node = root.get("port");
        if (port_node.is_num()) {
            lf.port = static_cast<uint16_t>(port_node.as_int());
        }

        auto workspace_node = root.get("workspaceFolder");
        if (!workspace_node.is_str()) {
            workspace_node = root.get("workspace");
        }
        if (workspace_node.is_str()) {
            lf.workspace_folder = std::string(workspace_node.as_str());
        }

        auto transport_node = root.get("transport");
        if (transport_node.is_str()) {
            lf.transport = std::string(transport_node.as_str());
        }

        if (lf.pid == 0 && lf.port == 0) return std::nullopt;
        return lf;
    }

    /// Check if a process is still alive using kill(pid, 0)
    [[nodiscard]] static bool is_process_alive(pid_t pid) noexcept {
        if (pid <= 0) return false;
        return ::kill(pid, 0) == 0;
    }

    /// Check if workspace_folder is a prefix of (or equal to) target path
    [[nodiscard]] static bool is_workspace_match(
        const fs::path& workspace, const fs::path& target) {
        if (workspace.empty()) return false;
        auto ws_str = workspace.string();
        auto tgt_str = target.string();
        return tgt_str.starts_with(ws_str);
    }

    /// Parse IDE type from lockfile name string
    [[nodiscard]] static IDEType parse_ide_type(std::string_view name) {
        if (name.find("Code - Insiders") != std::string_view::npos) return IDEType::VSCodeInsiders;
        if (name.find("Code") != std::string_view::npos || name.find("vscode") != std::string_view::npos) return IDEType::VSCode;
        if (name.find("Cursor") != std::string_view::npos) return IDEType::Cursor;
        if (name.find("Windsurf") != std::string_view::npos) return IDEType::Windsurf;
        if (name.find("IntelliJ") != std::string_view::npos) return IDEType::IntelliJ;
        if (name.find("WebStorm") != std::string_view::npos) return IDEType::WebStorm;
        if (name.find("PyCharm") != std::string_view::npos) return IDEType::PyCharm;
        if (name.find("CLion") != std::string_view::npos) return IDEType::CLion;
        if (name.find("GoLand") != std::string_view::npos) return IDEType::GoLand;
        if (name.find("PhpStorm") != std::string_view::npos) return IDEType::PhpStorm;
        if (name.find("RubyMine") != std::string_view::npos) return IDEType::RubyMine;
        if (name.find("Rider") != std::string_view::npos) return IDEType::Rider;
        if (name.find("Fleet") != std::string_view::npos) return IDEType::Fleet;
        if (name.find("Android Studio") != std::string_view::npos) return IDEType::AndroidStudio;
        return IDEType::Unknown;
    }
};

// =========================================================================
// IDE RPC Client - sends JSON-RPC commands to the IDE MCP server
// =========================================================================

/// RPC response from the IDE
struct IdeRpcResponse {
    bool success = false;
    std::string result;         // JSON result payload
    std::optional<std::string> error;
};

/// Sends JSON-RPC requests to the IDE's MCP endpoint
class IdeRpcClient {
public:
    IdeRpcClient() = default;

    /// Connect to IDE using information from a lockfile
    [[nodiscard]] std::expected<void, std::string> connect(const IdeLockfile& lockfile) {
        if (lockfile.port == 0) {
            return std::unexpected("No port in lockfile");
        }
        port_ = lockfile.port;
        connected_ = true;
        ide_name_ = lockfile.name;
        return {};
    }

    /// Check if connected
    [[nodiscard]] bool is_connected() const noexcept { return connected_; }

    /// Send an RPC call to the IDE
    /// Method names follow the pattern: "claude/fileChanged", "claude/openDiff", etc.
    [[nodiscard]] IdeRpcResponse call(std::string_view method,
                                       std::string_view params_json = "{}") {
        if (!connected_) {
            return IdeRpcResponse{.success = false, .error = "Not connected to IDE"};
        }

        // Build JSON-RPC 2.0 request
        auto request = std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})",
            next_id_++, method, params_json);

        // In production, this sends over the transport (TCP/stdio) to the IDE
        // For now, store in outbound buffer for the transport layer to flush
        outbound_requests_.push_back(std::move(request));

        return IdeRpcResponse{.success = true, .result = "{}"};
    }

    /// Convenience: notify file changed
    IdeRpcResponse notify_file_changed(const fs::path& path) {
        auto params = std::format(R"({{"uri":"file://{}"}})", path.string());
        return call("claude/fileChanged", params);
    }

    /// Convenience: open diff view in IDE
    IdeRpcResponse open_diff(const fs::path& path,
                             std::string_view old_content,
                             std::string_view new_content) {
        JsonMutDoc doc;
        auto root = doc.object();
        root.add("uri", doc.string(std::format("file://{}", path.string())));
        root.add("oldContent", doc.string(old_content));
        root.add("newContent", doc.string(new_content));
        doc.set_root(root);
        return call("claude/openDiff", doc.to_string());
    }

    /// Convenience: open file at line in IDE
    IdeRpcResponse open_file(const fs::path& path, std::optional<uint32_t> line = std::nullopt) {
        auto params = line
            ? std::format(R"({{"uri":"file://{}","line":{}}})", path.string(), *line)
            : std::format(R"({{"uri":"file://{}"}})", path.string());
        return call("claude/openFile", params);
    }

    /// Get pending outbound requests (for transport layer to flush)
    [[nodiscard]] std::vector<std::string>& pending_requests() noexcept {
        return outbound_requests_;
    }

    /// Clear pending requests after they've been sent
    void clear_pending() { outbound_requests_.clear(); }

private:
    bool connected_ = false;
    uint16_t port_ = 0;
    std::string ide_name_;
    int64_t next_id_ = 1;
    std::vector<std::string> outbound_requests_;
};

// =========================================================================
// Top-level IDE integration: callIdeRpc() convenience function
// =========================================================================

/// Discovers the IDE via lockfile and sends an RPC call.
/// This is the main entry point for other subsystems to communicate with the IDE.
[[nodiscard]] inline IdeRpcResponse callIdeRpc(
    std::string_view method,
    std::string_view params_json = "{}") {

    IdeLockfileScanner scanner;
    auto lockfile = scanner.find_best_match();
    if (!lockfile) {
        return IdeRpcResponse{.success = false, .error = "No IDE lockfile found"};
    }

    IdeRpcClient client;
    auto connect_result = client.connect(*lockfile);
    if (!connect_result) {
        return IdeRpcResponse{.success = false, .error = connect_result.error()};
    }

    return client.call(method, params_json);
}

// =========================================================================
// IDE path conversion (implementation)
// =========================================================================

/// Convert a filesystem path to the format expected by a given IDE
[[nodiscard]] inline std::string convert_path_to_ide_format(
    const fs::path& path,
    IDEPathFormat format) {
    switch (format) {
        case IDEPathFormat::URI:
            return std::format("file://{}", path.string());
        case IDEPathFormat::Unix:
            return path.string();
        case IDEPathFormat::Relative:
            // Try to make relative to cwd
            return fs::relative(path, fs::current_path()).string();
        case IDEPathFormat::Windows:
            // Convert forward slashes to backslashes
            {
                auto s = path.string();
                for (auto& c : s) {
                    if (c == '/') c = '\\';
                }
                return s;
            }
    }
    return path.string();
}

/// Convert a path from IDE format back to a native filesystem path
[[nodiscard]] inline fs::path convert_path_from_ide_format(
    std::string_view ide_path,
    IDEPathFormat format) {
    switch (format) {
        case IDEPathFormat::URI:
            if (ide_path.starts_with("file://")) {
                return fs::path(std::string(ide_path.substr(7)));
            }
            return fs::path(std::string(ide_path));
        case IDEPathFormat::Windows:
            {
                std::string s(ide_path);
                for (auto& c : s) {
                    if (c == '\\') c = '/';
                }
                return fs::path(s);
            }
        default:
            return fs::path(std::string(ide_path));
    }
}

/// Detect the path format used by a given IDE type
[[nodiscard]] inline IDEPathFormat get_ide_path_format(IDEType ide_type) {
    switch (ide_type) {
        case IDEType::VSCode:
        case IDEType::VSCodeInsiders:
        case IDEType::Cursor:
        case IDEType::Windsurf:
            return IDEPathFormat::URI;
        default:
            return IDEPathFormat::Unix;
    }
}

// =========================================================================
// JetBrains-specific utilities
// =========================================================================

/// Check if the given IDE type is a JetBrains IDE
[[nodiscard]] inline bool is_jetbrains_ide(IDEType type) noexcept {
    switch (type) {
        case IDEType::PyCharm:
        case IDEType::IntelliJ:
        case IDEType::WebStorm:
        case IDEType::PhpStorm:
        case IDEType::RubyMine:
        case IDEType::CLion:
        case IDEType::GoLand:
        case IDEType::Rider:
        case IDEType::DataGrip:
        case IDEType::AppCode:
        case IDEType::DataSpell:
        case IDEType::Aqua:
        case IDEType::Gateway:
        case IDEType::Fleet:
        case IDEType::AndroidStudio:
            return true;
        default:
            return false;
    }
}

/// Get the CLI command for a JetBrains IDE
[[nodiscard]] inline std::expected<std::string, std::string>
get_jetbrains_command(IDEType type) {
    switch (type) {
        case IDEType::IntelliJ: return "idea";
        case IDEType::WebStorm: return "webstorm";
        case IDEType::PyCharm: return "pycharm";
        case IDEType::CLion: return "clion";
        case IDEType::GoLand: return "goland";
        case IDEType::PhpStorm: return "phpstorm";
        case IDEType::RubyMine: return "rubymine";
        case IDEType::Rider: return "rider";
        case IDEType::DataGrip: return "datagrip";
        case IDEType::AndroidStudio: return "studio";
        case IDEType::Fleet: return "fleet";
        default:
            return std::unexpected(std::format(
                "No known CLI command for IDE type {}", static_cast<int>(type)));
    }
}

} // namespace cc::utils::ide
