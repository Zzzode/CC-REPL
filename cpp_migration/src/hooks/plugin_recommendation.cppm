module;
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.hooks.plugin_recommendation;

export namespace cc::hooks {


struct PluginRecommendation {
    std::string plugin_id;
    std::string reason;
    float confidence;
};

namespace detail {

    inline std::unordered_set<std::string>& dismissed_recommendations() {
        static std::unordered_set<std::string> dismissed;
        return dismissed;
    }
} // namespace detail


inline std::vector<PluginRecommendation> get_recommendations(std::string_view context) {

    (void)context;
    return {};
}


inline void dismiss_recommendation(std::string_view plugin_id) {
    detail::dismissed_recommendations().insert(std::string(plugin_id));
}


inline bool should_show_recommendations() {

    return true;
}

} // namespace cc::hooks
