/// @file modal.cppm
/// @brief Modal dialog context management.
/// Migrated from src/context/modalContext.tsx
module;

#include <string>
#include <optional>
#include <functional>
#include <variant>
#include <vector>

export module cc.context.modal;

export namespace cc::context {

/// Modal action button
struct ModalAction {
    std::string label;
    std::function<void()> on_press;
    bool is_destructive = false;
    bool is_primary = false;
};

/// Modal dialog configuration
struct ModalConfig {
    std::string title;
    std::string body;
    std::vector<ModalAction> actions;
    bool dismissible = true;
};

/// Modal context state
struct ModalState {
    std::optional<ModalConfig> current_modal;
    
    [[nodiscard]] bool is_open() const noexcept {
        return current_modal.has_value();
    }
};

/// Show a confirmation modal and return user choice
using ShowModalFn = std::function<void(ModalConfig)>;
using DismissModalFn = std::function<void()>;

} // namespace cc::context
