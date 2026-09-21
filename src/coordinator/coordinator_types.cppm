/// @file coordinator_types.cppm
/// @brief Coordinator mode types for multi-agent orchestration.
/// Migrated from src/coordinator/coordinatorMode.ts (supplements existing coordinator.cppm)
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

export module cc.coordinator.types;

export namespace cc::coordinator {

/// Coordinator mode determines how multi-agent work is orchestrated
enum class CoordinatorMode : std::uint8_t {
    /// Single agent, no coordination
    Solo,
    /// Coordinator spawns sub-agents for parallel work
    Parallel,
    /// Sequential task delegation
    Sequential,
    /// Team mode with persistent teammates
    Team,
};

/// Convert CoordinatorMode to display string
[[nodiscard]] constexpr std::string_view coordinator_mode_to_string(CoordinatorMode mode) noexcept {
    switch (mode) {
        case CoordinatorMode::Solo: return "solo";
        case CoordinatorMode::Parallel: return "parallel";
        case CoordinatorMode::Sequential: return "sequential";
        case CoordinatorMode::Team: return "team";
    }
    return "unknown";
}

/// Parse a string to CoordinatorMode
[[nodiscard]] inline std::optional<CoordinatorMode> parse_coordinator_mode(std::string_view str) {
    if (str == "solo") return CoordinatorMode::Solo;
    if (str == "parallel") return CoordinatorMode::Parallel;
    if (str == "sequential") return CoordinatorMode::Sequential;
    if (str == "team") return CoordinatorMode::Team;
    return std::nullopt;
}

} // namespace cc::coordinator
