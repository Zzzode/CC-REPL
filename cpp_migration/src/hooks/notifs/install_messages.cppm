module;
#include <string>
#include <string_view>
#include <vector>
#include <map>

export module cc.hooks.notifs.install_messages;

export namespace cc::hooks::notifs {

namespace detail {
    // 安装消息存储：id → message
    inline std::map<std::string, std::string>& message_store() {
        static std::map<std::string, std::string> store;
        return store;
    }

    // 已展示的消息 ID 集合
    inline std::vector<std::string>& shown_ids() {
        static std::vector<std::string> ids;
        return ids;
    }
} // namespace detail

// 获取尚未展示的安装消息列表
inline std::vector<std::string> get_pending_install_messages() {
    std::vector<std::string> pending;
    for (const auto& [id, msg] : detail::message_store()) {
        // 检查是否已展示过
        bool shown = false;
        for (const auto& sid : detail::shown_ids()) {
            if (sid == id) { shown = true; break; }
        }
        if (!shown) {
            pending.push_back(msg);
        }
    }
    return pending;
}

// 标记某条安装消息已展示
inline void mark_message_shown(std::string_view id) {
    detail::shown_ids().emplace_back(id);
}

// 添加一条安装消息（如首次安装提示、迁移提示等）
inline void add_install_message(std::string_view id, std::string_view message) {
    detail::message_store().emplace(std::string(id), std::string(message));
}

} // namespace cc::hooks::notifs
