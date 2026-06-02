module;
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

export module cc.hooks.ide_connection_status;

export namespace cc::hooks {

// IDE 连接状态详细信息
struct IdeConnectionState {
    bool connected;
    std::string ide_name;
    std::optional<std::string> workspace;
    std::chrono::milliseconds latency;
};

namespace detail {
    // 连接状态变更监听器
    inline std::vector<std::function<void(IdeConnectionState)>>& connection_listeners() {
        static std::vector<std::function<void(IdeConnectionState)>> listeners;
        return listeners;
    }

    inline int& next_listener_id() {
        static int id = 0;
        return id;
    }

    // 当前连接状态
    inline IdeConnectionState& current_state() {
        static IdeConnectionState state{
            .connected = false,
            .ide_name = "",
            .workspace = std::nullopt,
            .latency = std::chrono::milliseconds{0}
        };
        return state;
    }
} // namespace detail

// 获取当前 IDE 连接状态
inline IdeConnectionState get_ide_connection_state() {
    return detail::current_state();
}

// 注册连接状态变更监听，返回监听器 ID
inline int on_ide_connection_change(std::function<void(IdeConnectionState)> callback) {
    detail::connection_listeners().push_back(std::move(callback));
    return ++detail::next_listener_id();
}

// 断开 IDE 连接
inline void disconnect_ide() {
    auto& state = detail::current_state();
    state.connected = false;
    state.latency = std::chrono::milliseconds{0};

    // 通知所有监听器
    for (auto& listener : detail::connection_listeners()) {
        listener(state);
    }
}

} // namespace cc::hooks
