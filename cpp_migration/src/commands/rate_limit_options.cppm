module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>

export module cc.commands.rate_limit_options;

export namespace cc::commands {

// 当前速率限制信息
struct RateLimits {
    int rpm;   // 每分钟请求数
    int tpm;   // 每分钟 token 数
    int daily; // 每日请求数
};

auto get_current_limits() -> RateLimits;

// 显示当前速率限制状态
auto show_rate_limit_status() -> std::string {
    auto limits = get_current_limits();

    std::string status = "Rate Limit Status:\n";
    status += "  Requests/min:  " + std::to_string(limits.rpm) + "\n";
    status += "  Tokens/min:    " + std::to_string(limits.tpm) + "\n";
    status += "  Daily limit:   " + std::to_string(limits.daily) + "\n";
    return status;
}

// 获取当前限制值
auto get_current_limits() -> RateLimits {
    auto read_int = [](const char* name, int fallback) {
        if (const char* value = std::getenv(name)) {
            try { return std::stoi(value); } catch (...) { return fallback; }
        }
        return fallback;
    };
    return {
        .rpm = read_int("CC_REPL_RATE_LIMIT_RPM", 60),
        .tpm = read_int("CC_REPL_RATE_LIMIT_TPM", 100000),
        .daily = read_int("CC_REPL_RATE_LIMIT_DAILY", 1000)
    };
}

// 根据使用模式建议速率限制优化方案
auto suggest_rate_limit_optimization() -> std::string {
    auto limits = get_current_limits();

    std::string suggestion = "Rate Limit Optimization Suggestions:\n";

    // 根据当前限制提供建议
    if (limits.rpm < 100) {
        suggestion += "  - Consider upgrading your plan for higher RPM limits.\n";
    }
    if (limits.tpm < 200000) {
        suggestion += "  - Use /compact to reduce token usage per request.\n";
        suggestion += "  - Enable context compression to stay within TPM limits.\n";
    }
    suggestion += "  - Batch related questions into single prompts.\n";
    suggestion += "  - Use /brief mode for shorter responses.\n";

    return suggestion;
}

} // namespace cc::commands
