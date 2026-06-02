module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>

export module cc.ui.prompt.footer;

export namespace cc::ui::prompt {

// Configuration for the prompt footer bar
struct FooterConfig {
    std::string model;
    std::optional<std::string> mode;
    std::vector<std::string> shortcuts;
    std::optional<int> tokens;
    std::optional<double> cost;
};

// Render the left portion of the footer (model and mode info)
inline auto render_footer_left(FooterConfig config) -> std::string {
    std::ostringstream out;

    // Model identifier
    out << "\033[1m" << config.model << "\033[0m";

    // Mode indicator if present
    if (config.mode.has_value()) {
        out << " \033[2m│\033[0m \033[33m" << config.mode.value() << "\033[0m";
    }

    return out.str();
}

// Render the right portion of the footer (tokens, cost, shortcuts)
inline auto render_footer_right(FooterConfig config) -> std::string {
    std::ostringstream out;

    // Token count
    if (config.tokens.has_value()) {
        out << "\033[2m" << config.tokens.value() << " tokens\033[0m";
    }

    // Cost display
    if (config.cost.has_value()) {
        if (config.tokens.has_value()) out << " \033[2m│\033[0m ";
        out << "\033[2m$" << std::fixed;
        // Format cost with appropriate precision
        double cost = config.cost.value();
        if (cost < 0.01) {
            out.precision(4);
        } else {
            out.precision(2);
        }
        out << cost << "\033[0m";
    }

    // Keyboard shortcuts
    if (!config.shortcuts.empty()) {
        if (config.tokens.has_value() || config.cost.has_value()) {
            out << "  ";
        }
        for (size_t i = 0; i < config.shortcuts.size(); ++i) {
            if (i > 0) out << " ";
            out << "\033[2m" << config.shortcuts[i] << "\033[0m";
        }
    }

    return out.str();
}

// Render the complete prompt footer bar spanning the given width
inline auto render_prompt_footer(FooterConfig config, int width) -> std::string {
    std::string left = render_footer_left(config);
    std::string right = render_footer_right(config);

    // Calculate visible lengths (strip ANSI for measurement)
    auto visible_len = [](const std::string& s) -> int {
        int len = 0;
        bool in_esc = false;
        for (char c : s) {
            if (c == '\033') { in_esc = true; continue; }
            if (in_esc) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_esc = false;
                continue;
            }
            ++len;
        }
        return len;
    };

    int left_len = visible_len(left);
    int right_len = visible_len(right);
    int gap = std::max(1, width - left_len - right_len);

    std::ostringstream out;
    out << left << std::string(gap, ' ') << right;
    return out.str();
}

} // namespace cc::ui::prompt
