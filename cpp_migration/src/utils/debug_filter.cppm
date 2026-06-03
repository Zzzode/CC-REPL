module;
#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <mutex>

export module cc.utils.debug_filter;

export namespace cc::utils {


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


    [[nodiscard]] static DebugFilter from_env() {
        DebugFilter filter;
        const char* env = std::getenv("CC_DEBUG");
        if (env && *env) {
            filter.set_patterns(env);
        }
        return filter;
    }


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

                    disable_patterns_.emplace_back(pattern.substr(1));
                } else {
                    enable_patterns_.emplace_back(pattern);
                }
            }
            start = end + 1;
        }
    }


    [[nodiscard]] bool matches(std::string_view ns) const {
        std::lock_guard lock(mutex_);


        if (enable_patterns_.empty() && disable_patterns_.empty()) return false;


        for (const auto& pat : disable_patterns_) {
            if (glob_match(ns, pat)) return false;
        }


        for (const auto& pat : enable_patterns_) {
            if (glob_match(ns, pat)) return true;
        }


        return enable_patterns_.empty();
    }

private:
    std::vector<std::string> enable_patterns_;
    std::vector<std::string> disable_patterns_;
    mutable std::mutex mutex_;


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


        while (pi < pattern.size() && pattern[pi] == '*') ++pi;
        return pi == pattern.size();
    }


    [[nodiscard]] static std::string_view trim(std::string_view sv) {
        auto start = sv.find_first_not_of(" \t");
        if (start == std::string_view::npos) return {};
        auto end = sv.find_last_not_of(" \t");
        return sv.substr(start, end - start + 1);
    }
};

} // namespace cc::utils
