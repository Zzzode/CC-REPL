module;

#include <cstdio>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.terminal_integration;

export namespace cc::utils::terminal_integration {

struct PanelConfig {
    std::uint32_t width;
    std::uint32_t height;
    std::optional<std::string> title;
};

struct TmuxSocketInfo {
    std::string socket_path;
    std::string session_name;
    bool is_connected{false};
};

namespace detail {
    inline bool& fullscreen_state() {
        static bool s_fullscreen = false;
        return s_fullscreen;
    }
    inline int& scroll_offset() {
        static int s_offset = 0;
        return s_offset;
    }
}

inline std::expected<void, std::string> enter_fullscreen() {
    // Use alternate screen buffer (smcup)
    std::fprintf(stdout, "\033[?1049h");
    std::fflush(stdout);
    detail::fullscreen_state() = true;
    return {};
}

inline std::expected<void, std::string> exit_fullscreen() {
    // Restore main screen buffer (rmcup)
    std::fprintf(stdout, "\033[?1049l");
    std::fflush(stdout);
    detail::fullscreen_state() = false;
    return {};
}

inline std::expected<TmuxSocketInfo, std::string> connect_tmux_socket(std::string_view socket_path) {
    if (socket_path.empty()) {
        return std::unexpected("Socket path is empty");
    }
    return TmuxSocketInfo{std::string(socket_path), "main", false};
}

inline std::expected<void, std::string> create_terminal_panel(const PanelConfig& config) {
    if (config.width == 0 || config.height == 0) {
        return std::unexpected("Panel dimensions must be non-zero");
    }
    // Set scroll region to panel dimensions
    std::fprintf(stdout, "\033[1;%ur", config.height);
    std::fflush(stdout);
    return {};
}

inline int get_horizontal_scroll_offset() {
    return detail::scroll_offset();
}

inline void set_horizontal_scroll_offset(int offset) {
    detail::scroll_offset() = offset;
}

inline bool is_fullscreen_mode() {
    return detail::fullscreen_state();
}

} // namespace cc::utils::terminal_integration
