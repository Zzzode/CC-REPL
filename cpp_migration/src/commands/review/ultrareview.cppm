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


struct UltraReviewQuota {
    int remaining;
    int total;
};


struct ReviewConfig {
    std::string target;
    bool auto_fix;
    std::optional<std::string> focus_area;
    int max_issues;
};


auto run_ultrareview(ReviewConfig config) -> std::expected<std::string, std::string> {
    if (config.target.empty()) {
        return std::unexpected("Review target cannot be empty");
    }
    if (config.max_issues <= 0) {
        config.max_issues = 50;
    }
    std::string summary = "UltraReview prepared for " + config.target + "\n";
    summary += "Max issues: " + std::to_string(config.max_issues) + "\n";
    summary += "Auto-fix: " + std::string(config.auto_fix ? "enabled" : "disabled") + "\n";
    if (config.focus_area) summary += "Focus: " + *config.focus_area + "\n";
    return summary;
}


auto is_ultrareview_enabled() -> bool {
    const char* enabled = std::getenv("CC_REPL_ULTRAREVIEW_ENABLED");
    return enabled != nullptr && std::string_view{enabled} != "0" && std::string_view{enabled} != "false";
}


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
