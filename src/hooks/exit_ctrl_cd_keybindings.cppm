module;
#include <string>
#include <string_view>
#include <functional>
#include <optional>

export module cc.hooks.exit_ctrl_cd_keybindings;

export namespace cc::hooks {

// Exit state exposed by the keybinding-aware exit hook
struct ExitKeybindingState {
    bool exit_pending{false};       // Whether a first Ctrl+C has been received
    bool is_active{true};           // Whether the keybinding is currently active
    std::optional<std::string> pending_message;  // Message shown after first press
};

// Callback types
using ExitCallback = std::function<void()>;
using InterruptCallback = std::function<bool()>;  // Return true if handled

// Keybinding provider interface (abstraction over the keybinding system)
using KeybindingRegistrar = std::function<void(
    std::string_view key_sequence,
    std::function<void()> handler,
    bool active
)>;

/// Convenience hook that wires up exit-on-ctrl-cd with the keybinding system.
///
/// This is the standard way to use exit handling in components. The separation
/// from exit_handler exists to avoid import cycles - exit_handler does not
/// depend on the keybinding module directly.
///
/// @param registrar    Keybinding registration function
/// @param on_exit      Optional custom exit handler
/// @param on_interrupt Optional callback for features to handle interrupt (ctrl+c).
///                     Return true if handled, false to fall through to double-press exit.
/// @param is_active    Whether the keybinding is active (default true).
class ExitCtrlCDWithKeybindings {
public:
    ExitCtrlCDWithKeybindings() = default;

    explicit ExitCtrlCDWithKeybindings(
        KeybindingRegistrar registrar,
        ExitCallback on_exit = nullptr,
        InterruptCallback on_interrupt = nullptr,
        bool is_active = true
    )   : registrar_(std::move(registrar))
        , on_exit_(std::move(on_exit))
        , on_interrupt_(std::move(on_interrupt))
    {
        state_.is_active = is_active;
        register_keybindings();
    }

    // Get the current exit state
    [[nodiscard]] auto state() const -> const ExitKeybindingState& {
        return state_;
    }

    // Set whether the keybinding is active
    auto set_active(bool active) -> void {
        state_.is_active = active;
        // Re-register with updated active state
        register_keybindings();
    }

    // Handle a Ctrl+C event (called by keybinding system)
    auto handle_ctrl_c() -> void {
        // First try the interrupt callback
        if (on_interrupt_ && on_interrupt_()) {
            // Interrupt was handled by the feature
            state_.exit_pending = false;
            state_.pending_message.reset();
            return;
        }

        if (state_.exit_pending) {
            // Second press - actually exit
            state_.exit_pending = false;
            state_.pending_message.reset();
            if (on_exit_) on_exit_();
        } else {
            // First press - mark pending
            state_.exit_pending = true;
            state_.pending_message = "Press Ctrl+C again to exit";
        }
    }

    // Handle a Ctrl+D event (called by keybinding system)
    auto handle_ctrl_d() -> void {
        // Ctrl+D on empty input exits immediately
        state_.exit_pending = false;
        state_.pending_message.reset();
        if (on_exit_) on_exit_();
    }

    // Reset the pending state (e.g., when user types something)
    auto reset_pending() -> void {
        state_.exit_pending = false;
        state_.pending_message.reset();
    }

private:
    ExitKeybindingState state_;
    KeybindingRegistrar registrar_;
    ExitCallback on_exit_;
    InterruptCallback on_interrupt_;

    auto register_keybindings() -> void {
        if (!registrar_) return;

        registrar_("Ctrl+C", [this]() { handle_ctrl_c(); }, state_.is_active);
        registrar_("Ctrl+D", [this]() { handle_ctrl_d(); }, state_.is_active);
    }
};

// Factory: create the keybinding-aware exit hook
[[nodiscard]] inline auto create_exit_keybindings(
    KeybindingRegistrar registrar,
    ExitCallback on_exit = nullptr,
    InterruptCallback on_interrupt = nullptr,
    bool is_active = true
) -> ExitCtrlCDWithKeybindings {
    return ExitCtrlCDWithKeybindings(
        std::move(registrar),
        std::move(on_exit),
        std::move(on_interrupt),
        is_active
    );
}

} // namespace cc::hooks
