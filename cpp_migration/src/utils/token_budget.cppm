// C++23 Module: Token budget management
// Token 预算管理：追踪和分配各类 token 使用量
module;
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.token_budget;


export namespace cc::utils::token_budget {

struct TokenBudgetPosition {
    std::size_t start{0};
    std::size_t end{0};
};

[[nodiscard]] inline std::size_t multiplier_for_suffix(char suffix) {
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(suffix)))) {
        case 'k': return 1'000;
        case 'm': return 1'000'000;
        case 'b': return 1'000'000'000;
        default: return 1;
    }
}

[[nodiscard]] inline std::size_t parse_budget_match(const std::string& value, char suffix) {
    return static_cast<std::size_t>(std::llround(std::stod(value) * multiplier_for_suffix(suffix)));
}

[[nodiscard]] inline std::optional<std::size_t> parse_token_budget(std::string_view text) {
    const std::string input(text);
    static const std::regex shorthand_start(R"(^\s*\+(\d+(?:\.\d+)?)\s*([kmb])\b)", std::regex::icase);
    static const std::regex shorthand_end(R"(\s\+(\d+(?:\.\d+)?)\s*([kmb])\s*[.!?]?\s*$)", std::regex::icase);
    static const std::regex verbose(R"(\b(?:use|spend)\s+(\d+(?:\.\d+)?)\s*([kmb])\s*tokens?\b)", std::regex::icase);

    std::smatch match;
    if (std::regex_search(input, match, shorthand_start)) return parse_budget_match(match[1].str(), match[2].str()[0]);
    if (std::regex_search(input, match, shorthand_end)) return parse_budget_match(match[1].str(), match[2].str()[0]);
    if (std::regex_search(input, match, verbose)) return parse_budget_match(match[1].str(), match[2].str()[0]);
    return std::nullopt;
}

[[nodiscard]] inline std::vector<TokenBudgetPosition> find_token_budget_positions(std::string_view text) {
    const std::string input(text);
    std::vector<TokenBudgetPosition> positions;
    static const std::regex shorthand_start(R"(^\s*\+(\d+(?:\.\d+)?)\s*([kmb])\b)", std::regex::icase);
    static const std::regex shorthand_end(R"(\s\+(\d+(?:\.\d+)?)\s*([kmb])\s*[.!?]?\s*$)", std::regex::icase);
    static const std::regex verbose(R"(\b(?:use|spend)\s+(\d+(?:\.\d+)?)\s*([kmb])\s*tokens?\b)", std::regex::icase);

    std::smatch match;
    if (std::regex_search(input, match, shorthand_start)) {
        auto raw = match[0].str();
        auto leading = raw.find_first_not_of(" \t\n\r");
        if (leading == std::string::npos) leading = 0;
        positions.push_back({static_cast<std::size_t>(match.position(0)) + leading,
                             static_cast<std::size_t>(match.position(0) + match.length(0))});
    }
    if (std::regex_search(input, match, shorthand_end)) {
        std::size_t end_start = static_cast<std::size_t>(match.position(0)) + 1;
        bool covered = false;
        for (const auto& pos : positions) {
            if (end_start >= pos.start && end_start < pos.end) { covered = true; break; }
        }
        if (!covered) {
            positions.push_back({end_start, static_cast<std::size_t>(match.position(0) + match.length(0))});
        }
    }
    for (auto it = std::sregex_iterator(input.begin(), input.end(), verbose); it != std::sregex_iterator(); ++it) {
        positions.push_back({static_cast<std::size_t>(it->position(0)),
                             static_cast<std::size_t>(it->position(0) + it->length(0))});
    }
    return positions;
}

[[nodiscard]] inline std::string format_with_commas(std::size_t value) {
    auto digits = std::to_string(value);
    std::string result;
    auto first_group = digits.size() % 3;
    if (first_group == 0) first_group = 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i == first_group || (i > first_group && (i - first_group) % 3 == 0))) result += ',';
        result += digits[i];
    }
    return result;
}

[[nodiscard]] inline std::string get_budget_continuation_message(std::size_t pct, std::size_t turn_tokens, std::size_t budget) {
    return std::format("Stopped at {}% of token target ({} / {}). Keep working — do not summarize.",
                       pct, format_with_commas(turn_tokens), format_with_commas(budget));
}

// Token 预算配置结构
struct TokenBudget {
    size_t max_context{128000};        // 最大上下文窗口
    size_t max_output{4096};           // 最大输出 token 数
    size_t reserved_for_tools{8000};   // 工具 schema 预留
    size_t reserved_for_system{2000};  // 系统 prompt 预留

    // 可用于用户消息的 token 数
    [[nodiscard]] size_t available_for_messages() const {
        auto reserved = reserved_for_tools + reserved_for_system + max_output;
        return max_context > reserved ? max_context - reserved : 0;
    }
};

// 使用量分类枚举
enum class BudgetCategory : uint8_t {
    SystemPrompt,
    UserMessages,
    AssistantMessages,
    ToolSchemas,
    ToolResults,
    Reserved
};

// 分类使用量条目
struct CategoryUsage {
    BudgetCategory category;
    size_t tokens_used{0};
    size_t tokens_limit{0};
};

// Token 估算器：基于字符的简单估算
class TokenEstimator {
public:
    // 估算文本的 token 数 (简单的字符比率估算)
    [[nodiscard]] size_t estimate_tokens(std::string_view text) const {
        if (text.empty()) return 0;
        // 英文约 4 字符/token，中文约 2 字符/token
        size_t ascii_chars = 0;
        size_t non_ascii_chars = 0;

        for (unsigned char c : text) {
            if (c < 128) ++ascii_chars;
            else ++non_ascii_chars;
        }

        // 中文字符按 UTF-8 编码约 3 字节/字，每字约 1 token
        size_t cjk_tokens = non_ascii_chars / 3;
        size_t ascii_tokens = ascii_chars / 4;

        return ascii_tokens + cjk_tokens + 1;  // +1 避免零
    }

    // 估算一条消息的 token 数 (含开销)
    [[nodiscard]] size_t estimate_message_tokens(
        std::string_view role, std::string_view content) const {
        // 每条消息有固定开销 (role 标记、分隔符等)
        constexpr size_t message_overhead = 4;
        return message_overhead + estimate_tokens(role) + estimate_tokens(content);
    }

    // 估算工具 schema 的 token 数
    [[nodiscard]] size_t estimate_tool_schema_tokens(
        const std::vector<std::string>& tool_schemas) const {
        size_t total = 0;
        for (const auto& schema : tool_schemas) {
            // JSON schema 通常比纯文本更密集
            total += estimate_tokens(schema) + 10;  // 每个工具额外开销
        }
        return total;
    }

    // 设置每 token 平均字符数 (可调参)
    void set_chars_per_token(double ratio) {
        chars_per_token_ = ratio;
    }

private:
    double chars_per_token_{4.0};
};

// 预算分配报告
struct UsageReport {
    std::vector<CategoryUsage> categories;
    size_t total_used{0};
    size_t total_limit{0};
    double utilization{0.0};  // 使用率 (0.0 ~ 1.0)

    [[nodiscard]] std::string format() const {
        std::string report = std::format("Token Usage: {}/{} ({:.1f}%)\n",
            total_used, total_limit, utilization * 100.0);
        for (const auto& cat : categories) {
            report += std::format("  {:20s}: {:6}/{:6}\n",
                category_name(cat.category), cat.tokens_used, cat.tokens_limit);
        }
        return report;
    }

private:
    [[nodiscard]] static std::string_view category_name(BudgetCategory cat) {
        switch (cat) {
            case BudgetCategory::SystemPrompt:      return "System Prompt";
            case BudgetCategory::UserMessages:      return "User Messages";
            case BudgetCategory::AssistantMessages: return "Assistant Messages";
            case BudgetCategory::ToolSchemas:       return "Tool Schemas";
            case BudgetCategory::ToolResults:       return "Tool Results";
            case BudgetCategory::Reserved:          return "Reserved";
        }
        return "Unknown";
    }
};

// 预算管理器：分配、追踪和报告 token 使用情况
class BudgetManager {
public:
    explicit BudgetManager(TokenBudget budget = {})
        : budget_(budget) {
        // 初始化各分类的限额
        allocations_ = {
            {BudgetCategory::SystemPrompt,      0, budget_.reserved_for_system},
            {BudgetCategory::UserMessages,      0, budget_.available_for_messages() / 2},
            {BudgetCategory::AssistantMessages, 0, budget_.available_for_messages() / 2},
            {BudgetCategory::ToolSchemas,       0, budget_.reserved_for_tools},
            {BudgetCategory::ToolResults,       0, budget_.reserved_for_tools},
            {BudgetCategory::Reserved,          0, budget_.max_output},
        };
    }

    // 为某分类分配 token，返回剩余可用量
    [[nodiscard]] size_t allocate(BudgetCategory category, size_t tokens) {
        for (auto& alloc : allocations_) {
            if (alloc.category == category) {
                alloc.tokens_used += tokens;
                total_used_ += tokens;
                return alloc.tokens_limit > alloc.tokens_used
                    ? alloc.tokens_limit - alloc.tokens_used : 0;
            }
        }
        return 0;
    }

    // 检查是否可容纳指定数量的 token
    [[nodiscard]] bool can_fit(size_t tokens) const {
        return (total_used_ + tokens) <= budget_.max_context;
    }

    // 生成使用报告
    [[nodiscard]] UsageReport usage_report() const {
        UsageReport report;
        report.categories = allocations_;
        report.total_used = total_used_;
        report.total_limit = budget_.max_context;
        report.utilization = budget_.max_context > 0
            ? static_cast<double>(total_used_) / static_cast<double>(budget_.max_context)
            : 0.0;
        return report;
    }

    // 判断是否应该压缩上下文
    [[nodiscard]] bool should_compact() const {
        // 当使用率超过 80% 时建议压缩
        constexpr double compact_threshold = 0.80;
        double utilization = budget_.max_context > 0
            ? static_cast<double>(total_used_) / static_cast<double>(budget_.max_context)
            : 0.0;
        return utilization > compact_threshold;
    }

    // 获取剩余可用 token 数
    [[nodiscard]] size_t remaining() const {
        return budget_.max_context > total_used_
            ? budget_.max_context - total_used_ : 0;
    }

    // 重置所有使用量
    void reset() {
        total_used_ = 0;
        for (auto& alloc : allocations_) {
            alloc.tokens_used = 0;
        }
    }

    // 获取预算配置
    [[nodiscard]] const TokenBudget& budget() const { return budget_; }

    // 更新预算配置 (切换模型时)
    void update_budget(TokenBudget new_budget) {
        budget_ = new_budget;
        // 重新计算限额
        allocations_[0].tokens_limit = budget_.reserved_for_system;
        allocations_[1].tokens_limit = budget_.available_for_messages() / 2;
        allocations_[2].tokens_limit = budget_.available_for_messages() / 2;
        allocations_[3].tokens_limit = budget_.reserved_for_tools;
        allocations_[4].tokens_limit = budget_.reserved_for_tools;
        allocations_[5].tokens_limit = budget_.max_output;
    }

private:
    TokenBudget budget_;
    std::vector<CategoryUsage> allocations_;
    size_t total_used_{0};
};

} // namespace cc::utils::token_budget
