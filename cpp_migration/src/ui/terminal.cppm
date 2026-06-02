/// @file terminal.cppm
/// @brief Terminal UI module using FTXUI for the Claude Code REPL.
/// Provides the main TerminalUI class, REPL loop, input/output rendering,
/// spinner, status bar, and key binding management.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <format>
#include <chrono>
#include <atomic>
#include <mutex>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.terminal;

import cc.types.types;
import cc.state.app_state;

export namespace cc::ui {

// ============================================================
// Color Theme
// ============================================================

/// Color palette for terminal rendering
struct ColorTheme {
    ftxui::Color primary       = ftxui::Color::Cyan;
    ftxui::Color secondary     = ftxui::Color::Blue;
    ftxui::Color accent        = ftxui::Color::Magenta;
    ftxui::Color success       = ftxui::Color::Green;
    ftxui::Color warning       = ftxui::Color::Yellow;
    ftxui::Color error         = ftxui::Color::Red;
    ftxui::Color text          = ftxui::Color::White;
    ftxui::Color muted         = ftxui::Color::GrayDark;
    ftxui::Color background    = ftxui::Color::Default;
    ftxui::Color user_prefix   = ftxui::Color::Green;
    ftxui::Color assist_prefix = ftxui::Color::Cyan;

    /// Default dark theme
    [[nodiscard]] static ColorTheme dark() { return ColorTheme{}; }

    /// Light theme variant
    [[nodiscard]] static ColorTheme light() {
        ColorTheme t;
        t.text = ftxui::Color::Black;
        t.muted = ftxui::Color::GrayLight;
        t.background = ftxui::Color::White;
        return t;
    }
};

// ============================================================
// Key Binding Configuration
// ============================================================

/// Action triggered by a key binding
enum class KeyAction : std::uint8_t {
    Submit,          // Send message (Enter)
    Interrupt,       // Ctrl+C interrupt current operation
    Exit,            // Ctrl+D exit REPL
    NewLine,         // Shift+Enter or Alt+Enter for multiline
    ClearScreen,     // Ctrl+L clear display
    ScrollUp,        // Page up
    ScrollDown,      // Page down
    HistoryPrev,     // Up arrow for previous input
    HistoryNext,     // Down arrow for next input
    TabComplete,     // Tab for command completion
};

/// Single key binding entry mapping an event to an action
struct KeyBinding {
    ftxui::Event event;
    KeyAction action;
    std::string description;
};

/// Default key bindings for the REPL
[[nodiscard]] inline std::vector<KeyBinding> default_key_bindings() {
    return {
        {ftxui::Event::Return, KeyAction::Submit, "Send message"},
        {ftxui::Event::Special("\x03"), KeyAction::Interrupt, "Interrupt"},
        {ftxui::Event::Special("\x04"), KeyAction::Exit, "Exit"},
        {ftxui::Event::Special("\x0C"), KeyAction::ClearScreen, "Clear screen"},
        {ftxui::Event::PageUp, KeyAction::ScrollUp, "Scroll up"},
        {ftxui::Event::PageDown, KeyAction::ScrollDown, "Scroll down"},
        {ftxui::Event::ArrowUp, KeyAction::HistoryPrev, "Previous input"},
        {ftxui::Event::ArrowDown, KeyAction::HistoryNext, "Next input"},
        {ftxui::Event::Tab, KeyAction::TabComplete, "Tab complete"},
    };
}

// ============================================================
// Spinner Component
// ============================================================

/// Animated spinner for loading states
class Spinner {
    std::atomic<bool> active_{false};
    std::uint32_t frame_index_{0};
    // Unicode braille animation frames
    static constexpr std::array<std::string_view, 8> frames_ = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"
    };
    std::string label_;

public:
    explicit Spinner(std::string label = "Thinking")
        : label_(std::move(label)) {}

    void start() noexcept { active_.store(true); }
    void stop() noexcept { active_.store(false); frame_index_ = 0; }
    void set_label(std::string label) { label_ = std::move(label); }
    [[nodiscard]] bool is_active() const noexcept { return active_.load(); }

    /// Advance frame and render current spinner element
    [[nodiscard]] ftxui::Element render(const ColorTheme& theme) {
        if (!active_.load()) return ftxui::text("");
        auto frame = frames_[frame_index_ % frames_.size()];
        frame_index_++;
        return ftxui::hbox({
            ftxui::text(std::string(frame)) | ftxui::color(theme.accent),
            ftxui::text(" " + label_ + "...") | ftxui::color(theme.muted),
        });
    }
};

// ============================================================
// Status Bar
// ============================================================

/// Status bar data for rendering at the bottom of the screen
struct StatusBarData {
    std::string model_name;
    std::uint32_t input_tokens = 0;
    std::uint32_t output_tokens = 0;
    double cost_usd = 0.0;
    std::optional<std::string> session_id;
};

/// Render the status bar element
[[nodiscard]] inline ftxui::Element render_status_bar(
    const StatusBarData& data, const ColorTheme& theme) {
    auto model_el = ftxui::text(std::format(" {} ", data.model_name))
                    | ftxui::color(theme.primary) | ftxui::bold;
    auto tokens_el = ftxui::text(std::format(" Tokens: {}↑ {}↓ ",
                                             data.input_tokens, data.output_tokens))
                     | ftxui::color(theme.muted);
    auto cost_el = ftxui::text(std::format(" ${:.4f} ", data.cost_usd))
                   | ftxui::color(theme.warning);
    return ftxui::hbox({
        model_el, ftxui::separator(), tokens_el,
        ftxui::separator(), cost_el, ftxui::filler(),
    }) | ftxui::borderLight;
}

// ============================================================
// Input Box
// ============================================================

/// Multiline input state
struct InputState {
    std::string content;
    std::vector<std::string> history;
    std::int32_t history_index = -1;
    bool multiline_mode = false;
};

// ============================================================
// TerminalUI - Main UI Controller
// ============================================================

/// Callback for when user submits a message
using OnSubmitCallback = std::function<void(std::string)>;
/// Callback for interrupt signal
using OnInterruptCallback = std::function<void()>;

/// Main terminal UI controller using FTXUI
class TerminalUI {
    ftxui::ScreenInteractive screen_;
    ColorTheme theme_;
    Spinner spinner_;
    InputState input_state_;
    StatusBarData status_;
    std::vector<KeyBinding> key_bindings_;

    OnSubmitCallback on_submit_;
    OnInterruptCallback on_interrupt_;

    // Scroll state
    std::int32_t scroll_offset_{0};
    std::int32_t max_scroll_{0};

    std::mutex render_mutex_;

public:
    /// Construct with optional custom theme
    explicit TerminalUI(ColorTheme theme = ColorTheme::dark())
        : screen_(ftxui::ScreenInteractive::Fullscreen()),
          theme_(std::move(theme)),
          spinner_("Thinking"),
          key_bindings_(default_key_bindings()) {}

    // -- Configuration --

    void set_theme(ColorTheme theme) { theme_ = std::move(theme); }
    void set_on_submit(OnSubmitCallback cb) { on_submit_ = std::move(cb); }
    void set_on_interrupt(OnInterruptCallback cb) { on_interrupt_ = std::move(cb); }

    /// Update status bar data (thread-safe)
    void update_status(StatusBarData data) {
        std::lock_guard lock(render_mutex_);
        status_ = std::move(data);
    }

    // -- Spinner control --
    void show_spinner(std::string label = "Thinking") {
        spinner_.set_label(std::move(label));
        spinner_.start();
    }
    void hide_spinner() { spinner_.stop(); }

    // -- Screen management --

    /// Clear the display and reset scroll
    void clear_screen() {
        scroll_offset_ = 0;
        screen_.PostEvent(ftxui::Event::Custom);
    }

    /// Scroll to the bottom of the message list
    void scroll_to_bottom() {
        scroll_offset_ = max_scroll_;
        screen_.PostEvent(ftxui::Event::Custom);
    }

    /// Request a UI refresh from any thread
    void request_refresh() {
        screen_.PostEvent(ftxui::Event::Custom);
    }

    /// Exit the REPL loop gracefully
    void exit() { screen_.Exit(); }

    // -- Main REPL loop --

    /// Build the main component tree and run the interactive loop.
    /// This blocks until the user exits.
    void run(const cc::state::AppState& initial_state) {
        std::string input_content;

        // Input component with multiline support
        auto input_option = ftxui::InputOption::Default();
        input_option.multiline = true;
        input_option.on_enter = [&] {
            if (!input_state_.multiline_mode && on_submit_ && !input_content.empty()) {
                on_submit_(input_content);
                input_state_.history.push_back(input_content);
                input_content.clear();
            }
        };
        auto input_component = ftxui::Input(&input_content, "Type a message...", input_option);

        // Wrap with key event handler
        auto main_component = ftxui::CatchEvent(input_component, [&](ftxui::Event event) {
            return handle_key_event(event, input_content);
        });

        // Renderer composing all UI elements
        auto renderer = ftxui::Renderer(main_component, [&] {
            return ftxui::vbox({
                render_message_area(),
                spinner_.render(theme_),
                ftxui::separator(),
                render_input_prompt(input_content),
                render_status_bar(status_, theme_),
            }) | ftxui::flex;
        });

        screen_.Loop(renderer);
    }

private:
    /// Handle key events and dispatch to actions
    bool handle_key_event(ftxui::Event& event, std::string& input) {
        // Ctrl+C: interrupt
        if (event == ftxui::Event::Special("\x03")) {
            if (on_interrupt_) on_interrupt_();
            return true;
        }
        // Ctrl+D: exit
        if (event == ftxui::Event::Special("\x04")) {
            screen_.Exit();
            return true;
        }
        // Ctrl+L: clear screen
        if (event == ftxui::Event::Special("\x0C")) {
            clear_screen();
            return true;
        }
        return false;
    }

    /// Render the scrollable message display area
    [[nodiscard]] ftxui::Element render_message_area() {
        return ftxui::vbox({
            ftxui::text("Welcome to Claude Code REPL") | ftxui::color(theme_.primary),
            ftxui::text("Type /help for commands") | ftxui::color(theme_.muted),
        }) | ftxui::flex | ftxui::focusPositionRelative(0, 1);
    }

    /// Render the input prompt with prefix indicator
    [[nodiscard]] ftxui::Element render_input_prompt(const std::string& content) {
        auto prefix = ftxui::text("> ") | ftxui::color(theme_.user_prefix) | ftxui::bold;
        auto input_el = ftxui::text(content.empty() ? "Type a message..." : content);
        return ftxui::hbox({prefix, input_el});
    }
};

} // namespace cc::ui
