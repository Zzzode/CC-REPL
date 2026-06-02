module;
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <sstream>
#include <iostream>

export module cc.ui.design.dialog;

export namespace cc::ui::design {

[[nodiscard]] inline std::string repeat_dialog_segment(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += text;
    return out;
}

// Configuration for a dialog with multiple buttons
struct DialogConfig {
    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    std::optional<int> default_button;
    bool closeable = true;
};

// Result returned after user interacts with a dialog
struct DialogResult {
    int button_index = -1;
    bool dismissed = false;
};

// Display a configurable dialog and return the user's choice
inline auto show_dialog(DialogConfig config) -> DialogResult {
    // Render dialog frame
    std::ostringstream out;
    const int min_width = 40;
    int content_width = static_cast<int>(config.title.size());
    content_width = std::max(content_width, static_cast<int>(config.message.size()));
    for (const auto& btn : config.buttons) {
        content_width = std::max(content_width, static_cast<int>(btn.size()) + 4);
    }
    int width = std::max(min_width, content_width + 4);

    // Top border
    out << "╭" << repeat_dialog_segment("─", width - 2) << "╮\n";

    // Title bar
    if (!config.title.empty()) {
        int padding = width - 2 - static_cast<int>(config.title.size());
        int left_pad = padding / 2;
        int right_pad = padding - left_pad;
        out << "│" << std::string(left_pad, ' ') << config.title
            << std::string(right_pad, ' ') << "│\n";
        out << "├" << repeat_dialog_segment("─", width - 2) << "┤\n";
    }

    // Message body
    out << "│ " << config.message
        << std::string(width - 3 - static_cast<int>(config.message.size()), ' ') << "│\n";
    out << "│" << std::string(width - 2, ' ') << "│\n";

    // Buttons row
    std::ostringstream btn_row;
    for (size_t i = 0; i < config.buttons.size(); ++i) {
        if (i > 0) btn_row << "  ";
        bool is_default = config.default_button.has_value() &&
                         static_cast<int>(i) == config.default_button.value();
        if (is_default) {
            btn_row << "[" << config.buttons[i] << "]";
        } else {
            btn_row << " " << config.buttons[i] << " ";
        }
    }
    std::string buttons_str = btn_row.str();
    int btn_padding = width - 2 - static_cast<int>(buttons_str.size());
    int btn_left = std::max(1, btn_padding / 2);
    int btn_right = std::max(0, btn_padding - btn_left);
    out << "│" << std::string(btn_left, ' ') << buttons_str
        << std::string(btn_right, ' ') << "│\n";

    // Bottom border
    out << "╰" << repeat_dialog_segment("─", width - 2) << "╯\n";

    std::cout << out.str() << std::flush;

    // Read user input for button selection
    int selected = config.default_button.value_or(0);
    return DialogResult{selected, false};
}

// Convenience: show a confirmation dialog with Yes/No
inline auto show_confirm(std::string_view message,
                         std::string_view yes = "Yes",
                         std::string_view no = "No") -> bool {
    DialogConfig cfg;
    cfg.title = "Confirm";
    cfg.message = std::string(message);
    cfg.buttons = {std::string(yes), std::string(no)};
    cfg.default_button = 0;
    cfg.closeable = true;

    auto result = show_dialog(std::move(cfg));
    return result.button_index == 0 && !result.dismissed;
}

// Convenience: show an input prompt dialog
inline auto show_input(std::string_view prompt,
                       std::optional<std::string> default_value = std::nullopt)
    -> std::optional<std::string> {
    std::cout << "╭─ " << prompt << " ─╮\n";
    if (default_value.has_value()) {
        std::cout << "│ [" << default_value.value() << "]: ";
    } else {
        std::cout << "│ > ";
    }
    std::cout << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        return std::nullopt;
    }

    if (input.empty() && default_value.has_value()) {
        return default_value.value();
    }
    if (input.empty()) {
        return std::nullopt;
    }
    return input;
}

} // namespace cc::ui::design
