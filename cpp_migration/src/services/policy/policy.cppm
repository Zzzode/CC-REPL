/// @file policy.cppm
/// @brief Organization policy enforcement service.
/// Loads and enforces policies including tool restrictions, model restrictions,
/// network access policies, rate limits, and provides a policy evaluation engine.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>

export module cc.services.policy;

import cc.types.types;

export namespace cc::services::policy {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================
// 策略数据结构
// ============================================================

// 策略执行动作
enum class PolicyAction : std::uint8_t {
    Allow,     // 允许
    Deny,      // 拒绝
    Warn,      // 警告但允许
    Audit,     // 允许并记录审计日志
};

// 工具限制规则
struct ToolRestriction {
    std::string tool_name;          // 工具名 (或 "*" 表示所有)
    PolicyAction action{PolicyAction::Allow};
    std::string reason;
    std::vector<std::string> allowed_args;  // 允许的参数模式
};

// 模型限制规则
struct ModelRestriction {
    std::string model_pattern;      // 模型名匹配模式
    PolicyAction action{PolicyAction::Allow};
    std::size_t max_tokens_per_request{0};  // 0 = 无限制
    std::string reason;
};

// 网络访问策略
struct NetworkPolicy {
    std::string host_pattern;       // 主机匹配模式
    PolicyAction action{PolicyAction::Allow};
    std::vector<std::string> allowed_ports;
    std::string reason;
};

// 速率限制规则
struct RateLimitRule {
    std::string resource;           // 受限资源标识
    std::size_t max_requests{100};  // 窗口内最大请求数
    std::chrono::seconds window{60};
    PolicyAction on_exceed{PolicyAction::Deny};
};

// 策略评估结果
struct PolicyEvaluation {
    PolicyAction action{PolicyAction::Allow};
    std::string matched_rule;       // 匹配的规则名
    std::string reason;
    TimePoint evaluated_at;
};

// 策略集合
struct PolicySet {
    std::string name;
    std::string version;
    std::vector<ToolRestriction> tool_rules;
    std::vector<ModelRestriction> model_rules;
    std::vector<NetworkPolicy> network_rules;
    std::vector<RateLimitRule> rate_limits;
    TimePoint loaded_at;
};

// 速率限制追踪器
struct RateTracker {
    std::size_t request_count{0};
    TimePoint window_start;
};

// ============================================================
// PolicyLimitsService - 策略加载与执行
// ============================================================

class PolicyLimitsService {
public:
    PolicyLimitsService() = default;

    // 加载策略集
    VoidResult load_policy(PolicySet policy) {
        if (policy.name.empty()) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "policy name required"});
        }
        policy.loaded_at = Clock::now();
        active_policy_ = std::move(policy);
        return {};
    }

    // 评估工具使用权限
    [[nodiscard]] PolicyEvaluation evaluate_tool(std::string_view tool_name) const {
        if (!active_policy_) return {PolicyAction::Allow, "", "no policy loaded"};
        for (const auto& rule : active_policy_->tool_rules) {
            if (matches_pattern(tool_name, rule.tool_name)) {
                return {
                    .action = rule.action,
                    .matched_rule = std::format("tool:{}", rule.tool_name),
                    .reason = rule.reason,
                    .evaluated_at = Clock::now(),
                };
            }
        }
        return {PolicyAction::Allow, "", "no matching rule", Clock::now()};
    }

    // 评估模型使用权限
    [[nodiscard]] PolicyEvaluation evaluate_model(
        std::string_view model_name, std::size_t requested_tokens = 0) const
    {
        if (!active_policy_) return {PolicyAction::Allow, "", "no policy loaded"};
        for (const auto& rule : active_policy_->model_rules) {
            if (matches_pattern(model_name, rule.model_pattern)) {
                if (rule.action == PolicyAction::Allow &&
                    rule.max_tokens_per_request > 0 &&
                    requested_tokens > rule.max_tokens_per_request) {
                    return {PolicyAction::Deny, std::format("model:{}", rule.model_pattern),
                            std::format("exceeds token limit: {}", rule.max_tokens_per_request),
                            Clock::now()};
                }
                return {rule.action, std::format("model:{}", rule.model_pattern),
                        rule.reason, Clock::now()};
            }
        }
        return {PolicyAction::Allow, "", "no matching rule", Clock::now()};
    }

    // 评估网络访问权限
    [[nodiscard]] PolicyEvaluation evaluate_network(std::string_view host) const {
        if (!active_policy_) return {PolicyAction::Allow, "", "no policy loaded"};
        for (const auto& rule : active_policy_->network_rules) {
            if (matches_pattern(host, rule.host_pattern)) {
                return {rule.action, std::format("net:{}", rule.host_pattern),
                        rule.reason, Clock::now()};
            }
        }
        return {PolicyAction::Allow, "", "no matching rule", Clock::now()};
    }

    // 检查速率限制
    [[nodiscard]] PolicyEvaluation check_rate_limit(const std::string& resource) {
        if (!active_policy_) return {PolicyAction::Allow, "", "no policy loaded"};
        for (const auto& rule : active_policy_->rate_limits) {
            if (rule.resource == resource) {
                auto& tracker = rate_trackers_[resource];
                auto now = Clock::now();
                // 检查窗口是否已过期
                if (now - tracker.window_start >= rule.window) {
                    tracker = {.request_count = 1, .window_start = now};
                    return {PolicyAction::Allow, "", "within limit", now};
                }
                ++tracker.request_count;
                if (tracker.request_count > rule.max_requests) {
                    return {rule.on_exceed, std::format("rate:{}", resource),
                            std::format("rate limit exceeded: {}/{}", tracker.request_count, rule.max_requests),
                            now};
                }
                return {PolicyAction::Allow, "", "within limit", now};
            }
        }
        return {PolicyAction::Allow, "", "no rate limit", Clock::now()};
    }

    // 获取当前策略信息
    [[nodiscard]] std::optional<std::string> policy_name() const {
        return active_policy_ ? std::optional{active_policy_->name} : std::nullopt;
    }

    [[nodiscard]] bool has_policy() const noexcept { return active_policy_.has_value(); }

    // 清除策略
    void clear_policy() noexcept { active_policy_.reset(); }

private:
    std::optional<PolicySet> active_policy_;
    std::unordered_map<std::string, RateTracker> rate_trackers_;

    // 简单模式匹配 (支持 * 通配符)
    static bool matches_pattern(std::string_view value, std::string_view pattern) noexcept {
        if (pattern == "*") return true;
        if (pattern.ends_with('*')) {
            auto prefix = pattern.substr(0, pattern.size() - 1);
            return value.starts_with(prefix);
        }
        return value == pattern;
    }
};

} // namespace cc::services::policy
