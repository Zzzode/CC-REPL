// IDE detection, lockfile scanning, RPC, path conversion, and integration utilities
// Sources: ide.ts, idePathConversion.ts, jetbrains.ts
module;

#include <chrono>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

export module cc.utils.ide_integration;

import cc.utils.json;
import cc.utils.async;
import cc.services.mcp.client;
import cc.services.mcp.types;

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
    std::vector<fs::path> workspace_folders;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::string> transport;  // "stdio" or "ws"
    std::optional<std::string> auth_token;
    bool running_in_windows = false;

    [[nodiscard]] std::string mcp_url() const {
        if (transport && *transport == "ws") {
            return std::format("ws://127.0.0.1:{}", port);
        }
        return std::format("http://127.0.0.1:{}/sse", port);
    }
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
            if (entry.path().extension() != ".json" && entry.path().extension() != ".lock") continue;

            auto lockfile = parse_lockfile(entry.path());
            if (!lockfile) continue;

            if (lockfile->pid <= 0 || is_process_alive(lockfile->pid)) {
                results.push_back(std::move(*lockfile));
            } else {
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
            if (is_any_workspace_match(lf, workspace)) {
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

        for (auto& lf : lockfiles) {
            if (is_any_workspace_match(lf, cwd)) {
                return std::move(lf);
            }
        }
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

        auto port_from_name = parse_port_from_filename(path);
        auto doc = parse(content);
        if (!doc) {
            if (path.extension() != ".lock" || !port_from_name) return std::nullopt;
            IdeLockfile lf;
            lf.name = "IDE";
            lf.port = *port_from_name;
            std::istringstream lines(content);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) continue;
                lf.workspace_folders.emplace_back(line);
            }
            if (!lf.workspace_folders.empty()) {
                lf.workspace_folder = lf.workspace_folders.front();
            }
            return lf;
        }

        auto root = doc->root();
        if (!root.is_obj()) return std::nullopt;

        IdeLockfile lf;

        auto name_node = root.get("ideName");
        if (!name_node.is_str()) {
            name_node = root.get("name");
        }
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
        if (lf.port == 0 && port_from_name) {
            lf.port = *port_from_name;
        }

        auto workspace_folders_node = root.get("workspaceFolders");
        if (workspace_folders_node.is_arr()) {
            workspace_folders_node.iter([&lf](JsonVal item) {
                if (item.is_str()) {
                    lf.workspace_folders.emplace_back(std::string(item.as_str()));
                }
            });
        }

        auto workspace_node = root.get("workspaceFolder");
        if (!workspace_node.is_str()) {
            workspace_node = root.get("workspace");
        }
        if (workspace_node.is_str()) {
            lf.workspace_folder = std::string(workspace_node.as_str());
            lf.workspace_folders.push_back(lf.workspace_folder);
        } else if (!lf.workspace_folders.empty()) {
            lf.workspace_folder = lf.workspace_folders.front();
        }

        auto transport_node = root.get("transport");
        if (transport_node.is_str()) {
            lf.transport = std::string(transport_node.as_str());
        }

        auto auth_token_node = root.get("authToken");
        if (auth_token_node.is_str()) {
            lf.auth_token = std::string(auth_token_node.as_str());
        }

        auto running_in_windows_node = root.get("runningInWindows");
        lf.running_in_windows = running_in_windows_node.as_bool();

        if (lf.name.empty()) {
            lf.name = "IDE";
        }
        if (lf.pid == 0 && lf.port == 0) return std::nullopt;
        return lf;
    }

    [[nodiscard]] static std::optional<uint16_t> parse_port_from_filename(const fs::path& path) {
        const auto stem = path.stem().string();
        if (stem.empty()) return std::nullopt;
        char* end = nullptr;
        const auto parsed = std::strtol(stem.c_str(), &end, 10);
        if (end == stem.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535) {
            return std::nullopt;
        }
        return static_cast<uint16_t>(parsed);
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

    [[nodiscard]] static bool is_any_workspace_match(
        const IdeLockfile& lockfile,
        const fs::path& target) {
        if (is_workspace_match(lockfile.workspace_folder, target)) return true;
        for (const auto& workspace : lockfile.workspace_folders) {
            if (is_workspace_match(workspace, target)) return true;
        }
        return false;
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
        lockfile_ = lockfile;
        return {};
    }

    /// Check if connected
    [[nodiscard]] bool is_connected() const noexcept { return connected_; }

    /// Call an IDE MCP tool.
    [[nodiscard]] IdeRpcResponse call(std::string_view tool_name,
                                       std::string_view params_json = "{}") {
        if (!connected_ || !lockfile_) {
            return IdeRpcResponse{.success = false, .error = "Not connected to IDE"};
        }

        if (lockfile_->transport && *lockfile_->transport == "ws") {
            return call_websocket(tool_name, params_json);
        }

        auto request = std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})",
            next_id_++, json_escape(tool_name), params_json);
        outbound_requests_.push_back(std::move(request));

        cc::services::mcp::McpClient::Config config;
        config.name = "ide";
        config.request_timeout = std::chrono::milliseconds{5000};
        config.init_timeout = std::chrono::milliseconds{5000};
        config.client_info.name = "claude-code-native";
        config.client_info.version = "1.0.0";

        cc::services::mcp::McpClient client(std::move(config));
        const auto connected = client.connect_sse(lockfile_->mcp_url(), {});
        if (!connected) {
            return IdeRpcResponse{
                .success = false,
                .error = std::format("Failed to connect to IDE MCP server: {}", mcp_error_to_string(connected.error()))
            };
        }

        cc::services::mcp::ToolCallRequest tool_request{
            .name = std::string(tool_name),
            .arguments_json = std::string(params_json),
        };
        auto result = client.call_tool(tool_request);
        client.shutdown();

        if (!result) {
            return IdeRpcResponse{
                .success = false,
                .error = std::format("IDE MCP tool call failed: {}", mcp_error_to_string(result.error()))
            };
        }

        auto content_json = serialize_content(result->content);
        if (result->is_error) {
            return IdeRpcResponse{.success = false, .result = content_json, .error = content_json};
        }

        return IdeRpcResponse{.success = true, .result = content_json};
    }

    /// Convenience: notify file changed
    IdeRpcResponse notify_file_changed(const fs::path& path) {
        auto params = std::format(R"({{"filePath":"{}"}})", json_escape(path.string()));
        return call("fileChanged", params);
    }

    /// Convenience: open diff view in IDE
    IdeRpcResponse open_diff(const fs::path& path,
                             std::string_view old_content,
                             std::string_view new_content) {
        JsonMutDoc doc;
        auto root = doc.object();
        root.add("old_file_path", doc.string(path.string()));
        root.add("new_file_path", doc.string(path.string()));
        root.add("new_file_contents", doc.string(new_content));
        root.add("tab_name", doc.string(path.filename().empty() ? path.string() : path.filename().string()));
        root.add("old_file_contents", doc.string(old_content));
        doc.set_root(root);
        return call("openDiff", doc.to_string());
    }

    /// Convenience: open file at line in IDE
    IdeRpcResponse open_file(const fs::path& path, std::optional<uint32_t> line = std::nullopt) {
        JsonMutDoc doc;
        auto root = doc.object();
        root.add("filePath", doc.string(path.string()));
        root.add("preview", doc.boolean(false));
        root.add("startText", doc.string(""));
        root.add("endText", doc.string(""));
        root.add("selectToEndOfLine", doc.boolean(false));
        root.add("makeFrontmost", doc.boolean(false));
        if (line) {
            root.add("line", doc.number(static_cast<int64_t>(*line)));
        }
        doc.set_root(root);
        return call("openFile", doc.to_string());
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
    std::optional<IdeLockfile> lockfile_;

    [[nodiscard]] static std::string mcp_error_to_string(cc::services::mcp::McpClientError error) {
        using enum cc::services::mcp::McpClientError;
        switch (error) {
            case ConnectionFailed: return "connection failed";
            case NotConnected: return "not connected";
            case AlreadyConnected: return "already connected";
            case TransportError: return "transport error";
            case ServerClosed: return "server closed";
            case Timeout: return "timeout";
            case InvalidResponse: return "invalid response";
            case InitializationFailed: return "initialization failed";
            case ProtocolError: return "protocol error";
            case ServerNotFound: return "server not found";
            case ToolNotFound: return "tool not found";
            case Unauthorized: return "unauthorized";
        }
        return "unknown error";
    }

    [[nodiscard]] static std::string serialize_content(const std::vector<cc::services::mcp::ContentItem>& content) {
        std::string json = "[";
        bool first = true;
        for (const auto& item : content) {
            if (!first) json += ",";
            first = false;
            json += std::format(
                R"({{"type":"{}","text":"{}"}})",
                json_escape(item.type),
                json_escape(item.text));
        }
        json += "]";
        return json;
    }

    struct WsUrlParts {
        std::string host;
        uint16_t port = 80;
        std::string path = "/";
    };

    struct WsFrame {
        uint8_t opcode = 0;
        std::string payload;
    };

    [[nodiscard]] IdeRpcResponse call_websocket(
        std::string_view tool_name,
        std::string_view params_json) {
        auto parts = parse_ws_url(lockfile_->mcp_url());
        if (!parts) {
            return IdeRpcResponse{.success = false, .error = parts.error()};
        }

        auto fd = connect_tcp(*parts);
        if (!fd) {
            return IdeRpcResponse{.success = false, .error = fd.error()};
        }

        const auto close_fd = [](int socket_fd) {
            if (socket_fd >= 0) {
                ::shutdown(socket_fd, SHUT_RDWR);
                ::close(socket_fd);
            }
        };

        auto handshake = perform_ws_handshake(*fd, *parts, lockfile_->auth_token);
        if (!handshake) {
            auto error = handshake.error();
            close_fd(*fd);
            return IdeRpcResponse{.success = false, .error = error};
        }

        const auto initialize_id = next_id_++;
        auto initialize_request = std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"initialize","params":{{"protocolVersion":"2024-11-05","capabilities":{{}},"clientInfo":{{"name":"claude-code-native","version":"1.0.0"}}}}}})",
            initialize_id);
        if (auto sent = send_ws_text(*fd, initialize_request); !sent) {
            auto error = sent.error();
            close_fd(*fd);
            return IdeRpcResponse{.success = false, .error = error};
        }
        if (auto initialized = receive_ws_response(*fd, initialize_id); !initialized) {
            auto error = initialized.error();
            close_fd(*fd);
            return IdeRpcResponse{.success = false, .error = error};
        }

        const std::string initialized_notification =
            R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})";
        if (auto sent = send_ws_text(*fd, initialized_notification); !sent) {
            auto error = sent.error();
            close_fd(*fd);
            return IdeRpcResponse{.success = false, .error = error};
        }

        const auto tool_id = next_id_++;
        auto tool_request = std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})",
            tool_id, json_escape(tool_name), params_json);
        outbound_requests_.push_back(tool_request);
        if (auto sent = send_ws_text(*fd, tool_request); !sent) {
            auto error = sent.error();
            close_fd(*fd);
            return IdeRpcResponse{.success = false, .error = error};
        }

        auto tool_response = receive_ws_response(*fd, tool_id);
        close_fd(*fd);
        if (!tool_response) {
            return IdeRpcResponse{.success = false, .error = tool_response.error()};
        }
        return parse_tool_call_response(*tool_response);
    }

    [[nodiscard]] static std::expected<WsUrlParts, std::string> parse_ws_url(std::string_view url) {
        if (!url.starts_with("ws://")) {
            return std::unexpected("IDE WebSocket MCP URL must use ws://");
        }
        url.remove_prefix(5);
        auto slash = url.find('/');
        auto authority = slash == std::string_view::npos ? url : url.substr(0, slash);
        WsUrlParts parts;
        parts.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
        auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            parts.host = std::string(authority);
            parts.port = 80;
        } else {
            parts.host = std::string(authority.substr(0, colon));
            auto port_text = std::string(authority.substr(colon + 1));
            auto parsed_port = std::strtol(port_text.c_str(), nullptr, 10);
            if (parsed_port <= 0 || parsed_port > 65535) {
                return std::unexpected("Invalid IDE WebSocket MCP port");
            }
            parts.port = static_cast<uint16_t>(parsed_port);
        }
        if (parts.host.empty()) return std::unexpected("IDE WebSocket MCP host is empty");
        return parts;
    }

    [[nodiscard]] static std::expected<int, std::string> connect_tcp(const WsUrlParts& parts) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        auto port_text = std::to_string(parts.port);
        if (::getaddrinfo(parts.host.c_str(), port_text.c_str(), &hints, &results) != 0) {
            return std::unexpected("Failed to resolve IDE WebSocket MCP host");
        }

        int fd = -1;
        for (auto* ai = results; ai != nullptr; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            timeval timeout{};
            timeout.tv_sec = 5;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(results);
        if (fd < 0) return std::unexpected("Failed to connect to IDE WebSocket MCP server");
        return fd;
    }

    [[nodiscard]] static std::string random_websocket_key() {
        std::array<unsigned char, 16> bytes{};
        std::random_device rd;
        for (auto& byte : bytes) byte = static_cast<unsigned char>(rd());

        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0;
        int valb = -6;
        for (unsigned char byte : bytes) {
            val = (val << 8) + byte;
            valb += 8;
            while (valb >= 0) {
                out.push_back(alphabet[(val >> valb) & 0x3f]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3f]);
        while (out.size() % 4 != 0) out.push_back('=');
        return out;
    }

    [[nodiscard]] static std::expected<void, std::string> perform_ws_handshake(
        int fd,
        const WsUrlParts& parts,
        const std::optional<std::string>& auth_token) {
        auto request = std::format(
            "GET {} HTTP/1.1\r\n"
            "Host: {}:{}\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: mcp\r\n"
            "Sec-WebSocket-Key: {}\r\n"
            "User-Agent: claude-code-native\r\n",
            parts.path, parts.host, parts.port, random_websocket_key());
        if (auth_token && !auth_token->empty()) {
            request += "X-Claude-Code-Ide-Authorization: " + *auth_token + "\r\n";
        }
        request += "\r\n";

        if (!send_all(fd, request)) {
            return std::unexpected("Failed to send IDE WebSocket MCP handshake");
        }

        std::string response;
        std::array<char, 1024> buffer{};
        while (response.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) return std::unexpected("IDE WebSocket MCP handshake was closed");
            response.append(buffer.data(), static_cast<std::size_t>(n));
            if (response.size() > 64 * 1024) {
                return std::unexpected("IDE WebSocket MCP handshake response is too large");
            }
        }
        if (response.find(" 101 ") == std::string::npos &&
            response.find(" 101\r\n") == std::string::npos) {
            return std::unexpected("IDE WebSocket MCP handshake failed");
        }
        return {};
    }

    [[nodiscard]] static bool send_all(int fd, std::string_view data) {
        while (!data.empty()) {
            auto n = ::send(fd, data.data(), data.size(), 0);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return false;
            }
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    [[nodiscard]] static std::expected<void, std::string> send_ws_frame(
        int fd,
        uint8_t opcode,
        std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
        const auto len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(0x80 | len));
        } else if (len <= 0xffff) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((len >> 8) & 0xff));
            frame.push_back(static_cast<char>(len & 0xff));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((len >> shift) & 0xff));
            }
        }

        std::array<unsigned char, 4> mask{};
        std::random_device rd;
        for (auto& byte : mask) byte = static_cast<unsigned char>(rd());
        for (auto byte : mask) frame.push_back(static_cast<char>(byte));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }

        if (!send_all(fd, frame)) {
            return std::unexpected("Failed to send IDE WebSocket MCP frame");
        }
        return {};
    }

    [[nodiscard]] static std::expected<void, std::string> send_ws_text(
        int fd,
        std::string_view payload) {
        return send_ws_frame(fd, 0x1, payload);
    }

    [[nodiscard]] static std::expected<void, std::string> read_exact(
        int fd,
        char* data,
        std::size_t size) {
        while (size > 0) {
            auto n = ::recv(fd, data, size, 0);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return std::unexpected("IDE WebSocket MCP connection closed while reading");
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return {};
    }

    [[nodiscard]] static std::expected<WsFrame, std::string> read_ws_frame(int fd) {
        unsigned char header[2]{};
        if (auto read = read_exact(fd, reinterpret_cast<char*>(header), 2); !read) {
            return std::unexpected(read.error());
        }

        WsFrame frame;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        uint64_t len = header[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2]{};
            if (auto read = read_exact(fd, reinterpret_cast<char*>(ext), 2); !read) {
                return std::unexpected(read.error());
            }
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8]{};
            if (auto read = read_exact(fd, reinterpret_cast<char*>(ext), 8); !read) {
                return std::unexpected(read.error());
            }
            len = 0;
            for (unsigned char byte : ext) len = (len << 8) | byte;
        }
        if (len > 16 * 1024 * 1024) {
            return std::unexpected("IDE WebSocket MCP frame exceeds 16MiB");
        }

        std::array<unsigned char, 4> mask{};
        if (masked) {
            if (auto read = read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size()); !read) {
                return std::unexpected(read.error());
            }
        }

        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0) {
            if (auto read = read_exact(fd, frame.payload.data(), frame.payload.size()); !read) {
                return std::unexpected(read.error());
            }
        }
        if (masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
            }
        }
        return frame;
    }

    [[nodiscard]] static std::expected<std::string, std::string> receive_ws_response(
        int fd,
        int64_t expected_id) {
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto frame = read_ws_frame(fd);
            if (!frame) return std::unexpected(frame.error());
            if (frame->opcode == 0x8) {
                return std::unexpected("IDE WebSocket MCP server closed the connection");
            }
            if (frame->opcode == 0x9) {
                (void)send_ws_frame(fd, 0xA, frame->payload);
                continue;
            }
            if (frame->opcode != 0x1 && frame->opcode != 0x2) {
                continue;
            }

            auto doc = parse(frame->payload);
            if (!doc) continue;
            auto id = doc->root().get("id");
            const bool matches_id =
                (id.is_num() && id.as_int() == expected_id) ||
                (id.is_str() && id.as_str() == std::to_string(expected_id));
            if (matches_id) return frame->payload;
        }
        return std::unexpected("Timed out waiting for IDE WebSocket MCP response");
    }

    [[nodiscard]] static IdeRpcResponse parse_tool_call_response(std::string_view response_json) {
        auto doc = parse(response_json);
        if (!doc) {
            return IdeRpcResponse{.success = false, .error = "Invalid IDE WebSocket MCP response"};
        }
        auto root = doc->root();
        auto error = root.get("error");
        if (error.valid() && !error.is_null()) {
            return IdeRpcResponse{
                .success = false,
                .result = error.to_string(),
                .error = error.to_string()
            };
        }

        auto result = root.get("result");
        if (!result.valid() || result.is_null()) {
            return IdeRpcResponse{.success = true, .result = "[]"};
        }

        auto content = result.get("content");
        auto content_json = content.valid() ? content.to_string() : result.to_string();
        if (result.get("isError").as_bool()) {
            return IdeRpcResponse{
                .success = false,
                .result = content_json,
                .error = content_json
            };
        }
        return IdeRpcResponse{.success = true, .result = content_json};
    }

    [[nodiscard]] static std::string json_escape(std::string_view input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (char ch : input) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (std::iscntrl(static_cast<unsigned char>(ch))) {
                        out += std::format("\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    } else {
                        out.push_back(ch);
                    }
                    break;
            }
        }
        return out;
    }
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
