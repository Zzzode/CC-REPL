module;
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.marketplace_notification;

export namespace cc::hooks {


struct MarketplaceUpdate {
    std::string plugin_id;
    std::string new_version;
};


inline std::vector<MarketplaceUpdate> check_marketplace_updates() {

    return {};
}


inline void show_update_notification(std::string_view plugin_id, std::string_view version) {
    std::fprintf(stderr, "[Plugin] Update available: %.*s -> v%.*s\n",
                 static_cast<int>(plugin_id.size()), plugin_id.data(),
                 static_cast<int>(version.size()), version.data());
}


inline bool is_auto_update_enabled() {
    const char* auto_update = std::getenv("CC_AUTO_UPDATE_PLUGINS");
    return auto_update != nullptr && std::string_view(auto_update) == "1";
}

} // namespace cc::hooks
