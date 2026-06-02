module;
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.hooks.plugin_recommendation;

export namespace cc::hooks {

// 插件推荐信息
struct PluginRecommendation {
    std::string plugin_id;
    std::string reason;
    float confidence;   // 推荐置信度 [0.0, 1.0]
};

namespace detail {
    // 已被用户关闭的推荐
    inline std::unordered_set<std::string>& dismissed_recommendations() {
        static std::unordered_set<std::string> dismissed;
        return dismissed;
    }
} // namespace detail

// 根据上下文获取插件推荐列表
inline std::vector<PluginRecommendation> get_recommendations(std::string_view context) {
    // 分析上下文，匹配适用的插件
    (void)context;
    return {};
}

// 关闭某个插件推荐（不再显示）
inline void dismiss_recommendation(std::string_view plugin_id) {
    detail::dismissed_recommendations().insert(std::string(plugin_id));
}

// 判断是否应该展示推荐（用户可全局关闭推荐功能）
inline bool should_show_recommendations() {
    // 检查用户配置中是否启用了推荐
    return true;
}

} // namespace cc::hooks
