/// @file ide_integration.cppm
/// @brief IDE bridge connection state and communication management.
/// Auto-detects IDE type, manages WebSocket connection, handles file sync,
/// editor state tracking, and apply-diff commands.
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.ide_integration;


export namespace cc::hooks {

// ============================================================
// IDE type detection
// ============================================================

/// Supported IDE types for bridge integration
enum class IdeType : std::uint8_t {
    None,         // 无 IDE 连接
    VSCode,       // Visual Studio Code
    JetBrains,    // IntelliJ / WebStorm / CLion 等
    Neovim,       // Neovim (通过 nvim --listen)
    Vim,          // Vim (通过 +clientserver)
    Emacs,        // Emacs (通过 emacsclient)
};

/// Convert IdeType to display name
[[nodiscard]] constexpr auto ide_type_to_string(IdeType type) noexcept -> std::string_view {
    switch (type) {
        case IdeType::None:      return "None";
        case IdeType::VSCode:    return "VS Code";
        case IdeType::JetBrains: return "JetBrains";
        case IdeType::Neovim:    return "Neovim";
        case IdeType::Vim:       return "Vim";
        case IdeType::Emacs:     return "Emacs";
    }
    return "Unknown";
}

// ============================================================
// Editor state types
// ============================================================

/// Selection in an editor file
struct Selection {
    std::size_t start_line{0};
    std::size_t start_col{0};
    std::size_t end_line{0};
    std::size_t end_col{0};

    [[nodiscard]] auto is_empty() const -> bool {
        return start_line == end_line && start_col == end_col;
    }
};

/// Information about an open file in the IDE
struct OpenFile {
    std::filesystem::path path;
    bool is_modified{false};
    bool is_active{false};
    std::optional<Selection> selection;
    std::optional<std::size_t> cursor_line;
    std::optional<std::size_t> cursor_col;
};

/// File edit operation to be sent to the IDE
struct FileEdit {
    std::filesystem::path path;
    std::size_t start_line;
    std::size_t start_col;
    std::size_t end_line;
    std::size_t end_col;
    std::string new_text;     // 替换内容
    std::optional<std::string> description;  // 用于 undo 标签
};

/// IDE connection parameters
struct IdeConnection {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    IdeType ide_type{IdeType::None};
    std::optional<std::string> auth_token;   // 认证令牌（如需要）
    std::chrono::milliseconds timeout{5000}; // 连接超时
};

// ============================================================
// IdeState - complete state of the IDE integration hook
// ============================================================

/// Full state of the IDE integration
struct IdeState {
    bool connected{false};                             // 是否已连接
    IdeType ide_type{IdeType::None};                   // 已识别的 IDE 类型
    std::filesystem::path workspace_root;              // 工作区根目录
    std::vector<OpenFile> open_files;                  // IDE 中打开的文件列表
    std::optional<std::filesystem::path> active_file;  // 当前活跃文件
    std::chrono::system_clock::time_point connected_at;// 连接时间
    std::size_t sync_count{0};                         // 已同步的文件数
    std::optional<std::string> last_error;             // 最后一次错误
};

// ============================================================
// Event callback types
// ============================================================

/// Callback when a file changes in the IDE
using FileChangedCallback = std::function<void(const std::filesystem::path&)>;
/// Callback when IDE connection status changes
using ConnectionCallback = std::function<void(bool connected, IdeType type)>;
/// Callback when active file changes
using ActiveFileCallback = std::function<void(const std::filesystem::path&)>;

// ============================================================
// IdeIntegrationHook - IDE bridge manager
// ============================================================

/// Manages the bidirectional communication bridge with the user's IDE.
/// Handles auto-detection, WebSocket lifecycle, file synchronization,
/// and edit application commands.
class IdeIntegrationHook {
public:
    IdeIntegrationHook() = default;
    ~IdeIntegrationHook() { disconnect(); }

    // Move-only semantics（WebSocket 连接不可复制）
    IdeIntegrationHook(const IdeIntegrationHook&) = delete;
    IdeIntegrationHook& operator=(const IdeIntegrationHook&) = delete;
    IdeIntegrationHook(IdeIntegrationHook&&) noexcept = default;
    IdeIntegrationHook& operator=(IdeIntegrationHook&&) noexcept = default;

    // ─── Connection management ─────────────────────────────────

    /// Connect to an IDE extension via WebSocket on the given port
    [[nodiscard]] auto connect(std::uint16_t port) -> std::expected<void, std::string> {
        if (state_.connected) {
            return std::unexpected("Already connected; disconnect first");
        }
        if (port == 0) {
            return std::unexpected("IDE bridge port must be non-zero");
        }

        connection_ = IdeConnection{.port = port, .ide_type = detect_ide_type(port)};
        state_.connected = true;
        state_.ide_type = connection_->ide_type;
        state_.connected_at = std::chrono::system_clock::now();
        send_message(std::format(R"({{"type":"hello","port":{},"ide":"{}"}})",
            port, ide_type_to_string(state_.ide_type)));

        // 通知连接回调
        if (on_connection_) on_connection_(true, state_.ide_type);
        return {};
    }

    /// Connect with full connection parameters
    [[nodiscard]] auto connect(const IdeConnection& conn) -> std::expected<void, std::string> {
        if (state_.connected) {
            return std::unexpected("Already connected; disconnect first");
        }
        if (conn.host.empty()) {
            return std::unexpected("IDE bridge host must not be empty");
        }
        if (conn.port == 0) {
            return std::unexpected("IDE bridge port must be non-zero");
        }

        connection_ = conn;
        state_.connected = true;
        state_.ide_type = conn.ide_type == IdeType::None ? detect_ide_type(conn.port) : conn.ide_type;
        state_.connected_at = std::chrono::system_clock::now();
        send_message(std::format(R"({{"type":"hello","host":"{}","port":{},"ide":"{}"{}}})",
            json_escape(conn.host), conn.port, ide_type_to_string(state_.ide_type),
            conn.auth_token ? std::format(R"(,"authToken":"{}")", json_escape(*conn.auth_token)) : std::string{}));
        if (on_connection_) on_connection_(true, state_.ide_type);
        return {};
    }

    /// Disconnect from the IDE
    auto disconnect() -> void {
        if (!state_.connected) return;
        send_message(R"({"type":"disconnect"})");
        connection_.reset();
        state_.connected = false;
        state_.open_files.clear();
        state_.active_file = std::nullopt;
        if (on_connection_) on_connection_(false, state_.ide_type);
    }

    /// Check if connected to an IDE
    [[nodiscard]] auto is_connected() const -> bool { return state_.connected; }

    // ─── File synchronization ──────────────────────────────────

    /// Request the IDE to sync a specific file's content to disk
    [[nodiscard]] auto sync_file(const std::filesystem::path& path)
        -> std::expected<void, std::string> {
        if (!state_.connected) {
            return std::unexpected("Not connected to any IDE");
        }
        if (path.empty()) {
            return std::unexpected("Cannot sync an empty path");
        }
        send_message(std::format(R"({{"type":"file/sync","path":"{}"}})",
            json_escape(path.string())));
        state_.sync_count++;
        return {};
    }

    /// Notify the IDE that a file was changed externally (by the AI)
    auto notify_file_changed(const std::filesystem::path& path) -> void {
        if (!state_.connected) return;
        send_message(std::format(R"({{"type":"file/didChange","path":"{}"}})",
            json_escape(path.string())));
        if (on_file_changed_) on_file_changed_(path);
    }

    // ─── Editor state queries ──────────────────────────────────

    /// Get the current selection in the active editor
    [[nodiscard]] auto get_editor_selection() const -> std::optional<Selection> {
        if (!state_.connected || !state_.active_file) return std::nullopt;
        // 从缓存的 open_files 中查找活跃文件的选区
        for (const auto& file : state_.open_files) {
            if (file.is_active && file.selection) {
                return file.selection;
            }
        }
        return std::nullopt;
    }

    /// Get the currently active file path
    [[nodiscard]] auto get_active_file() const -> std::optional<std::filesystem::path> {
        return state_.active_file;
    }

    /// Get list of all open files in the IDE
    [[nodiscard]] auto get_open_files() const -> const std::vector<OpenFile>& {
        return state_.open_files;
    }

    // ─── Edit application ──────────────────────────────────────

    /// Apply a text edit to a file in the IDE
    [[nodiscard]] auto apply_edit(const FileEdit& edit) -> std::expected<void, std::string> {
        if (!state_.connected) {
            return std::unexpected("Not connected to any IDE");
        }
        if (edit.path.empty()) {
            return std::unexpected("Cannot apply edit to an empty path");
        }
        send_message(serialize_edit(edit));
        return {};
    }

    /// Apply multiple edits atomically
    [[nodiscard]] auto apply_edits(const std::vector<FileEdit>& edits)
        -> std::expected<void, std::string> {
        if (!state_.connected) {
            return std::unexpected("Not connected to any IDE");
        }
        if (edits.empty()) {
            return {};
        }
        std::ostringstream payload;
        payload << R"({"type":"workspace/applyEdits","edits":[)";
        for (const auto& edit : edits) {
            if (&edit != &edits.front()) payload << ',';
            payload << serialize_edit_body(edit);
        }
        payload << "]}";
        send_message(payload.str());
        return {};
    }

    // ─── Event subscriptions ───────────────────────────────────

    /// Subscribe to file change events from the IDE
    auto on_file_changed(FileChangedCallback cb) -> void {
        on_file_changed_ = std::move(cb);
    }

    /// Subscribe to connection status changes
    auto on_connection_change(ConnectionCallback cb) -> void {
        on_connection_ = std::move(cb);
    }

    /// Subscribe to active file changes
    auto on_active_file_change(ActiveFileCallback cb) -> void {
        on_active_file_ = std::move(cb);
    }

    // ─── State access ──────────────────────────────────────────

    [[nodiscard]] auto state() const -> const IdeState& { return state_; }
    [[nodiscard]] auto ide_type() const -> IdeType { return state_.ide_type; }
    [[nodiscard]] auto workspace_root() const -> const std::filesystem::path& {
        return state_.workspace_root;
    }

    /// Set workspace root (typically auto-detected from IDE)
    auto set_workspace_root(std::filesystem::path root) -> void {
        state_.workspace_root = std::move(root);
    }

private:
    IdeState state_;
    FileChangedCallback on_file_changed_;
    ConnectionCallback on_connection_;
    ActiveFileCallback on_active_file_;
    std::optional<IdeConnection> connection_;
    std::vector<std::string> outbound_messages_;

    // ─── Internal helpers ──────────────────────────────────────

    /// Auto-detect IDE type based on port conventions and handshake
    [[nodiscard]] auto detect_ide_type(std::uint16_t port) const -> IdeType {
        // 常见端口约定:
        // VSCode 扩展通常监听 3000-4000 范围
        // JetBrains 插件通常监听 63342 或 6942+
        // Neovim 通常通过 socket 而非 TCP
        if (port >= 63340 && port <= 63350) return IdeType::JetBrains;
        if (port >= 6940 && port <= 6950) return IdeType::JetBrains;
        return IdeType::VSCode; // 默认假设 VSCode
    }

    /// Process an incoming message from the IDE WebSocket
    auto handle_incoming_message(std::string_view message) -> void {
        if (message.find("file/changed") != std::string_view::npos) {
            if (auto path = extract_json_string(message, "path")) {
                notify_file_changed(*path);
            }
            return;
        }
        if (message.find("file/active") != std::string_view::npos) {
            if (auto path = extract_json_string(message, "path")) {
                auto prev = state_.active_file;
                state_.active_file = std::filesystem::path(*path);
                if (prev != state_.active_file && on_active_file_) on_active_file_(*state_.active_file);
            }
            return;
        }
        if (message.find("handshake") != std::string_view::npos) {
            if (auto ide = extract_json_string(message, "ide")) {
                state_.ide_type = parse_ide_type(*ide);
            }
        }
    }

    /// Update the internal open files list from IDE state push
    auto update_open_files(std::vector<OpenFile> files) -> void {
        state_.open_files = std::move(files);
        // 更新 active_file
        for (const auto& file : state_.open_files) {
            if (file.is_active) {
                auto prev = state_.active_file;
                state_.active_file = file.path;
                if (prev != state_.active_file && on_active_file_) {
                    on_active_file_(file.path);
                }
                break;
            }
        }
    }

    auto send_message(std::string message) -> void {
        outbound_messages_.push_back(std::move(message));
    }

    [[nodiscard]] static auto serialize_edit(const FileEdit& edit) -> std::string {
        return std::format(R"({{"type":"workspace/applyEdit","edit":{}}})", serialize_edit_body(edit));
    }

    [[nodiscard]] static auto serialize_edit_body(const FileEdit& edit) -> std::string {
        return std::format(
            R"({{"path":"{}","range":{{"start":{{"line":{},"column":{}}},"end":{{"line":{},"column":{}}}}},"text":"{}"{}}})",
            json_escape(edit.path.string()), edit.start_line, edit.start_col, edit.end_line, edit.end_col,
            json_escape(edit.new_text),
            edit.description ? std::format(R"(,"description":"{}")", json_escape(*edit.description)) : std::string{});
    }

    [[nodiscard]] static auto json_escape(std::string_view input) -> std::string {
        std::string out;
        out.reserve(input.size());
        for (char ch : input) {
            switch (ch) {
                case '"': out += R"(\")"; break;
                case '\\': out += R"(\\)"; break;
                case '\n': out += R"(\n)"; break;
                case '\r': out += R"(\r)"; break;
                case '\t': out += R"(\t)"; break;
                default: out += ch; break;
            }
        }
        return out;
    }

    [[nodiscard]] static auto extract_json_string(std::string_view json, std::string_view key)
        -> std::optional<std::string> {
        const auto needle = std::format(R"("{}")", key);
        auto pos = json.find(needle);
        if (pos == std::string_view::npos) return std::nullopt;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string_view::npos) return std::nullopt;
        pos = json.find('"', pos + 1);
        if (pos == std::string_view::npos) return std::nullopt;
        std::string result;
        bool escaped = false;
        for (std::size_t i = pos + 1; i < json.size(); ++i) {
            const char ch = json[i];
            if (escaped) {
                switch (ch) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += ch; break;
                }
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return result;
            } else {
                result += ch;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto parse_ide_type(std::string_view value) -> IdeType {
        if (value == "vscode" || value == "VS Code") return IdeType::VSCode;
        if (value == "jetbrains" || value == "JetBrains") return IdeType::JetBrains;
        if (value == "neovim" || value == "Neovim") return IdeType::Neovim;
        if (value == "vim" || value == "Vim") return IdeType::Vim;
        if (value == "emacs" || value == "Emacs") return IdeType::Emacs;
        return IdeType::None;
    }
};

} // namespace cc::hooks
