module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.system_theme;

export namespace cc::utils {

// ─── System Theme Type ───────────────────────────────────────────────────────

/// Terminal dark/light theme classification
enum class SystemTheme {
    dark,
    light
};

/// Theme setting (may be 'auto' or explicit)
enum class ThemeSetting {
    dark,
    light,
    automatic  // Resolve from terminal detection
};

/// Concrete theme name (always resolved, never 'auto')
using ThemeName = SystemTheme;

// ─── Theme Detection ─────────────────────────────────────────────────────────

/// Get the current terminal theme. Cached after first detection;
/// the watcher updates the cache on live changes.
SystemTheme get_system_theme_name();

/// Update the cached terminal theme.
/// Called by the watcher when the OSC 11 query returns.
void set_cached_system_theme(SystemTheme theme);

/// Resolve a ThemeSetting (which may be 'automatic') to a concrete ThemeName.
ThemeName resolve_theme_setting(ThemeSetting setting);

// ─── OSC Color Parsing ───────────────────────────────────────────────────────

/**
 * Parse an OSC color response data string into a theme.
 *
 * Accepts XParseColor formats returned by OSC 10/11 queries:
 * - `rgb:R/G/B` where each component is 1-4 hex digits
 * - `#RRGGBB` / `#RRRRGGGGBBBB`
 *
 * Returns nullopt for unrecognized formats.
 */
std::optional<SystemTheme> theme_from_osc_color(std::string_view data);

// ─── COLORFGBG Detection ─────────────────────────────────────────────────────

/**
 * Read $COLORFGBG for a synchronous initial guess.
 * Format is `fg;bg` (or `fg;other;bg`) where values are ANSI color indices.
 * Only set by some terminals (rxvt-family, Konsole, iTerm2).
 */
std::optional<SystemTheme> detect_from_color_fg_bg();

// ─── Theme Change Notification ───────────────────────────────────────────────

/// Callback type for theme change notifications
using ThemeChangeCallback = std::function<void(SystemTheme)>;

/// Register a callback for theme change notifications.
/// Returns an unsubscribe function.
std::function<void()> on_theme_change(ThemeChangeCallback callback);

/// Notify all registered callbacks of a theme change.
/// Called by the OSC 11 watcher when it detects a change.
void notify_theme_change(SystemTheme new_theme);

} // namespace cc::utils
