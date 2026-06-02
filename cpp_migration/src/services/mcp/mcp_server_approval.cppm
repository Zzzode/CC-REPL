/// @file mcp_server_approval.cppm
/// @brief MCP服务器审批服务
/// 处理MCP服务器的审批流程，管理待审批服务器列表
module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <coroutine>

export module cc.services.mcp.mcp_server_approval;

import cc.types.types;
import cc.utils.async;
import cc.utils.error;
import cc.utils.json;

export namespace cc::services::mcp::mcp_server_approval {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

namespace fs = std::filesystem;

// ============================================================
// 类型定义
// ============================================================

/// 服务器审批状态
enum class ApprovalStatus : std::uint8_t {
    Pending,
    Approved,
    Rejected
};

/// MCP服务器信息
struct McpServerInfo {
    std::string name;
    std::string description;
    std::string version;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::string scope; // "user" 或 "project"
    ApprovalStatus status;
    TimePoint created_at;
    TimePoint updated_at;
};

/// 审批回调类型
using ApprovalCallback = std::function<void(const std::string& server_name, bool approved)>;
using MultiApprovalCallback = std::function<void(const std::unordered_map<std::string, bool>& results)>;

// ============================================================
// MCP服务器审批服务
// ============================================================

class McpServerApprovalService {
public:
    explicit McpServerApprovalService() = default;

    ~McpServerApprovalService() = default;

    // 禁止拷贝，允许移动
    McpServerApprovalService(const McpServerApprovalService&) = delete;
    McpServerApprovalService& operator=(const McpServerApprovalService&) = delete;
    McpServerApprovalService(McpServerApprovalService&&) noexcept = default;
    McpServerApprovalService& operator=(McpServerApprovalService&&) noexcept = default;

    /// 获取指定范围的待审批服务器
    /// @param scope 服务器范围 ("project" 或 "user")
    /// @return 待审批服务器列表
    [[nodiscard]] std::vector<McpServerInfo> get_pending_servers(std::string_view scope) const {
        std::vector<McpServerInfo> pending;
        for (const auto& [name, info] : servers_) {
            if (info.scope == scope && info.status == ApprovalStatus::Pending) {
                pending.push_back(info);
            }
        }
        return pending;
    }

    /// 获取所有待审批服务器
    [[nodiscard]] std::vector<McpServerInfo> get_all_pending_servers() const {
        std::vector<McpServerInfo> pending;
        for (const auto& [name, info] : servers_) {
            if (info.status == ApprovalStatus::Pending) {
                pending.push_back(info);
            }
        }
        return pending;
    }

    /// 审批单个服务器
    /// @param server_name 服务器名称
    /// @param approved 是否批准
    /// @return 操作结果
    Result<void> approve_server(const std::string& server_name, bool approved) {
        auto it = servers_.find(server_name);
        if (it == servers_.end()) {
            return std::unexpected(Error(ErrorCode::not_found, "server not found"));
        }

        it->second.status = approved ? ApprovalStatus::Approved : ApprovalStatus::Rejected;
        it->second.updated_at = Clock::now();
        save_servers_to_config();

        // 调用回调
        if (on_approval_) {
            on_approval_(server_name, approved);
        }

        return {};
    }

    /// 批量审批服务器
    /// @param decisions 服务器名称和审批决定的映射
    /// @return 操作结果
    Result<void> approve_servers(const std::unordered_map<std::string, bool>& decisions) {
        for (const auto& [name, approved] : decisions) {
            auto it = servers_.find(name);
            if (it != servers_.end()) {
                it->second.status = approved ? ApprovalStatus::Approved : ApprovalStatus::Rejected;
                it->second.updated_at = Clock::now();
            }
        }
        save_servers_to_config();

        // 调用回调
        if (on_multi_approval_) {
            on_multi_approval_(decisions);
        }

        return {};
    }

    /// 添加待审批服务器
    /// @param server_info 服务器信息
    void add_pending_server(const McpServerInfo& server_info) {
        auto info = server_info;
        info.status = ApprovalStatus::Pending;
        info.created_at = Clock::now();
        info.updated_at = Clock::now();
        servers_[server_info.name] = std::move(info);
    }

    /// 移除服务器
    /// @param server_name 服务器名称
    void remove_server(const std::string& server_name) {
        servers_.erase(server_name);
    }

    /// 获取服务器信息
    /// @param server_name 服务器名称
    /// @return 服务器信息（如果存在）
    [[nodiscard]] std::optional<McpServerInfo> get_server(const std::string& server_name) const {
        auto it = servers_.find(server_name);
        if (it != servers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// 检查是否有待审批的服务器
    [[nodiscard]] bool has_pending_servers() const {
        for (const auto& [name, info] : servers_) {
            if (info.status == ApprovalStatus::Pending) {
                return true;
            }
        }
        return false;
    }

    /// 检查特定范围是否有待审批的服务器
    [[nodiscard]] bool has_pending_servers(std::string_view scope) const {
        for (const auto& [name, info] : servers_) {
            if (info.scope == scope && info.status == ApprovalStatus::Pending) {
                return true;
            }
        }
        return false;
    }

    /// 设置审批回调
    void set_approval_callback(ApprovalCallback callback) {
        on_approval_ = std::move(callback);
    }

    /// 设置批量审批回调
    void set_multi_approval_callback(MultiApprovalCallback callback) {
        on_multi_approval_ = std::move(callback);
    }

    /// 从配置加载服务器
    /// @param scope 服务器范围
    void load_servers_from_config(std::string_view scope) {
        std::ifstream input(config_path());
        if (!input) return;

        std::string line;
        while (std::getline(input, line)) {
            auto parsed = parse_config_line(line);
            if (!parsed || parsed->scope != scope) continue;

            auto it = servers_.find(parsed->name);
            if (it == servers_.end()) {
                servers_.emplace(parsed->name, std::move(*parsed));
            } else {
                it->second.status = parsed->status;
                it->second.updated_at = Clock::now();
            }
        }
    }

    /// 保存服务器状态到配置
    void save_servers_to_config() {
        auto path = config_path();
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) return;

        std::ofstream output(path, std::ios::trunc);
        if (!output) return;
        for (const auto& [name, info] : servers_) {
            output << escape_field(name) << '\t'
                   << escape_field(info.scope) << '\t'
                   << status_to_string(info.status) << '\t'
                   << escape_field(info.command) << '\t'
                   << escape_field(info.description) << '\t'
                   << escape_field(info.version) << '\n';
        }
    }

private:
    [[nodiscard]] static fs::path config_path() {
        if (const char* home = std::getenv("HOME")) {
            return fs::path(home) / ".cc-repl" / "mcp-server-approvals.txt";
        }
        return fs::path(".cc-repl") / "mcp-server-approvals.txt";
    }

    [[nodiscard]] static std::string_view status_to_string(ApprovalStatus status) noexcept {
        switch (status) {
            case ApprovalStatus::Pending: return "pending";
            case ApprovalStatus::Approved: return "approved";
            case ApprovalStatus::Rejected: return "rejected";
        }
        return "pending";
    }

    [[nodiscard]] static ApprovalStatus status_from_string(std::string_view status) noexcept {
        if (status == "approved") return ApprovalStatus::Approved;
        if (status == "rejected") return ApprovalStatus::Rejected;
        return ApprovalStatus::Pending;
    }

    [[nodiscard]] static std::string escape_field(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '\r') escaped.push_back('\\');
            switch (ch) {
                case '\t': escaped.push_back('t'); break;
                case '\n': escaped.push_back('n'); break;
                case '\r': escaped.push_back('r'); break;
                default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    [[nodiscard]] static std::string unescape_field(std::string_view value) {
        std::string unescaped;
        unescaped.reserve(value.size());
        bool escaped = false;
        for (char ch : value) {
            if (escaped) {
                switch (ch) {
                    case 't': unescaped.push_back('\t'); break;
                    case 'n': unescaped.push_back('\n'); break;
                    case 'r': unescaped.push_back('\r'); break;
                    default: unescaped.push_back(ch); break;
                }
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else {
                unescaped.push_back(ch);
            }
        }
        return unescaped;
    }

    [[nodiscard]] static std::optional<McpServerInfo> parse_config_line(std::string_view line) {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (start <= line.size()) {
            auto end = line.find('\t', start);
            if (end == std::string_view::npos) {
                parts.push_back(line.substr(start));
                break;
            }
            parts.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        if (parts.size() < 3) return std::nullopt;
        auto now = Clock::now();
        return McpServerInfo{
            .name = unescape_field(parts[0]),
            .description = parts.size() > 4 ? unescape_field(parts[4]) : std::string{},
            .version = parts.size() > 5 ? unescape_field(parts[5]) : std::string{},
            .command = parts.size() > 3 ? unescape_field(parts[3]) : std::string{},
            .args = {},
            .env = {},
            .scope = unescape_field(parts[1]),
            .status = status_from_string(parts[2]),
            .created_at = now,
            .updated_at = now,
        };
    }

    std::unordered_map<std::string, McpServerInfo> servers_;
    ApprovalCallback on_approval_;
    MultiApprovalCallback on_multi_approval_;
};

// ============================================================
// 便捷函数
// ============================================================

/// 处理待审批的MCP服务器
/// @param scope 服务器范围
/// @param approval_service 审批服务
/// @param callback 审批回调
Task<void> handle_pending_approvals(
    std::string_view scope,
    McpServerApprovalService& approval_service,
    ApprovalCallback callback)
{
    auto pending = approval_service.get_pending_servers(scope);
    if (pending.empty()) {
        co_return;
    }

    approval_service.set_approval_callback(std::move(callback));

    // 无交互环境下使用确定性本地策略：当前待处理列表全部批准，并触发回调/持久化。
    for (const auto& server : pending) {
        approval_service.approve_server(server.name, true);
    }

    co_return;
}

} // namespace cc::services::mcp::mcp_server_approval
