module;
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.marketplace_notification;

export namespace cc::hooks {

// 市场插件更新信息
struct MarketplaceUpdate {
    std::string plugin_id;
    std::string new_version;
};

// 检查市场中可用的插件更新
inline std::vector<MarketplaceUpdate> check_marketplace_updates() {
    // 查询远程市场 API 获取已安装插件的更新信息
    return {};
}

// 显示插件更新通知
inline void show_update_notification(std::string_view plugin_id, std::string_view version) {
    std::fprintf(stderr, "[Plugin] Update available: %.*s -> v%.*s\n",
                 static_cast<int>(plugin_id.size()), plugin_id.data(),
                 static_cast<int>(version.size()), version.data());
}

// 检查是否启用了自动更新
inline bool is_auto_update_enabled() {
    const char* auto_update = std::getenv("CC_AUTO_UPDATE_PLUGINS");
    return auto_update != nullptr && std::string_view(auto_update) == "1";
}

} // namespace cc::hooks
