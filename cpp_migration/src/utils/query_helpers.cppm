module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

export module cc.utils.query_helpers;


export namespace cc::utils {

// Prompt 段落 (用于组装系统提示词)
struct PromptSection {
    std::string id;
    std::string content;
    int priority{0};          // 高优先级先保留
    bool is_cacheable{true};  // 是否可缓存
};

// Budget 分配结果
struct BudgetAllocation {
    size_t system_tokens{0};
    size_t messages_tokens{0};
    size_t tools_tokens{0};
    size_t remaining{0};
};

// 组装系统提示词 (按优先级截断到 budget)
[[nodiscard]] inline auto build_system_prompt(
    std::vector<PromptSection> sections, size_t budget_tokens) -> std::string {
    // 按优先级降序排列
    std::sort(sections.begin(), sections.end(),
        [](const auto& a, const auto& b) { return a.priority > b.priority; });
    
    std::string result;
    size_t estimated_tokens = 0;
    
    for (const auto& section : sections) {
        size_t section_tokens = (section.content.size() + 3) / 4;  // 粗略估算
        if (estimated_tokens + section_tokens > budget_tokens) break;
        
        if (!result.empty()) result += "\n\n";
        result += section.content;
        estimated_tokens += section_tokens;
    }
    
    return result;
}

// 分配 token 预算
[[nodiscard]] inline auto allocate_budget(size_t total_tokens, size_t system_estimate,
    size_t tools_estimate) -> BudgetAllocation {
    BudgetAllocation alloc;
    alloc.system_tokens = std::min(system_estimate, total_tokens / 4);      // 系统最多占 25%
    alloc.tools_tokens = std::min(tools_estimate, total_tokens / 10);       // 工具最多占 10%
    alloc.messages_tokens = total_tokens - alloc.system_tokens - alloc.tools_tokens;
    alloc.remaining = 0;
    return alloc;
}

// 判断是否需要压缩
[[nodiscard]] inline auto should_compact(size_t current_tokens, size_t context_window,
    double threshold = 0.8) -> bool {
    return static_cast<double>(current_tokens) > static_cast<double>(context_window) * threshold;
}

// 估算上下文使用量
[[nodiscard]] inline auto estimate_context_usage(size_t system_tokens, size_t messages_tokens,
    size_t tools_tokens) -> size_t {
    return system_tokens + messages_tokens + tools_tokens;
}

} // namespace cc::utils
