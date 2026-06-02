module;
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.tips.tip_registry;

export namespace cc::services::tips {

// A tip that can be shown to the user
struct Tip {
    std::string id;
    std::string message;
    std::string category;
    int priority{0};
    std::function<bool()> condition; // When to show this tip
};

namespace detail {
    inline std::mutex registry_mutex;
    inline std::vector<Tip> registered_tips;
} // namespace detail

// Register a new tip
auto register_tip(Tip tip) -> void {
    std::lock_guard lock(detail::registry_mutex);
    detail::registered_tips.push_back(std::move(tip));
}

// Get all registered tips
auto get_all_tips() -> std::vector<Tip> {
    std::lock_guard lock(detail::registry_mutex);
    return detail::registered_tips;
}

// Get tips relevant to a specific context/category
auto get_tips_for_context(std::string_view context) -> std::vector<Tip> {
    std::lock_guard lock(detail::registry_mutex);
    std::vector<Tip> result;
    for (const auto& tip : detail::registered_tips) {
        if (tip.category == context) {
            // Check condition if set
            if (!tip.condition || tip.condition()) {
                result.push_back(tip);
            }
        }
    }
    return result;
}

} // namespace cc::services::tips
