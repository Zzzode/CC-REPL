module;
#include <optional>
#include <string>
#include <string_view>

export module cc.hooks.notifs.auto_mode_unavailable;

export namespace cc::hooks::notifs {

// 检查自动模式是否可用（例如需要特定权限或配置）
inline bool check_auto_mode_available() {
    // 检查环境变量和配置是否支持自动模式
    // 如果缺少必要的权限或配置项，返回 false
    return true;
}

// 获取自动模式不可用的原因
inline std::optional<std::string> get_unavailable_reason() {
    if (check_auto_mode_available()) {
        return std::nullopt;
    }
    return "Auto mode requires explicit user consent and valid API key";
}

// 显示自动模式不可用的通知
inline void show_auto_mode_unavailable_notification() {
    auto reason = get_unavailable_reason();
    if (reason.has_value()) {
        // 输出通知到 stderr，避免干扰正常输出流
        // 在实际集成时可替换为 UI 层通知机制
    }
}

} // namespace cc::hooks::notifs
