// C++23 Module: Input handling hooks with mode switching, key detection, and clipboard
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module cc.hooks.input_hooks;


export namespace cc::hooks {

// Input processing modes
enum class InputMode { Normal, Vim, Search, Modal };

// Key event representation
struct KeyEvent {
    std::string key;              // Key name: "a", "Enter", "Escape", "Tab"
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};             // Cmd on macOS
    std::chrono::steady_clock::time_point timestamp;

    // Generate canonical string representation for matching
    [[nodiscard]] auto canonical() const -> std::string {
        std::string result;
        if (ctrl)  result += "Ctrl+";
        if (alt)   result += "Alt+";
        if (shift) result += "Shift+";
        if (meta)  result += "Meta+";
        result += key;
        return result;
    }

    // Check if this key matches a pattern string (e.g., "Ctrl+C")
    [[nodiscard]] auto matches(std::string_view pattern) const -> bool {
        return canonical() == pattern;
    }
};

// Action result from key processing
enum class ActionType {
    Submit,       // Submit current input
    Cancel,       // Cancel operation
    Navigate,     // History navigation
    Complete,     // Tab completion
    ModeSwitch,   // Mode change
    Custom,       // User-defined action
    Passthrough   // Key not consumed
};

// Result of processing a key event
struct InputAction {
    ActionType type{ActionType::Passthrough};
    std::string action_name;       // For Custom type
    std::optional<std::string> payload;

    [[nodiscard]] auto consumed() const -> bool { return type != ActionType::Passthrough; }
};

// Key pattern for handler registration
struct KeyPattern {
    std::string pattern;       // e.g., "Ctrl+C", "Enter", "a"
    InputMode mode;            // Which mode this applies in

    auto operator==(const KeyPattern&) const -> bool = default;
};

// Handler callback type
using KeyHandler = std::function<InputAction(const KeyEvent&)>;

// DoublePress detector: detects repeated key presses within a time window
class DoublePressDetector {
    using Clock = std::chrono::steady_clock;
public:
    explicit DoublePressDetector(std::chrono::milliseconds window = std::chrono::milliseconds(500))
        : window_(window) {}

    // Feed a key event; returns true if it's a double-press of the same key
    [[nodiscard]] auto detect(const KeyEvent& event) -> bool {
        auto now = Clock::now();
        if (last_key_ == event.canonical() && last_time_ &&
            (now - *last_time_) <= window_) {
            last_key_.clear();
            last_time_ = std::nullopt;
            return true; // Double press detected
        }
        last_key_ = event.canonical();
        last_time_ = now;
        return false;
    }

    auto reset() -> void { last_key_.clear(); last_time_ = std::nullopt; }

private:
    std::chrono::milliseconds window_;
    std::string last_key_;
    std::optional<Clock::time_point> last_time_;
};

// Idle timeout handler: fires callback after N seconds of no input
class IdleTimeoutHandler {
    using Clock = std::chrono::steady_clock;
public:
    explicit IdleTimeoutHandler(std::chrono::seconds timeout = std::chrono::seconds(300))
        : timeout_(timeout) {}

    // Record activity (call on every key event)
    auto record_activity() -> void { last_activity_ = Clock::now(); }

    // Check if idle timeout has been reached
    [[nodiscard]] auto is_idle() const -> bool {
        if (!last_activity_) return false;
        return (Clock::now() - *last_activity_) >= timeout_;
    }

    // Get seconds remaining until idle
    [[nodiscard]] auto seconds_until_idle() const -> std::size_t {
        if (!last_activity_) return timeout_.count();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - *last_activity_);
        return elapsed < timeout_ ? (timeout_ - elapsed).count() : 0;
    }

    auto set_timeout(std::chrono::seconds timeout) -> void { timeout_ = timeout; }
    auto set_callback(std::function<void()> cb) -> void { on_idle_ = std::move(cb); }

    // Check and fire callback if idle
    auto check_and_fire() -> void {
        if (is_idle() && on_idle_ && !fired_) {
            on_idle_();
            fired_ = true;
        }
    }

    // Reset fired state (call when activity resumes)
    auto resume() -> void { fired_ = false; record_activity(); }

private:
    std::chrono::seconds timeout_;
    std::optional<Clock::time_point> last_activity_;
    std::function<void()> on_idle_;
    bool fired_{false};
};

// Clipboard integration: read/write system clipboard
class ClipboardManager {
public:
    // Write text to system clipboard (platform-dependent implementation)
    [[nodiscard]] auto write(std::string_view text) -> std::expected<void, std::string> {
        // Implementation would use pbcopy on macOS, xclip on Linux
        clipboard_content_ = std::string(text);
        return {};
    }

    // Read text from system clipboard
    [[nodiscard]] auto read() const -> std::expected<std::string, std::string> {
        if (clipboard_content_.empty()) {
            return std::unexpected("Clipboard is empty");
        }
        return clipboard_content_;
    }

    // Check if clipboard has content
    [[nodiscard]] auto has_content() const -> bool { return !clipboard_content_.empty(); }

private:
    std::string clipboard_content_; // Fallback internal clipboard
};

// Paste detector: distinguish rapid keystrokes from paste events
class PasteDetector {
    using Clock = std::chrono::steady_clock;
public:
    // Threshold: if characters arrive faster than this, it's likely a paste
    explicit PasteDetector(std::chrono::microseconds threshold = std::chrono::microseconds(5000))
        : threshold_(threshold) {}

    // Feed a character timestamp, returns true if currently in paste mode
    [[nodiscard]] auto feed(const KeyEvent& event) -> bool {
        if (last_time_) {
            auto delta = event.timestamp - *last_time_;
            if (delta < threshold_) {
                consecutive_rapid_++;
            } else {
                if (consecutive_rapid_ > 3) {
                    // End of paste
                    consecutive_rapid_ = 0;
                    last_time_ = event.timestamp;
                    return false;
                }
                consecutive_rapid_ = 0;
            }
        }
        last_time_ = event.timestamp;
        return consecutive_rapid_ > 3; // Paste detected after 4 rapid chars
    }

    [[nodiscard]] auto is_pasting() const -> bool { return consecutive_rapid_ > 3; }
    auto reset() -> void { consecutive_rapid_ = 0; last_time_ = std::nullopt; }

private:
    std::chrono::microseconds threshold_;
    std::optional<Clock::time_point> last_time_;
    std::size_t consecutive_rapid_{0};
};

// InputHookManager: central input processing hub
class InputHookManager {
public:
    InputHookManager() = default;

    // Set current input mode
    auto set_mode(InputMode mode) -> void { current_mode_ = mode; }
    [[nodiscard]] auto mode() const -> InputMode { return current_mode_; }

    // Register a key handler for a specific mode and pattern
    auto register_handler(InputMode mode, std::string_view key_pattern, KeyHandler handler) -> void {
        handlers_.push_back({
            .pattern = KeyPattern{.pattern = std::string(key_pattern), .mode = mode},
            .handler = std::move(handler)
        });
    }

    // Process a key event through the handler chain
    [[nodiscard]] auto process_key(const KeyEvent& event) -> InputAction {
        // Record activity for idle detection
        idle_handler_.record_activity();
        idle_handler_.resume();

        // Check double-press
        if (double_press_.detect(event)) {
            // Route to double-press specific handlers
            auto dp_key = event.canonical() + "+" + event.canonical();
            for (const auto& entry : handlers_) {
                if (entry.pattern.mode == current_mode_ && entry.pattern.pattern == dp_key) {
                    auto action = entry.handler(event);
                    if (action.consumed()) return action;
                }
            }
        }

        // Feed paste detector
        paste_detector_.feed(event);

        // Find matching handler for current mode
        auto canonical = event.canonical();
        for (const auto& entry : handlers_) {
            if (entry.pattern.mode == current_mode_ && entry.pattern.pattern == canonical) {
                auto action = entry.handler(event);
                if (action.consumed()) return action;
            }
        }

        // Global handlers (mode-independent) — registered with InputMode::Normal as fallback
        if (current_mode_ != InputMode::Normal) {
            for (const auto& entry : handlers_) {
                if (entry.pattern.mode == InputMode::Normal && entry.pattern.pattern == canonical) {
                    auto action = entry.handler(event);
                    if (action.consumed()) return action;
                }
            }
        }

        return InputAction{.type = ActionType::Passthrough};
    }

    // Access sub-systems
    [[nodiscard]] auto double_press() -> DoublePressDetector& { return double_press_; }
    [[nodiscard]] auto idle_handler() -> IdleTimeoutHandler& { return idle_handler_; }
    [[nodiscard]] auto clipboard() -> ClipboardManager& { return clipboard_; }
    [[nodiscard]] auto paste_detector() -> PasteDetector& { return paste_detector_; }

    // Check if currently in paste mode
    [[nodiscard]] auto is_pasting() const -> bool { return paste_detector_.is_pasting(); }

private:
    struct HandlerEntry {
        KeyPattern pattern;
        KeyHandler handler;
    };

    InputMode current_mode_{InputMode::Normal};
    std::vector<HandlerEntry> handlers_;
    DoublePressDetector double_press_;
    IdleTimeoutHandler idle_handler_;
    ClipboardManager clipboard_;
    PasteDetector paste_detector_;
};

// Factory: create pre-configured InputHookManager with common bindings
[[nodiscard]] inline auto create_default_input_hooks() -> InputHookManager {
    InputHookManager manager;

    // Ctrl+C: cancel
    manager.register_handler(InputMode::Normal, "Ctrl+c", [](const KeyEvent&) {
        return InputAction{.type = ActionType::Cancel, .action_name = "cancel"};
    });

    // Enter: submit
    manager.register_handler(InputMode::Normal, "Enter", [](const KeyEvent&) {
        return InputAction{.type = ActionType::Submit, .action_name = "submit"};
    });

    // Tab: complete
    manager.register_handler(InputMode::Normal, "Tab", [](const KeyEvent&) {
        return InputAction{.type = ActionType::Complete, .action_name = "tab_complete"};
    });

    // Ctrl+R: search mode
    manager.register_handler(InputMode::Normal, "Ctrl+r", [&manager](const KeyEvent&) {
        return InputAction{.type = ActionType::ModeSwitch, .action_name = "search",
                          .payload = "search"};
    });

    // Escape: back to normal
    manager.register_handler(InputMode::Search, "Escape", [](const KeyEvent&) {
        return InputAction{.type = ActionType::ModeSwitch, .action_name = "normal",
                          .payload = "normal"};
    });

    return manager;
}

} // namespace cc::hooks
