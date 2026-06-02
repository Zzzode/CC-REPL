module;
#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <mutex>

export module cc.utils.debug_filter;

export namespace cc::utils {

// 调试命名空间过滤器，支持通配符匹配和取反模式
class DebugFilter {
public:
    DebugFilter() = default;

    // Move constructor (mutex is not movable, create fresh one)
    DebugFilter(DebugFilter&& other) noexcept
        : enable_patterns_(std::move(other.enable_patterns_))
        , disable_patterns_(std::move(other.disable_patterns_)) {}
    DebugFilter& operator=(DebugFilter&& other) noexcept {
        if (this != &other) {
            enable_patterns_ = std::move(other.enable_patterns_);
            disable_patterns_ = std::move(other.disable_patterns_);
        }
        return *this;
    }
    DebugFilter(const DebugFilter&) = delete;
    DebugFilter& operator=(const DebugFilter&) = delete;

    // 从 CC_DEBUG 环境变量初始化
    [[nodiscard]] static DebugFilter from_env() {
        DebugFilter filter;
        const char* env = std::getenv("CC_DEBUG");
        if (env && *env) {
            filter.set_patterns(env);
        }
        return filter;
    }

    // 设置过滤模式（逗号分隔），例如 "api:*,mcp:*,-verbose:*"
    void set_patterns(std::string_view patterns) {
        std::lock_guard lock(mutex_);
        enable_patterns_.clear();
        disable_patterns_.clear();

        size_t start = 0;
        while (start < patterns.size()) {
            auto end = patterns.find(',', start);
            if (end == std::string_view::npos) end = patterns.size();

            auto pattern = trim(patterns.substr(start, end - start));
            if (!pattern.empty()) {
                if (pattern[0] == '-') {
                    // 取反模式
                    disable_patterns_.emplace_back(pattern.substr(1));
                } else {
                    enable_patterns_.emplace_back(pattern);
                }
            }
            start = end + 1;
        }
    }

    // 检查命名空间是否匹配（先检查禁用，再检查启用）
    [[nodiscard]] bool matches(std::string_view ns) const {
        std::lock_guard lock(mutex_);

        // 无模式时全部禁用
        if (enable_patterns_.empty() && disable_patterns_.empty()) return false;

        // 检查是否被明确禁用
        for (const auto& pat : disable_patterns_) {
            if (glob_match(ns, pat)) return false;
        }

        // 检查是否被启用
        for (const auto& pat : enable_patterns_) {
            if (glob_match(ns, pat)) return true;
        }

        // 有启用模式但不匹配 → 禁用
        return enable_patterns_.empty();
    }

private:
    std::vector<std::string> enable_patterns_;
    std::vector<std::string> disable_patterns_;
    mutable std::mutex mutex_;

    // 简单通配符匹配（仅支持 * 作为任意字符序列）
    [[nodiscard]] static bool glob_match(std::string_view str, std::string_view pattern) {
        size_t si = 0, pi = 0;
        size_t star_pi = std::string_view::npos;
        size_t star_si = 0;

        while (si < str.size()) {
            if (pi < pattern.size() && (pattern[pi] == str[si] || pattern[pi] == '?')) {
                ++si; ++pi;
            } else if (pi < pattern.size() && pattern[pi] == '*') {
                star_pi = pi;
                star_si = si;
                ++pi;
            } else if (star_pi != std::string_view::npos) {
                pi = star_pi + 1;
                si = ++star_si;
            } else {
                return false;
            }
        }

        // 跳过模式末尾的 *
        while (pi < pattern.size() && pattern[pi] == '*') ++pi;
        return pi == pattern.size();
    }

    // 去除首尾空白
    [[nodiscard]] static std::string_view trim(std::string_view sv) {
        auto start = sv.find_first_not_of(" \t");
        if (start == std::string_view::npos) return {};
        auto end = sv.find_last_not_of(" \t");
        return sv.substr(start, end - start + 1);
    }
};

} // namespace cc::utils
