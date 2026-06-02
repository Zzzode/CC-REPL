module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>

export module cc.commands.review.ultrareview;

export namespace cc::commands {

// UltraReview 配额信息
struct UltraReviewQuota {
    int remaining;
    int total;
};

// UltraReview 配置
struct ReviewConfig {
    std::string target;                  // 审查目标（文件/目录/PR）
    bool auto_fix;                       // 是否自动修复问题
    std::optional<std::string> focus_area; // 关注领域（安全/性能/风格等）
    int max_issues;                      // 最大报告问题数
};

// 执行 UltraReview 深度代码审查
auto run_ultrareview(ReviewConfig config) -> std::expected<std::string, std::string> {
    if (config.target.empty()) {
        return std::unexpected("Review target cannot be empty");
    }
    if (config.max_issues <= 0) {
        config.max_issues = 50; // 默认最大问题数
    }
    std::string summary = "UltraReview prepared for " + config.target + "\n";
    summary += "Max issues: " + std::to_string(config.max_issues) + "\n";
    summary += "Auto-fix: " + std::string(config.auto_fix ? "enabled" : "disabled") + "\n";
    if (config.focus_area) summary += "Focus: " + *config.focus_area + "\n";
    return summary;
}

// 检查 UltraReview 功能是否可用
auto is_ultrareview_enabled() -> bool {
    const char* enabled = std::getenv("CC_REPL_ULTRAREVIEW_ENABLED");
    return enabled != nullptr && std::string_view{enabled} != "0" && std::string_view{enabled} != "false";
}

// 获取 UltraReview 剩余配额
auto get_ultrareview_quota() -> UltraReviewQuota {
    auto read_int = [](const char* name, int fallback) {
        if (const char* value = std::getenv(name)) {
            try { return std::stoi(value); } catch (...) { return fallback; }
        }
        return fallback;
    };
    return {.remaining = read_int("CC_REPL_ULTRAREVIEW_REMAINING", 0), .total = read_int("CC_REPL_ULTRAREVIEW_TOTAL", 0)};
}

} // namespace cc::commands
