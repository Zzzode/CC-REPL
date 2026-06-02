module;

#include <atomic>
#include <cstddef>
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

/// Detects if running inside tmux -CC control mode (iTerm2 integration)
bool is_tmux_control_mode();

/// Whether fullscreen/alt-screen mode is supported in the current terminal
bool supports_fullscreen();

/// Whether fullscreen mode is currently enabled
bool is_fullscreen_enabled();

/// Enter fullscreen/alternate screen mode
void enter_fullscreen();

/// Exit fullscreen/alternate screen mode
void exit_fullscreen();

/// Toggle fullscreen mode
void toggle_fullscreen();

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
