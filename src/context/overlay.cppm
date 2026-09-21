/// @file overlay.cppm
/// @brief Overlay and prompt overlay context management.
/// Migrated from src/context/overlayContext.tsx and promptOverlayContext.tsx
module;

#include <cstdint>
#include <string>
#include <optional>
#include <functional>
#include <variant>
#include <vector>

export module cc.context.overlay;

export namespace cc::context {

/// Overlay types that can be displayed
enum class OverlayType : std::uint8_t {
    None,
    Help,
    Settings,
    Agents,
    Tasks,
    Compact,
    Memory,
    Team,
};

/// Prompt overlay types
enum class PromptOverlayType : std::uint8_t {
    None,
    FileSearch,
    CommandPalette,
    ModelSelector,
    PermissionRequest,
};

/// State for the overlay context
struct OverlayState {
    OverlayType current_overlay = OverlayType::None;
    std::optional<std::string> overlay_data;
    
    [[nodiscard]] bool is_open() const noexcept {
        return current_overlay != OverlayType::None;
    }
};

/// State for the prompt overlay context
struct PromptOverlayState {
    PromptOverlayType current_overlay = PromptOverlayType::None;
    std::optional<std::string> initial_query;
    
    [[nodiscard]] bool is_open() const noexcept {
        return current_overlay != PromptOverlayType::None;
    }
};

/// Callback to open/close an overlay
using SetOverlayFn = std::function<void(OverlayType, std::optional<std::string>)>;
using SetPromptOverlayFn = std::function<void(PromptOverlayType, std::optional<std::string>)>;

} // namespace cc::context
