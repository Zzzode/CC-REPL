module;

#include <string>

export module ui.components.figures;

export namespace ui::components::figures {

// The former is better vertically aligned, but isn't usually supported on Windows/Linux
inline constexpr std::string_view BLACK_CIRCLE = "⏺";
inline constexpr std::string_view BULLET_OPERATOR = "∙";
inline constexpr std::string_view TEARDROP_ASTERISK = "✻";
inline constexpr std::string_view UP_ARROW = "↑";
inline constexpr std::string_view DOWN_ARROW = "↓";
inline constexpr std::string_view LIGHTNING_BOLT = "↯";
inline constexpr std::string_view EFFORT_LOW = "○";
inline constexpr std::string_view EFFORT_MEDIUM = "◐";
inline constexpr std::string_view EFFORT_HIGH = "●";
inline constexpr std::string_view EFFORT_MAX = "◉";

// Media/trigger status indicators
inline constexpr std::string_view PLAY_ICON = "▶";
inline constexpr std::string_view PAUSE_ICON = "⏸";

// MCP subscription indicators
inline constexpr std::string_view REFRESH_ARROW = "↻";
inline constexpr std::string_view CHANNEL_ARROW = "←";
inline constexpr std::string_view INJECTED_ARROW = "→";
inline constexpr std::string_view FORK_GLYPH = "⑂";

// Review status indicators (ultrareview diamond states)
inline constexpr std::string_view DIAMOND_OPEN = "◇";
inline constexpr std::string_view DIAMOND_FILLED = "◆";
inline constexpr std::string_view REFERENCE_MARK = "※";

// Issue flag indicator
inline constexpr std::string_view FLAG_ICON = "⚑";

// Blockquote indicator
inline constexpr std::string_view BLOCKQUOTE_BAR = "▎";
inline constexpr std::string_view HEAVY_HORIZONTAL = "━";

// Bridge status indicators
inline constexpr std::string_view BRIDGE_READY_INDICATOR = "·✓·";
inline constexpr std::string_view BRIDGE_FAILED_INDICATOR = "×";

} // namespace ui::components::figures
