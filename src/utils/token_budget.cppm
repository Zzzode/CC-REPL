// C++23 Module: Token budget management

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


struct TokenBudget {
    size_t max_context{128000};
    size_t max_output{4096};
    size_t reserved_for_tools{8000};
    size_t reserved_for_system{2000};


    [[nodiscard]] size_t available_for_messages() const {
        auto reserved = reserved_for_tools + reserved_for_system + max_output;
        return max_context > reserved ? max_context - reserved : 0;
    }
};


enum class BudgetCategory : uint8_t {
    SystemPrompt,
    UserMessages,
    AssistantMessages,
    ToolSchemas,
    ToolResults,
    Reserved
};


struct CategoryUsage {
    BudgetCategory category;
    size_t tokens_used{0};
    size_t tokens_limit{0};
};


class TokenEstimator {
public:

    [[nodiscard]] size_t estimate_tokens(std::string_view text) const {
        if (text.empty()) return 0;

        size_t ascii_chars = 0;
        size_t non_ascii_chars = 0;

        for (unsigned char c : text) {
            if (c < 128) ++ascii_chars;
            else ++non_ascii_chars;
        }


        size_t cjk_tokens = non_ascii_chars / 3;
        size_t ascii_tokens = ascii_chars / 4;

        return ascii_tokens + cjk_tokens + 1;
    }


    [[nodiscard]] size_t estimate_message_tokens(
        std::string_view role, std::string_view content) const {

        constexpr size_t message_overhead = 4;
        return message_overhead + estimate_tokens(role) + estimate_tokens(content);
    }


    [[nodiscard]] size_t estimate_tool_schema_tokens(
        const std::vector<std::string>& tool_schemas) const {
        size_t total = 0;
        for (const auto& schema : tool_schemas) {

            total += estimate_tokens(schema) + 10;
        }
        return total;
    }


    void set_chars_per_token(double ratio) {
        chars_per_token_ = ratio;
    }

private:
    double chars_per_token_{4.0};
};


struct UsageReport {
    std::vector<CategoryUsage> categories;
    size_t total_used{0};
    size_t total_limit{0};
    double utilization{0.0};

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


class BudgetManager {
public:
    explicit BudgetManager(TokenBudget budget = {})
        : budget_(budget) {

        allocations_ = {
            {BudgetCategory::SystemPrompt,      0, budget_.reserved_for_system},
            {BudgetCategory::UserMessages,      0, budget_.available_for_messages() / 2},
            {BudgetCategory::AssistantMessages, 0, budget_.available_for_messages() / 2},
            {BudgetCategory::ToolSchemas,       0, budget_.reserved_for_tools},
            {BudgetCategory::ToolResults,       0, budget_.reserved_for_tools},
            {BudgetCategory::Reserved,          0, budget_.max_output},
        };
    }


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


    [[nodiscard]] bool can_fit(size_t tokens) const {
        return (total_used_ + tokens) <= budget_.max_context;
    }


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


    [[nodiscard]] bool should_compact() const {

        constexpr double compact_threshold = 0.80;
        double utilization = budget_.max_context > 0
            ? static_cast<double>(total_used_) / static_cast<double>(budget_.max_context)
            : 0.0;
        return utilization > compact_threshold;
    }


    [[nodiscard]] size_t remaining() const {
        return budget_.max_context > total_used_
            ? budget_.max_context - total_used_ : 0;
    }


    void reset() {
        total_used_ = 0;
        for (auto& alloc : allocations_) {
            alloc.tokens_used = 0;
        }
    }


    [[nodiscard]] const TokenBudget& budget() const { return budget_; }


    void update_budget(TokenBudget new_budget) {
        budget_ = new_budget;

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
