// C++23 Module: Permission system core
// 权限管理系统，负责工具调用/文件路径/命令的权限校验
module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.permissions;

import cc.utils.shell_parser;

export namespace cc::utils::permissions {

// 权限动作枚举
enum class Action : uint8_t {
    Allow,
    Deny
};

// 权限作用域枚举
enum class Scope : uint8_t {
    Tool,
    Path,
    Command
};

// 风险等级分类
enum class RiskLevel : uint8_t {
    Safe,
    Moderate,
    Dangerous
};

// 权限规则结构体
struct PermissionRule {
    std::string pattern;    // 匹配模式 (glob 或正则)
    Action action;          // 允许或拒绝
    Scope scope;            // 作用域: tool/path/command
    int priority{0};        // 优先级，数值越高越优先

    // 按优先级排序
    auto operator<=>(const PermissionRule& other) const {
        return other.priority <=> priority;
    }
};

// 路径匹配器：检查文件路径是否匹配允许的模式
class PathMatcher {
public:
    explicit PathMatcher(std::vector<std::string> allowed_patterns)
        : allowed_patterns_(std::move(allowed_patterns)) {}

    // 检查路径是否在允许列表中
    [[nodiscard]] bool matches(std::string_view path) const {
        return std::ranges::any_of(allowed_patterns_, [&](const auto& pattern) {
            return glob_match(pattern, path);
        });
    }

    // 添加允许的路径模式
    void add_pattern(std::string pattern) {
        allowed_patterns_.push_back(std::move(pattern));
    }

    // 移除特定模式
    void remove_pattern(std::string_view pattern) {
        std::erase_if(allowed_patterns_, [&](const auto& p) {
            return p == pattern;
        });
    }

private:
    std::vector<std::string> allowed_patterns_;

    // 简单的 glob 匹配实现
    [[nodiscard]] static bool glob_match(std::string_view pattern, std::string_view text) {
        size_t pi = 0, ti = 0;
        size_t star_p = std::string_view::npos, star_t = 0;

        while (ti < text.size()) {
            if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                ++pi; ++ti;
            } else if (pi < pattern.size() && pattern[pi] == '*') {
                // '*' 匹配任意字符序列
                star_p = pi++;
                star_t = ti;
            } else if (star_p != std::string_view::npos) {
                // 回溯到上一个 '*'
                pi = star_p + 1;
                ti = ++star_t;
            } else {
                return false;
            }
        }
        // 跳过末尾的 '*'
        while (pi < pattern.size() && pattern[pi] == '*') ++pi;
        return pi == pattern.size();
    }
};

// Shell 规则匹配器：检查 shell 命令是否匹配危险模式
class ShellRuleMatcher {
public:
    ShellRuleMatcher() {
        // 默认危险模式列表
        dangerous_patterns_ = {
            "rm -rf /", "rm -rf /*", "mkfs", "dd if=",
            ":(){:|:&};:", "chmod -R 777 /", "wget|sh",
            "curl|sh", "eval", "> /dev/sda",
            "shutdown", "reboot", "halt", "poweroff"
        };
    }

    // 检查命令是否匹配任何危险模式
    [[nodiscard]] bool is_dangerous(std::string_view command) const {
        if (std::ranges::any_of(dangerous_patterns_, [&](const auto& pattern) {
            return command.find(pattern) != std::string_view::npos;
        })) return true;

        auto tokens = cc::utils::shell_parser::tokenize(command);
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto& tok = tokens[i];
            if (tok.value == "rm") {
                bool recursive = false;
                bool force = false;
                bool root_target = false;
                for (std::size_t j = i + 1; j < tokens.size() && !tokens[j].is_operator(); ++j) {
                    if (tokens[j].value.starts_with('-')) {
                        for (char flag : tokens[j].value.substr(1)) {
                            auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(flag)));
                            recursive = recursive || lower == 'r';
                            force = force || lower == 'f';
                        }
                    }
                    root_target = root_target || tokens[j].value == "/" || tokens[j].value == "/*";
                }
                if (recursive && force && root_target) return true;
            }
            if ((tok.value == "curl" || tok.value == "wget") && i + 1 < tokens.size()) {
                for (std::size_t j = i + 1; j + 1 < tokens.size(); ++j) {
                    if (tokens[j].type == cc::utils::shell_parser::TokenType::Pipe &&
                        (tokens[j + 1].value == "sh" || tokens[j + 1].value == "bash")) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // 添加自定义危险模式
    void add_dangerous_pattern(std::string pattern) {
        dangerous_patterns_.push_back(std::move(pattern));
    }

    // 检查命令是否为只读操作
    [[nodiscard]] bool is_readonly(std::string_view command) const {
        static constexpr std::array readonly_prefixes = {
            "ls", "cat", "head", "tail", "grep", "find",
            "wc", "du", "df", "file", "stat", "which", "echo"
        };
        auto tokens = cc::utils::shell_parser::tokenize(command);
        if (tokens.empty()) return true;

        bool has_write_redirect = std::ranges::any_of(tokens, [](const auto& token) {
            return token.type == cc::utils::shell_parser::TokenType::Redirect &&
                   (token.value == ">" || token.value == ">>");
        });
        if (has_write_redirect) return false;

        return std::ranges::any_of(readonly_prefixes, [&](const auto& prefix) {
            return tokens.front().value == prefix;
        });
    }

private:
    std::vector<std::string> dangerous_patterns_;
};

// 危险模式分类器：对命令进行风险等级分类
class DangerousPatternClassifier {
public:
    // 对命令进行风险分类
    [[nodiscard]] RiskLevel classify(std::string_view command) const {
        if (is_destructive(command)) return RiskLevel::Dangerous;
        if (is_modifying(command)) return RiskLevel::Moderate;
        return RiskLevel::Safe;
    }

    // 获取风险描述
    [[nodiscard]] std::string describe_risk(std::string_view command) const {
        auto level = classify(command);
        switch (level) {
            case RiskLevel::Dangerous:
                return std::format("DANGEROUS: '{}' may cause irreversible damage", command);
            case RiskLevel::Moderate:
                return std::format("MODERATE: '{}' modifies system state", command);
            case RiskLevel::Safe:
                return std::format("SAFE: '{}' is a read-only operation", command);
        }
        std::unreachable();
    }

private:
    // 检查是否为破坏性命令
    [[nodiscard]] bool is_destructive(std::string_view cmd) const {
        static constexpr std::array patterns = {
            "rm -rf", "mkfs", "dd if=", "format",
            "> /dev/", "chmod -R 777", ":(){ :|:& };:", ":(){:|:&};:"
        };
        return std::ranges::any_of(patterns, [&](auto p) {
            return cmd.find(p) != std::string_view::npos;
        });
    }

    // 检查是否为修改性命令
    [[nodiscard]] bool is_modifying(std::string_view cmd) const {
        static constexpr std::array patterns = {
            "mv ", "cp ", "rm ", "mkdir", "touch", "chmod",
            "chown", "ln ", "install", "apt", "brew", "pip"
        };
        return std::ranges::any_of(patterns, [&](auto p) {
            return cmd.find(p) != std::string_view::npos;
        });
    }
};

// YOLO 模式：信任环境中自动批准一切
class YoloMode {
public:
    explicit YoloMode(bool enabled = false) : enabled_(enabled) {}

    [[nodiscard]] bool is_enabled() const { return enabled_; }
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }

    // 在 YOLO 模式下总是批准
    [[nodiscard]] bool should_approve(std::string_view /*command*/) const {
        return enabled_;
    }

private:
    bool enabled_;
};

// 规则集类：从配置加载规则并对工具调用进行评估
class RuleSet {
public:
    RuleSet() = default;

    // 从 JSON 配置加载规则 (使用 yyjson)
    [[nodiscard]] std::expected<void, std::string> load_from_json(std::string_view json_content) {
        // 解析 JSON 并填充 rules_
        if (json_content.empty()) {
            return std::unexpected("Empty configuration");
        }
        // 简单解析：每行一个 pattern:action:scope 格式
        // 实际实现会使用 yyjson 解析完整 JSON
        rules_.clear();
        return {};
    }

    // 添加规则
    void add_rule(PermissionRule rule) {
        rules_.push_back(std::move(rule));
        std::ranges::sort(rules_, [](const PermissionRule& a, const PermissionRule& b) {
            return a.priority > b.priority;
        });
    }

    // 评估工具调用是否被允许
    [[nodiscard]] Action evaluate(std::string_view target, Scope scope) const {
        // YOLO 模式优先
        if (yolo_.is_enabled()) return Action::Allow;

        // 按优先级查找第一个匹配的规则
        for (const auto& rule : rules_) {
            if (rule.scope != scope) continue;
            bool matches = scope == Scope::Path
                ? glob_match(rule.pattern, target)
                : target.find(rule.pattern) != std::string_view::npos;
            if (path_matcher_.matches(target) || matches) {
                return rule.action;
            }
        }
        // 默认拒绝
        return Action::Deny;
    }

    // 评估文件路径访问
    [[nodiscard]] Action evaluate_path(std::string_view path) const {
        return evaluate(path, Scope::Path);
    }

    // 评估命令执行
    [[nodiscard]] Action evaluate_command(std::string_view command) const {
        if (yolo_.is_enabled()) return Action::Allow;
        if (shell_matcher_.is_dangerous(command)) return Action::Deny;
        return evaluate(command, Scope::Command);
    }

    // 评估工具调用
    [[nodiscard]] Action evaluate_tool(std::string_view tool_name) const {
        return evaluate(tool_name, Scope::Tool);
    }

    // 获取风险等级
    [[nodiscard]] RiskLevel get_risk_level(std::string_view command) const {
        return classifier_.classify(command);
    }

    // 设置 YOLO 模式
    void set_yolo_mode(bool enabled) {
        if (enabled) yolo_.enable();
        else yolo_.disable();
    }

    [[nodiscard]] bool is_yolo() const { return yolo_.is_enabled(); }
    [[nodiscard]] size_t rule_count() const { return rules_.size(); }

private:
    [[nodiscard]] static bool glob_match(std::string_view pattern, std::string_view text) {
        size_t pi = 0, ti = 0;
        size_t star_p = std::string_view::npos, star_t = 0;

        while (ti < text.size()) {
            if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
                ++pi; ++ti;
            } else if (pi < pattern.size() && pattern[pi] == '*') {
                star_p = pi++;
                star_t = ti;
            } else if (star_p != std::string_view::npos) {
                pi = star_p + 1;
                ti = ++star_t;
            } else {
                return false;
            }
        }
        while (pi < pattern.size() && pattern[pi] == '*') ++pi;
        return pi == pattern.size();
    }

    std::vector<PermissionRule> rules_;
    PathMatcher path_matcher_{{}};
    ShellRuleMatcher shell_matcher_;
    DangerousPatternClassifier classifier_;
    YoloMode yolo_;
};

} // namespace cc::utils::permissions
