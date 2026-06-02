module;
#include <optional>
#include <string>

export module cc.hooks.clipboard_image_hint;

export namespace cc::hooks {

namespace detail {
    // 剪贴板提示是否已被关闭
    inline bool& clipboard_hint_dismissed() {
        static bool dismissed = false;
        return dismissed;
    }
} // namespace detail

// 检查剪贴板中是否包含图片数据
inline bool check_clipboard_has_image() {
    // 平台相关实现：检查系统剪贴板内容类型
    // macOS 使用 NSPasteboard，Linux 使用 xclip
    return false;
}

// 获取剪贴板图片提示信息（如果有图片且未被关闭）
inline std::optional<std::string> get_clipboard_image_hint() {
    if (detail::clipboard_hint_dismissed()) {
        return std::nullopt;
    }
    if (check_clipboard_has_image()) {
        return "Clipboard contains an image. Use /paste-image to include it in the conversation.";
    }
    return std::nullopt;
}

// 关闭剪贴板图片提示
inline void dismiss_clipboard_hint() {
    detail::clipboard_hint_dismissed() = true;
}

} // namespace cc::hooks
