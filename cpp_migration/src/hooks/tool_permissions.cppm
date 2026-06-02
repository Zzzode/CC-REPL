// C++23 Module: Tool permission checking with path-based rules, auto-approve, and audit logging
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.hooks.tool_permissions;


export namespace cc::hooks {

// 权限决策结果
enum class PermissionDecision {
    allow,        // 允许执行
    deny,         // 拒绝执行
    ask_user,     // 需要询问用户
    allow_once,   // 仅本次允许
};

// 权限规则
struct PermissionRule {
    std::string tool_pattern;      // 工具名模式（支持通配符 *）
    std::string path_pattern;      // 路径模式（支持通配符）
    PermissionDecision decision{PermissionDecision::allow};
    std::string reason;            // 规则说明

    // 检查规则是否匹配指定的工具和路径
    [[nodiscard]] auto matches(std::string_view tool_name,
                                std::string_view path) const -> bool {
        return matches_pattern(tool_pattern, tool_name) &&
               (path_pattern.empty() || matches_pattern(path_pattern, path));
    }

private:
    // 简单通配符匹配（支持 * 作为任意字符序列）
    [[nodiscard]] static auto matches_pattern(std::string_view pattern,
                                               std::string_view value) -> bool {
        if (pattern == "*") return true;
        if (pattern == value) return true;

        // 前缀匹配: "File*" matches "FileReadTool"
        if (pattern.ends_with('*')) {
            auto prefix = pattern.substr(0, pattern.size() - 1);
            return value.starts_with(prefix);
        }
        // 后缀匹配: "*Tool" matches "FileReadTool"
        if (pattern.starts_with('*')) {
            auto suffix = pattern.substr(1);
            return value.ends_with(suffix);
        }
        return false;
    }
};

// 权限检查上下文
struct PermissionContext {
    std::string tool_name;
    std::string args;              // 工具参数的序列化表示
    std::string working_dir;       // 当前工作目录
    std::vector<PermissionRule> session_rules;  // 本次会话的临时规则
};

// 审计日志条目
struct AuditEntry {
    std::string tool_name;
    std::string args_summary;
    PermissionDecision decision;
    std::string reason;
    std::chrono::system_clock::time_point timestamp;

    // 格式化为可读字符串
    [[nodiscard]] auto format() const -> std::string {
        auto time_t = std::chrono::system_clock::to_time_t(timestamp);
        std::string decision_str;
        switch (decision) {
            case PermissionDecision::allow:      decision_str = "ALLOW"; break;
            case PermissionDecision::deny:       decision_str = "DENY"; break;
            case PermissionDecision::ask_user:   decision_str = "ASK"; break;
            case PermissionDecision::allow_once: decision_str = "ONCE"; break;
        }
        return std::format("[{}] {} {} - {}", decision_str, tool_name, args_summary, reason);
    }
};

// 用户决策回调：询问用户是否允许
using AskUserFn = std::function<PermissionDecision(const PermissionContext&)>;

// ToolPermissionHook: 工具权限管理器
class ToolPermissionHook {
public:
    ToolPermissionHook() = default;

    /**
     * 检查工具是否可以在当前上下文中使用。
     * 按规则优先级依次匹配，自动审批模式跳过询问。
     */
    [[nodiscard]] auto can_use(std::string_view tool_name,
                                std::string_view args = "") -> PermissionDecision {
        std::lock_guard lock{mu_};

        // 自动审批模式：全部放行
        if (auto_approve_) {
            auto decision = PermissionDecision::allow;
            log_decision(tool_name, args, decision, "auto-approve mode");
            return decision;
        }

        // 检查会话记忆（之前用户的选择）
        if (auto it = session_decisions_.find(std::string(tool_name));
            it != session_decisions_.end()) {
            log_decision(tool_name, args, it->second, "session memory");
            return it->second;
        }

        // 按注册的规则匹配
        for (const auto& rule : rules_) {
            if (rule.matches(tool_name, extract_path(args))) {
                log_decision(tool_name, args, rule.decision, rule.reason);
                return rule.decision;
            }
        }

        // 默认需要询问用户
        auto decision = PermissionDecision::ask_user;

        // 如果配置了 ask_user 回调，立即调用
        if (ask_user_fn_) {
            PermissionContext ctx{
                .tool_name = std::string(tool_name),
                .args = std::string(args),
                .working_dir = working_dir_,
            };
            decision = ask_user_fn_(ctx);
        }

        log_decision(tool_name, args, decision, "default policy");
        return decision;
    }

    // 添加权限规则
    auto add_rule(PermissionRule rule) -> void {
        std::lock_guard lock{mu_};
        rules_.push_back(std::move(rule));
    }

    // 移除指定索引的规则
    auto remove_rule(std::size_t index) -> bool {
        std::lock_guard lock{mu_};
        if (index >= rules_.size()) return false;
        rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    // 获取所有规则
    [[nodiscard]] auto get_rules() const -> std::span<const PermissionRule> {
        return rules_;
    }

    // 记住用户对特定工具的决策（本次会话有效）
    auto remember_decision(std::string_view tool_name, PermissionDecision decision) -> void {
        std::lock_guard lock{mu_};
        session_decisions_[std::string(tool_name)] = decision;
    }

    // 清除对特定工具的会话记忆
    auto forget_decision(std::string_view tool_name) -> void {
        std::lock_guard lock{mu_};
        session_decisions_.erase(std::string(tool_name));
    }

    // 是否处于自动审批（YOLO）模式
    [[nodiscard]] auto is_auto_approve_mode() const -> bool {
        std::lock_guard lock{mu_};
        return auto_approve_;
    }

    // 设置自动审批模式
    auto set_auto_approve(bool enable) -> void {
        std::lock_guard lock{mu_};
        auto_approve_ = enable;
        if (enable) {
            log_decision("*", "", PermissionDecision::allow, "auto-approve enabled");
        }
    }

    // 获取审计日志
    [[nodiscard]] auto get_audit_log() const -> std::vector<AuditEntry> {
        std::lock_guard lock{mu_};
        return audit_log_;
    }

    // 清除审计日志
    auto clear_audit_log() -> void {
        std::lock_guard lock{mu_};
        audit_log_.clear();
    }

    // 设置工作目录（用于路径规则匹配）
    auto set_working_dir(std::string_view dir) -> void {
        std::lock_guard lock{mu_};
        working_dir_ = std::string(dir);
    }

    // 设置用户询问回调
    auto set_ask_user_fn(AskUserFn fn) -> void {
        std::lock_guard lock{mu_};
        ask_user_fn_ = std::move(fn);
    }

    // 重置所有会话状态
    auto reset_session() -> void {
        std::lock_guard lock{mu_};
        session_decisions_.clear();
        audit_log_.clear();
    }

    // 获取审计日志条目数
    [[nodiscard]] auto audit_log_size() const -> std::size_t {
        std::lock_guard lock{mu_};
        return audit_log_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<PermissionRule> rules_;
    std::unordered_map<std::string, PermissionDecision> session_decisions_;
    std::vector<AuditEntry> audit_log_;
    bool auto_approve_{false};
    std::string working_dir_;
    AskUserFn ask_user_fn_;
    static constexpr std::size_t max_audit_entries_ = 1000;

    // 记录审计日志
    auto log_decision(std::string_view tool_name, std::string_view args,
                      PermissionDecision decision, std::string_view reason) -> void {
        if (audit_log_.size() >= max_audit_entries_) {
            audit_log_.erase(audit_log_.begin()); // FIFO 淘汰
        }
        audit_log_.push_back(AuditEntry{
            .tool_name = std::string(tool_name),
            .args_summary = truncate_args(args),
            .decision = decision,
            .reason = std::string(reason),
            .timestamp = std::chrono::system_clock::now()
        });
    }

    // 从参数中提取路径（简化实现）
    [[nodiscard]] static auto extract_path(std::string_view args) -> std::string_view {
        // 工具参数通常包含 "path" 字段，这里做简化处理
        if (args.starts_with('/')) return args;
        auto pos = args.find("\"path\"");
        if (pos != std::string_view::npos) {
            auto val_start = args.find(':', pos);
            if (val_start != std::string_view::npos) {
                auto quote_start = args.find('"', val_start + 1);
                if (quote_start != std::string_view::npos) {
                    auto quote_end = args.find('"', quote_start + 1);
                    if (quote_end != std::string_view::npos) {
                        return args.substr(quote_start + 1, quote_end - quote_start - 1);
                    }
                }
            }
        }
        return {};
    }

    // 截断过长的参数摘要
    [[nodiscard]] static auto truncate_args(std::string_view args, std::size_t max_len = 80)
        -> std::string {
        if (args.size() <= max_len) return std::string(args);
        return std::string(args.substr(0, max_len - 3)) + "...";
    }
};

} // namespace cc::hooks
