module;
#include <cstdio>
#include <string>
#include <string_view>

export module cc.hooks.copy_on_select;

import cc.utils.bash_execution;

export namespace cc::hooks {

inline bool copy_to_clipboard(std::string_view text);

namespace detail {

    inline bool& copy_on_select_flag() {
        static bool enabled = false;
        return enabled;
    }
} // namespace detail


inline bool is_copy_on_select_enabled() {
    return detail::copy_on_select_flag();
}


inline void set_copy_on_select(bool enabled) {
    detail::copy_on_select_flag() = enabled;
}


inline void handle_selection_change(std::string_view selected_text) {
    if (is_copy_on_select_enabled() && !selected_text.empty()) {
        copy_to_clipboard(selected_text);
    }
}


inline bool copy_to_clipboard(std::string_view text) {
    if (text.empty()) return false;
    #if defined(__APPLE__)
    auto clip = cc::utils::bash::exec_write("pbcopy", text);
    #elif defined(_WIN32)
    auto clip = cc::utils::bash::exec_write("clip", text);
    #else
    auto clip = cc::utils::bash::exec_write("xclip -selection clipboard", text);
    if (!clip) clip = cc::utils::bash::exec_write("xsel --clipboard --input", text);
    #endif
    return clip && *clip == 0;
}

} // namespace cc::hooks
