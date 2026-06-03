module;
#include <optional>
#include <string>

export module cc.hooks.clipboard_image_hint;

export namespace cc::hooks {

namespace detail {

    inline bool& clipboard_hint_dismissed() {
        static bool dismissed = false;
        return dismissed;
    }
} // namespace detail


inline bool check_clipboard_has_image() {


    return false;
}


inline std::optional<std::string> get_clipboard_image_hint() {
    if (detail::clipboard_hint_dismissed()) {
        return std::nullopt;
    }
    if (check_clipboard_has_image()) {
        return "Clipboard contains an image. Use /paste-image to include it in the conversation.";
    }
    return std::nullopt;
}


inline void dismiss_clipboard_hint() {
    detail::clipboard_hint_dismissed() = true;
}

} // namespace cc::hooks
