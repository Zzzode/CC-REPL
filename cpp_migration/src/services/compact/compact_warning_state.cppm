/// @file compact_warning_state.cppm
/// @brief Context window warning state tracking
module;
#include <string>
#include <cstdint>
#include <optional>
export module cc.services.compact.compact_warning_state;
export namespace cc::services::compact {
enum class WarningLevel { None, Yellow, Orange, Red };
struct CompactWarningState { WarningLevel level{WarningLevel::None}; double usage_pct{0}; std::optional<std::string> suggested_action; };
[[nodiscard]] inline CompactWarningState compute_warning(double usage_pct) {
    if (usage_pct > 0.95) return {WarningLevel::Red, usage_pct, "auto-compact recommended"};
    if (usage_pct > 0.85) return {WarningLevel::Orange, usage_pct, "consider compacting"};
    if (usage_pct > 0.70) return {WarningLevel::Yellow, usage_pct, std::nullopt};
    return {WarningLevel::None, usage_pct, std::nullopt};
}
} // namespace
