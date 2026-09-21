/// @file figures.cppm
/// @brief Unicode figure constants for terminal UI rendering.
/// Migrated from src/constants/figures.ts
module;

#include <string_view>
#include <array>

export module cc.constants.figures;

export namespace cc::constants::figures {

// Platform-dependent circle (darwin uses U+23FA, others use U+25CF)
#ifdef __APPLE__
inline constexpr std::string_view BLACK_CIRCLE = "\u23FA";  // ⏺
#else
inline constexpr std::string_view BLACK_CIRCLE = "\u25CF";  // ●
#endif

inline constexpr std::string_view BULLET_OPERATOR = "\u2219";    // ∙
inline constexpr std::string_view TEARDROP_ASTERISK = "\u273B";  // ✻
inline constexpr std::string_view UP_ARROW = "\u2191";           // ↑
inline constexpr std::string_view DOWN_ARROW = "\u2193";         // ↓
inline constexpr std::string_view LIGHTNING_BOLT = "\u21AF";     // ↯
inline constexpr std::string_view EFFORT_LOW = "\u25CB";         // ○
inline constexpr std::string_view EFFORT_MEDIUM = "\u25D0";      // ◐
inline constexpr std::string_view EFFORT_HIGH = "\u25CF";        // ●
inline constexpr std::string_view EFFORT_MAX = "\u25C9";         // ◉

// Media/trigger status indicators
inline constexpr std::string_view PLAY_ICON = "\u25B6";   // ▶
inline constexpr std::string_view PAUSE_ICON = "\u23F8";  // ⏸

// MCP subscription indicators
inline constexpr std::string_view REFRESH_ARROW = "\u21BB";    // ↻
inline constexpr std::string_view CHANNEL_ARROW = "\u2190";    // ←
inline constexpr std::string_view INJECTED_ARROW = "\u2192";   // →
inline constexpr std::string_view FORK_GLYPH = "\u2442";       // ⑂

// Review status indicators
inline constexpr std::string_view DIAMOND_OPEN = "\u25C7";      // ◇
inline constexpr std::string_view DIAMOND_FILLED = "\u25C6";    // ◆
inline constexpr std::string_view REFERENCE_MARK = "\u203B";    // ※

// Issue flag indicator
inline constexpr std::string_view FLAG_ICON = "\u2691";  // ⚑

// Block/line indicators
inline constexpr std::string_view BLOCKQUOTE_BAR = "\u258E";       // ▎
inline constexpr std::string_view HEAVY_HORIZONTAL = "\u2501";     // ━

// Bridge status indicators
inline constexpr std::array<std::string_view, 4> BRIDGE_SPINNER_FRAMES = {
    "\u00B7|\u00B7",
    "\u00B7/\u00B7",
    "\u00B7\u2014\u00B7",
    "\u00B7\\\u00B7",
};
inline constexpr std::string_view BRIDGE_READY_INDICATOR = "\u00B7\u2714\uFE0E\u00B7";
inline constexpr std::string_view BRIDGE_FAILED_INDICATOR = "\u00D7";

} // namespace cc::constants::figures
