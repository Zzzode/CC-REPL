module;

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.terminal_helpers;

export namespace cc::utils {

// ─── Terminal Panel ──────────────────────────────────────────────────────────

/// Get the tmux socket name for the terminal panel.
/// Uses a unique socket per instance (based on session ID).
std::string get_terminal_panel_socket();

/// Terminal panel class managing a persistent tmux-based shell panel.
/// Toggled with a keyboard shortcut; uses tmux for shell persistence.
class TerminalPanel {
public:
    TerminalPanel();
    ~TerminalPanel();

    // Non-copyable
    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    /// Toggle the terminal panel (show/hide)
    void toggle();

    /// Check if the panel is currently visible
    bool is_visible() const { return visible_; }

private:
    bool check_tmux();
    bool has_session();
    void create_session();
    void attach_session();
    void show_shell();

    std::optional<bool> has_tmux_;
    bool cleanup_registered_ = false;
    bool visible_ = false;
};

/// Return the singleton TerminalPanel, creating it lazily on first use
TerminalPanel& get_terminal_panel();

// ─── Tmux Socket Isolation ───────────────────────────────────────────────────

/// Constants
inline constexpr std::string_view CLAUDE_SOCKET_PREFIX = "claude";

/// Gets the socket name for Claude's isolated tmux session (format: claude-<PID>)
std::string get_claude_socket_name();

/// Gets the socket path if the socket has been initialized. Returns empty if not.
std::optional<std::string> get_claude_socket_path();

/// Sets socket info after initialization
void set_claude_socket_info(std::string_view path, int pid);

/// Returns whether the socket has been initialized
bool is_socket_initialized();

/// Gets the TMUX environment variable value for Claude's isolated socket.
/// Format: "socket_path,server_pid,pane_index"
/// Returns nullopt if socket is not yet initialized.
std::optional<std::string> get_claude_tmux_env();

/// Checks if tmux is available on this system (cached after first check)
std::expected<bool, std::string> check_tmux_available();

/// Returns the cached tmux availability status (false if not yet checked)
bool is_tmux_available();

/// Marks that the Tmux tool has been used at least once
void mark_tmux_tool_used();

/// Returns whether the Tmux tool has been used at least once
bool has_tmux_tool_been_used();

/// Ensures the socket is initialized with a tmux session.
/// Safe to call multiple times; will only initialize once.
std::expected<void, std::string> ensure_socket_initialized();

/// Reset socket state (for testing purposes)
void reset_socket_state();

// ─── Fullscreen Mode ─────────────────────────────────────────────────────────

namespace detail {

/// Helper: check if an env var is set and truthy (1, true, yes, on).
inline bool is_env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string_view s(v);
    return s == "1" || s == "true" || s == "yes" || s == "on"
        || s == "TRUE" || s == "YES" || s == "ON";
}

/// Helper: check if an env var is explicitly defined and falsy (0, false, no, off).
inline bool is_env_defined_falsy(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;  // not defined at all
    if (!*v) return true;   // empty string = falsy
    std::string_view s(v);
    return s == "0" || s == "false" || s == "no" || s == "off"
        || s == "FALSE" || s == "NO" || s == "OFF";
}

} // namespace detail

/// Detects if running inside tmux -CC control mode (iTerm2 integration).
/// Uses env-heuristic only (no subprocess probe): checks $TMUX is set AND
/// $TERM_PROGRAM is unset (iTerm2's tell-tale sign of -CC mode).
inline bool is_tmux_control_mode() {
    const char* tmux = std::getenv("TMUX");
    if (!tmux || !*tmux) return false;
    // When tmux -CC is active, TERM_PROGRAM is typically unset because
    // tmux takes over terminal emulation for iTerm2's control mode.
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (!term_program || !*term_program) return true;
    return false;
}

/// Whether fullscreen/alt-screen mode is supported in the current terminal.
/// Faithful to TS: supported unless tmux -CC control mode is detected.
inline bool supports_fullscreen() {
    return !is_tmux_control_mode();
}

/// Whether fullscreen mode is currently enabled.
///
/// Faithful port of TS `isFullscreenEnvEnabled()` from utils/fullscreen.ts:
///   - Explicit opt-out (CLAUDE_CODE_NO_FLICKER=0) → false
///   - Explicit opt-in  (CLAUDE_CODE_NO_FLICKER=1) → true
///   - Auto-disable under tmux -CC control mode    → false
///   - Default: USER_TYPE == "ant" ? true : false
inline bool is_fullscreen_enabled() {
    // Explicit user opt-out always wins.
    if (detail::is_env_defined_falsy("CLAUDE_CODE_NO_FLICKER")) return false;
    // Explicit opt-in overrides auto-detection.
    if (detail::is_env_truthy("CLAUDE_CODE_NO_FLICKER")) return true;
    // Auto-disable under tmux -CC: alt-screen + mouse tracking corrupts
    // terminal state on double-click and mouse wheel is dead.
    if (is_tmux_control_mode()) return false;
    // Default: on for ant builds, off for external users.
    const char* user_type = std::getenv("USER_TYPE");
    return user_type && std::string_view(user_type) == "ant";
}

/// Enter fullscreen/alternate screen mode (smcup / DEC 1049).
inline void enter_fullscreen() {
    std::fprintf(stdout, "\033[?1049h");
    std::fflush(stdout);
}

/// Exit fullscreen/alternate screen mode (rmcup / DEC 1049).
inline void exit_fullscreen() {
    std::fprintf(stdout, "\033[?1049l");
    std::fflush(stdout);
}

/// Toggle fullscreen mode.
///
/// Tracks state locally with a static flag (mirrors TS `toggleFullscreen()`).
/// Caller is responsible for ensuring enter/exit are paired correctly.
inline void toggle_fullscreen() {
    static bool s_fullscreen_active = false;
    if (s_fullscreen_active) {
        exit_fullscreen();
        s_fullscreen_active = false;
    } else {
        enter_fullscreen();
        s_fullscreen_active = true;
    }
}

// ─── Horizontal Scroll ───────────────────────────────────────────────────────

/// Visible window bounds for horizontally scrollable item lists
struct HorizontalScrollWindow {
    size_t start_index = 0;
    size_t end_index = 0;
    bool show_left_arrow = false;
    bool show_right_arrow = false;
};

/**
 * Calculate the visible window of items that fit within available width,
 * ensuring the selected item is always visible. Uses edge-based scrolling:
 * the window only scrolls when the selected item would be outside the visible
 * range, and positions the selected item at the edge (not centered).
 *
 * @param item_widths      Array of item widths (including separator if applicable)
 * @param available_width  Total available width for items
 * @param arrow_width      Width of scroll indicator arrow (including space)
 * @param selected_idx     Index of selected item (must stay visible)
 * @param first_item_has_separator Whether first item's width includes a leading separator
 */
HorizontalScrollWindow calculate_horizontal_scroll_window(
    const std::vector<int>& item_widths,
    int available_width,
    int arrow_width,
    size_t selected_idx,
    bool first_item_has_separator);

/// Overload with first_item_has_separator defaulting to true
HorizontalScrollWindow calculate_horizontal_scroll_window(
    const std::vector<int>& item_widths,
    int available_width,
    int arrow_width,
    size_t selected_idx);

} // namespace cc::utils
