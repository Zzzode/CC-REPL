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


enum class PermissionDecision {
    allow,
    deny,
    ask_user,
    allow_once,
};


struct PermissionRule {
    std::string tool_pattern;
    std::string path_pattern;
    PermissionDecision decision{PermissionDecision::allow};
    std::string reason;


    [[nodiscard]] auto matches(std::string_view tool_name,
                                std::string_view path) const -> bool {
        return matches_pattern(tool_pattern, tool_name) &&
               (path_pattern.empty() || matches_pattern(path_pattern, path));
    }

private:

    [[nodiscard]] static auto matches_pattern(std::string_view pattern,
                                               std::string_view value) -> bool {
        if (pattern == "*") return true;
        if (pattern == value) return true;


        if (pattern.ends_with('*')) {
            auto prefix = pattern.substr(0, pattern.size() - 1);
            return value.starts_with(prefix);
        }

        if (pattern.starts_with('*')) {
            auto suffix = pattern.substr(1);
            return value.ends_with(suffix);
        }
        return false;
    }
};


struct PermissionContext {
    std::string tool_name;
    std::string args;
    std::string working_dir;
    std::vector<PermissionRule> session_rules;
};


struct AuditEntry {
    std::string tool_name;
    std::string args_summary;
    PermissionDecision decision;
    std::string reason;
    std::chrono::system_clock::time_point timestamp;


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


using AskUserFn = std::function<PermissionDecision(const PermissionContext&)>;


class ToolPermissionHook {
public:
    ToolPermissionHook() = default;

    /**
     * Check whether a tool may run in the current context.
     * Rules are matched by priority, and auto-approval mode skips prompts.
     */
    [[nodiscard]] auto can_use(std::string_view tool_name,
                                std::string_view args = "") -> PermissionDecision {
        std::lock_guard lock{mu_};


        if (auto_approve_) {
            auto decision = PermissionDecision::allow;
            log_decision(tool_name, args, decision, "auto-approve mode");
            return decision;
        }


        if (auto it = session_decisions_.find(std::string(tool_name));
            it != session_decisions_.end()) {
            log_decision(tool_name, args, it->second, "session memory");
            return it->second;
        }


        for (const auto& rule : rules_) {
            if (rule.matches(tool_name, extract_path(args))) {
                log_decision(tool_name, args, rule.decision, rule.reason);
                return rule.decision;
            }
        }


        auto decision = PermissionDecision::ask_user;


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


    auto add_rule(PermissionRule rule) -> void {
        std::lock_guard lock{mu_};
        rules_.push_back(std::move(rule));
    }


    auto remove_rule(std::size_t index) -> bool {
        std::lock_guard lock{mu_};
        if (index >= rules_.size()) return false;
        rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }


    [[nodiscard]] auto get_rules() const -> std::span<const PermissionRule> {
        return rules_;
    }


    auto remember_decision(std::string_view tool_name, PermissionDecision decision) -> void {
        std::lock_guard lock{mu_};
        session_decisions_[std::string(tool_name)] = decision;
    }


    auto forget_decision(std::string_view tool_name) -> void {
        std::lock_guard lock{mu_};
        session_decisions_.erase(std::string(tool_name));
    }


    [[nodiscard]] auto is_auto_approve_mode() const -> bool {
        std::lock_guard lock{mu_};
        return auto_approve_;
    }


    auto set_auto_approve(bool enable) -> void {
        std::lock_guard lock{mu_};
        auto_approve_ = enable;
        if (enable) {
            log_decision("*", "", PermissionDecision::allow, "auto-approve enabled");
        }
    }


    [[nodiscard]] auto get_audit_log() const -> std::vector<AuditEntry> {
        std::lock_guard lock{mu_};
        return audit_log_;
    }


    auto clear_audit_log() -> void {
        std::lock_guard lock{mu_};
        audit_log_.clear();
    }


    auto set_working_dir(std::string_view dir) -> void {
        std::lock_guard lock{mu_};
        working_dir_ = std::string(dir);
    }


    auto set_ask_user_fn(AskUserFn fn) -> void {
        std::lock_guard lock{mu_};
        ask_user_fn_ = std::move(fn);
    }


    auto reset_session() -> void {
        std::lock_guard lock{mu_};
        session_decisions_.clear();
        audit_log_.clear();
    }


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


    auto log_decision(std::string_view tool_name, std::string_view args,
                      PermissionDecision decision, std::string_view reason) -> void {
        if (audit_log_.size() >= max_audit_entries_) {
            audit_log_.erase(audit_log_.begin());
        }
        audit_log_.push_back(AuditEntry{
            .tool_name = std::string(tool_name),
            .args_summary = truncate_args(args),
            .decision = decision,
            .reason = std::string(reason),
            .timestamp = std::chrono::system_clock::now()
        });
    }


    [[nodiscard]] static auto extract_path(std::string_view args) -> std::string_view {

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


    [[nodiscard]] static auto truncate_args(std::string_view args, std::size_t max_len = 80)
        -> std::string {
        if (args.size() <= max_len) return std::string(args);
        return std::string(args.substr(0, max_len - 3)) + "...";
    }
};

} // namespace cc::hooks
