module;
#include <cstdio>
#include <string>
#include <string_view>

export module cc.hooks.copy_on_select;

export namespace cc::hooks {

inline bool copy_to_clipboard(std::string_view text);

namespace detail {
    // 选中即复制功能开关
    inline bool& copy_on_select_flag() {
        static bool enabled = false;
        return enabled;
    }
} // namespace detail

// 检查选中即复制功能是否启用
inline bool is_copy_on_select_enabled() {
    return detail::copy_on_select_flag();
}

// 设置选中即复制功能开关
inline void set_copy_on_select(bool enabled) {
    detail::copy_on_select_flag() = enabled;
}

// 处理文本选择变更事件
inline void handle_selection_change(std::string_view selected_text) {
    if (is_copy_on_select_enabled() && !selected_text.empty()) {
        copy_to_clipboard(selected_text);
    }
}

// 将文本复制到系统剪贴板
inline bool copy_to_clipboard(std::string_view text) {
    if (text.empty()) return false;
    #if defined(__APPLE__)
    FILE* pipe = popen("pbcopy", "w");
    #elif defined(_WIN32)
    FILE* pipe = popen("clip", "w");
    #else
    // Try xclip first, fall back to xsel
    FILE* pipe = popen("xclip -selection clipboard", "w");
    if (!pipe) pipe = popen("xsel --clipboard --input", "w");
    #endif
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    int status = pclose(pipe);
    return status == 0;
}

} // namespace cc::hooks
