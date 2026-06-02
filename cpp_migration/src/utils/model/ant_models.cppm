module;
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.ant_models;

export namespace cc::utils {

struct ModelInfo {
    std::string id;
    std::string display_name;
    int context_window;
    int max_output;
    bool supports_thinking;
    bool supports_vision;
    std::chrono::system_clock::time_point release_date;
};

// Helper to build a time_point from year/month/day
namespace detail {
    inline std::chrono::system_clock::time_point make_date(int year, int month, int day) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        auto t = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(t);
    }
} // namespace detail

std::vector<ModelInfo> get_all_models() {
    return {
        ModelInfo{
            "claude-sonnet-4-20250514",
            "Claude Sonnet 4",
            200000,
            16384,
            true,
            true,
            detail::make_date(2025, 5, 14)
        },
        ModelInfo{
            "claude-opus-4-20250514",
            "Claude Opus 4",
            200000,
            32768,
            true,
            true,
            detail::make_date(2025, 5, 14)
        },
        ModelInfo{
            "claude-haiku-4-20250514",
            "Claude Haiku 4",
            200000,
            8192,
            true,
            true,
            detail::make_date(2025, 5, 14)
        },
    };
}

std::optional<ModelInfo> get_model_info(std::string_view id) {
    for (auto& m : get_all_models()) {
        if (m.id == id) return m;
    }
    return std::nullopt;
}

std::string get_default_model() {
    return "claude-sonnet-4-20250514";
}

std::string get_latest_model() {
    return "claude-opus-4-20250514";
}

} // namespace cc::utils
