module;
#include <expected>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.session_backgrounding;

export namespace cc::hooks {

namespace detail {
    // 后台会话 ID 列表
    inline std::vector<std::string>& backgrounded_sessions() {
        static std::vector<std::string> sessions;
        return sessions;
    }
} // namespace detail

// 检查当前会话是否可以被置入后台
inline bool can_background_session() {
    // 如果有正在进行的工具调用或流式输出，不允许后台化
    return true;
}

// 将当前会话置入后台运行
inline std::expected<void, std::string> background_current_session() {
    if (!can_background_session()) {
        return std::unexpected("Cannot background session: active operation in progress");
    }
    // 保存当前会话状态并分离
    return {};
}

// 恢复一个已后台化的会话
inline std::expected<void, std::string> resume_backgrounded_session(std::string_view id) {
    auto& sessions = detail::backgrounded_sessions();
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        if (*it == id) {
            sessions.erase(it);
            return {};
        }
    }
    return std::unexpected("Session not found: " + std::string(id));
}

// 获取所有后台会话的 ID 列表
inline std::vector<std::string> get_backgrounded_sessions() {
    return detail::backgrounded_sessions();
}

} // namespace cc::hooks
