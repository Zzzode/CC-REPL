module;

#include <cstdlib>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>

export module cc.utils.terminal;


export namespace cc::utils {

// 颜色支持级别
enum class ColorSupport { none, basic16, palette256, truecolor };

// 终端信息
struct TerminalInfo {
    std::string name;
    ColorSupport color_support{ColorSupport::truecolor};
    bool supports_hyperlinks{false};
    bool supports_images{false};
    bool is_tmux{false};
    bool is_ssh{false};
};

// RAII Raw Mode 守卫
class RawModeGuard {
    bool active_{false};
    termios original_{};
public:
    RawModeGuard() { enter(); }
    ~RawModeGuard() { if (active_) leave(); }
    RawModeGuard(const RawModeGuard&) = delete;
    RawModeGuard& operator=(const RawModeGuard&) = delete;
    RawModeGuard(RawModeGuard&& o) noexcept : active_(o.active_) { o.active_ = false; }
    
private:
    void enter() {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) return;
        auto raw = original_;
        raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    void leave() {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        active_ = false;
    }
};

// 进入 raw mode (返回 RAII 守卫)
[[nodiscard]] inline auto enter_raw_mode() -> RawModeGuard {
    return RawModeGuard{};
}

// 检测颜色支持
[[nodiscard]] inline auto detect_color_support() -> ColorSupport {
    if (auto* ct = std::getenv("COLORTERM")) {
        std::string_view s(ct);
        if (s == "truecolor" || s == "24bit") return ColorSupport::truecolor;
    }
    if (auto* term = std::getenv("TERM")) {
        std::string_view s(term);
        if (s.find("256color") != std::string_view::npos) return ColorSupport::palette256;
        if (s.find("color") != std::string_view::npos) return ColorSupport::basic16;
    }
    return ColorSupport::basic16;
}

// 检测终端
[[nodiscard]] inline auto detect_terminal() -> TerminalInfo {
    TerminalInfo info;
    if (auto* term_program = std::getenv("TERM_PROGRAM"))
        info.name = term_program;
    info.color_support = detect_color_support();
    info.is_tmux = std::getenv("TMUX") != nullptr;
    info.is_ssh = std::getenv("SSH_CONNECTION") != nullptr;
    
    // Kitty/iTerm2/WezTerm 支持 hyperlinks
    if (info.name == "iTerm.app" || info.name == "WezTerm") {
        info.supports_hyperlinks = true;
        info.supports_images = true;
    }
    if (info.name == "vscode") info.supports_hyperlinks = true;
    
    return info;
}

// 设置终端标题
inline void set_title(std::string_view text) {
    // OSC 2 序列
    std::printf("\033]2;%.*s\007", static_cast<int>(text.size()), text.data());
}

// 光标控制
inline void show_cursor(bool visible) {
    std::printf(visible ? "\033[?25h" : "\033[?25l");
}

inline void move_cursor(int row, int col) {
    std::printf("\033[%d;%dH", row, col);
}

inline void clear_screen() { std::printf("\033[2J\033[H"); }
inline void clear_line() { std::printf("\033[2K\r"); }

// 生成 OSC 8 超链接
[[nodiscard]] inline auto make_hyperlink(std::string_view url, std::string_view text) -> std::string {
    std::string result;
    result.reserve(url.size() + text.size() + 20);
    result += "\033]8;;";
    result += url;
    result += "\033\\";
    result += text;
    result += "\033]8;;\033\\";
    return result;
}

} // namespace cc::utils
