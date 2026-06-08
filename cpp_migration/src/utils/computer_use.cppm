module;

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.computer_use;

export namespace cc::utils::computer_use {

inline constexpr std::string_view kComputerUseMcpServerName =
    "computer-use";
inline constexpr std::string_view kCliHostBundleId =
    "com.anthropic.claude-code.cli-no-window";

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/// Mouse button identifier.
enum class MouseButton {
    Left,
    Right,
    Middle,
};

/// Mouse action type.
enum class MouseAction {
    Click,
    Press,
    Release,
};

/// Key action type.
enum class KeyAction {
    Press,
    Release,
};

/// Scroll direction.
enum class ScrollDirection {
    Vertical,
    Horizontal,
};

/// Screenshot filtering capability.
enum class ScreenshotFiltering {
    Native,
};

/// Platform for computer use (currently macOS only).
enum class CuPlatform {
    Darwin,
};

/// Coordinate mode for computer use.
enum class CoordinateMode {
    Pixels,
    Normalized,
};

/// Result kind from lock acquisition.
enum class AcquireResultKind {
    Acquired,
    Blocked,
};

/// Result kind from lock status check.
enum class CheckResultKind {
    Free,
    HeldBySelf,
    Blocked,
};

// ---------------------------------------------------------------------------
// Geometry / Display types
// ---------------------------------------------------------------------------

struct Point {
    double x{0.0};
    double y{0.0};
};

struct Rect {
    double x{0.0};
    double y{0.0};
    double w{0.0};
    double h{0.0};
};

struct DisplayGeometry {
    std::uint32_t display_id{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    double scale_factor{1.0};
};

// ---------------------------------------------------------------------------
// App types
// ---------------------------------------------------------------------------

struct FrontmostApp {
    std::string bundle_id;
    std::string display_name;
};

struct InstalledApp {
    std::string bundle_id;
    std::string display_name;
    std::string path;
    std::optional<std::string> icon_data_url;
};

struct RunningApp {
    std::string bundle_id;
    std::string display_name;
};

struct AppUnderPointResult {
    std::string bundle_id;
    std::string display_name;
};

struct HidePreviewEntry {
    std::string bundle_id;
    std::string display_name;
};

struct WindowDisplayInfo {
    std::string bundle_id;
    std::vector<std::uint32_t> display_ids;
};

// ---------------------------------------------------------------------------
// Screenshot types
// ---------------------------------------------------------------------------

struct ScreenshotResult {
    std::string base64;
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct ResolvePrepareCaptureResult {
    ScreenshotResult screenshot;
    std::vector<std::string> hidden_bundle_ids;
    std::optional<std::string> activated;
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

struct ComputerUseCapabilities {
    ScreenshotFiltering screenshot_filtering{ScreenshotFiltering::Native};
    CuPlatform platform{CuPlatform::Darwin};
    std::string host_bundle_id;
};

// ---------------------------------------------------------------------------
// Sub-gates (feature flags for individual CU features)
// ---------------------------------------------------------------------------

struct CuSubGates {
    bool pixel_validation{false};
    bool clipboard_paste_multiline{true};
    bool mouse_animation{true};
    bool hide_before_action{true};
    bool auto_target_display{true};
    bool clipboard_guard{true};
};

// ---------------------------------------------------------------------------
// ComputerExecutor — primary abstraction for computer use operations
// ---------------------------------------------------------------------------

/// Options for the executor factory.
struct ExecutorOptions {
    std::function<bool()> get_mouse_animation_enabled;
    std::function<bool()> get_hide_before_action_enabled;
};

/// The executor interface for computer control operations.
struct ComputerExecutor {
    ComputerUseCapabilities capabilities;

    // Pre-action sequence
    std::function<std::expected<std::vector<std::string>, std::string>(
        std::vector<std::string> const& allowlist_bundle_ids,
        std::optional<std::uint32_t> display_id)> prepare_for_action;

    std::function<std::expected<std::vector<HidePreviewEntry>, std::string>(
        std::vector<std::string> const& allowlist_bundle_ids,
        std::optional<std::uint32_t> display_id)> preview_hide_set;

    // Display
    std::function<std::expected<DisplayGeometry, std::string>(
        std::optional<std::uint32_t> display_id)> get_display_size;

    std::function<std::expected<std::vector<DisplayGeometry>, std::string>()>
        list_displays;

    std::function<std::expected<std::vector<WindowDisplayInfo>, std::string>(
        std::vector<std::string> const& bundle_ids)> find_window_displays;

    // Screenshots
    std::function<std::expected<ScreenshotResult, std::string>(
        std::vector<std::string> const& allowed_bundle_ids,
        std::optional<std::uint32_t> display_id)> screenshot;

    std::function<std::expected<ScreenshotResult, std::string>(
        Rect const& region_logical,
        std::vector<std::string> const& allowed_bundle_ids,
        std::optional<std::uint32_t> display_id)> zoom;

    std::function<std::expected<ResolvePrepareCaptureResult, std::string>(
        std::vector<std::string> const& allowed_bundle_ids,
        std::optional<std::uint32_t> preferred_display_id,
        bool auto_resolve,
        std::optional<bool> do_hide)> resolve_prepare_capture;

    // Keyboard
    std::function<std::expected<void, std::string>(
        std::string_view key_sequence,
        std::uint32_t repeat)> key;

    std::function<std::expected<void, std::string>(
        std::vector<std::string> const& key_names,
        std::uint32_t duration_ms)> hold_key;

    std::function<std::expected<void, std::string>(
        std::string_view text,
        bool via_clipboard)> type;

    // Clipboard
    std::function<std::expected<std::string, std::string>()> read_clipboard;
    std::function<std::expected<void, std::string>(std::string_view text)>
        write_clipboard;

    // Mouse
    std::function<std::expected<void, std::string>(
        double x, double y)> move_mouse;

    std::function<std::expected<void, std::string>(
        double x, double y,
        MouseButton button,
        std::uint8_t count,
        std::vector<std::string> const& modifiers)> click;

    std::function<std::expected<void, std::string>()> mouse_down;
    std::function<std::expected<void, std::string>()> mouse_up;
    std::function<std::expected<Point, std::string>()> get_cursor_position;

    std::function<std::expected<void, std::string>(
        std::optional<Point> from,
        Point to)> drag;

    std::function<std::expected<void, std::string>(
        double x, double y,
        double dx, double dy)> scroll;

    // App management
    std::function<std::expected<std::optional<FrontmostApp>, std::string>()>
        get_frontmost_app;

    std::function<std::expected<std::optional<AppUnderPointResult>, std::string>(
        double x, double y)> app_under_point;

    std::function<std::expected<std::vector<InstalledApp>, std::string>()>
        list_installed_apps;

    std::function<std::expected<std::optional<std::string>, std::string>(
        std::string_view path)> get_app_icon;

    std::function<std::expected<std::vector<RunningApp>, std::string>()>
        list_running_apps;

    std::function<std::expected<void, std::string>(
        std::string_view bundle_id)> open_app;
};

/// Create the CLI executor (macOS only). Throws on unsupported platforms.
auto create_cli_executor(ExecutorOptions const& opts)
    -> std::expected<ComputerExecutor, std::string>;

/// Unhide previously-hidden apps at turn end.
auto unhide_computer_use_apps(
    std::vector<std::string> const& bundle_ids)
    -> std::expected<void, std::string>;

// ---------------------------------------------------------------------------
// Input loading
// ---------------------------------------------------------------------------

/// Eagerly load and cache the computer-use input native module.
/// Returns an error if the platform is unsupported.
auto require_computer_use_input()
    -> std::expected<void, std::string>;

// ---------------------------------------------------------------------------
// Gates (feature flags)
// ---------------------------------------------------------------------------

/// Whether computer use (Chicago) is enabled for the current user.
auto get_chicago_enabled() -> bool;

/// Sub-gates controlling individual CU features.
auto get_chicago_sub_gates() -> CuSubGates;

/// Frozen coordinate mode (pixels or normalized).
auto get_chicago_coordinate_mode() -> CoordinateMode;

// ---------------------------------------------------------------------------
// Lock management
// ---------------------------------------------------------------------------

struct AcquireResult {
    AcquireResultKind kind;
    bool fresh{false};        // meaningful when kind == Acquired
    std::string blocked_by;   // meaningful when kind == Blocked
};

struct CheckResult {
    CheckResultKind kind;
    std::string blocked_by;   // meaningful when kind == Blocked
};

/// Check lock state without acquiring.
auto check_computer_use_lock()
    -> std::expected<CheckResult, std::string>;

/// Zero-syscall check: does THIS process hold the lock?
auto is_lock_held_locally() -> bool;

/// Try to acquire the computer-use lock for the current session.
auto try_acquire_computer_use_lock()
    -> std::expected<AcquireResult, std::string>;

/// Release the computer-use lock if the current session owns it.
/// Returns true if we actually released (callers fire exit notifications).
auto release_computer_use_lock()
    -> std::expected<bool, std::string>;

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

/// Turn-end cleanup: unhide apps and release lock.
auto cleanup_computer_use_after_turn()
    -> std::expected<void, std::string>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Detect the terminal emulator's bundle ID (macOS).
auto get_terminal_bundle_id()
    -> std::optional<std::string>;

/// Check if a name matches the computer-use MCP server.
auto is_computer_use_mcp_server(std::string_view name) -> bool;

} // namespace cc::utils::computer_use
