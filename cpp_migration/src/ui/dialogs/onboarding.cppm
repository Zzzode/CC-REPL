module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>

export module cc.ui.dialogs.onboarding;

export namespace cc::ui::dialogs {

[[nodiscard]] inline std::string repeat_onboarding(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// A step in the onboarding flow
struct OnboardingStep {
    std::string title;
    std::string description;
    std::optional<std::string> action;
    bool completed = false;
};

// Get the default onboarding steps for new users
inline auto get_onboarding_steps() -> std::vector<OnboardingStep> {
    return {
        {
            "Welcome to CC-REPL",
            "A powerful CLI interface for Claude. Let's get you set up.",
            std::nullopt,
            false
        },
        {
            "API Key",
            "Configure your Anthropic API key for authentication.",
            "Run: /config set api_key <your-key>",
            false
        },
        {
            "Choose a Model",
            "Select your preferred Claude model.",
            "Run: /model",
            false
        },
        {
            "Try a Command",
            "Send your first message or try a slash command.",
            "Type: Hello, Claude!",
            false
        },
        {
            "Explore Features",
            "Use /help to discover all available commands and shortcuts.",
            "Run: /help",
            false
        },
    };
}

// Mark a specific step as complete (mutates the step vector)
inline auto mark_step_complete(std::vector<OnboardingStep>& steps, int step) -> void {
    if (step >= 0 && step < static_cast<int>(steps.size())) {
        steps[step].completed = true;
    }
}

// Render the onboarding wizard dialog
inline auto render_onboarding(std::vector<OnboardingStep> steps,
                              int current_step,
                              int width) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(40, width - 4);

    // Top border
    out << "╭" << repeat_onboarding("─", inner_width) << "╮\n";

    // Progress indicator
    out << "│ ";
    for (size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].completed) {
            out << "\033[32m●\033[0m";
        } else if (static_cast<int>(i) == current_step) {
            out << "\033[36m◉\033[0m";
        } else {
            out << "\033[2m○\033[0m";
        }
        if (i < steps.size() - 1) out << "─";
    }
    int progress_len = static_cast<int>(steps.size()) * 2 - 1;
    int progress_pad = inner_width - 1 - progress_len;
    if (progress_pad > 0) out << std::string(progress_pad, ' ');
    out << "│\n";
    out << "├" << repeat_onboarding("─", inner_width) << "┤\n";

    // Current step content
    if (current_step >= 0 && current_step < static_cast<int>(steps.size())) {
        const auto& step = steps[current_step];

        // Step number and title
        out << "│" << std::string(inner_width, ' ') << "│\n";
        std::string step_header = "Step " + std::to_string(current_step + 1) + "/" +
                                   std::to_string(steps.size()) + ": " + step.title;
        out << "│ \033[1m" << step_header << "\033[0m";
        int header_pad = inner_width - 1 - static_cast<int>(step_header.size());
        if (header_pad > 0) out << std::string(header_pad, ' ');
        out << "│\n";

        // Description
        out << "│" << std::string(inner_width, ' ') << "│\n";
        out << "│ " << step.description;
        int desc_pad = inner_width - 1 - static_cast<int>(step.description.size());
        if (desc_pad > 0) out << std::string(desc_pad, ' ');
        out << "│\n";

        // Action hint
        if (step.action.has_value()) {
            out << "│" << std::string(inner_width, ' ') << "│\n";
            out << "│ \033[36m→ " << step.action.value() << "\033[0m";
            int action_pad = inner_width - 3 - static_cast<int>(step.action->size());
            if (action_pad > 0) out << std::string(action_pad, ' ');
            out << "│\n";
        }

        out << "│" << std::string(inner_width, ' ') << "│\n";
    }

    // Navigation hints
    out << "├" << repeat_onboarding("─", inner_width) << "┤\n";
    std::string nav = "[←/→] Navigate  [Enter] Continue  [Esc] Skip";
    out << "│ \033[2m" << nav << "\033[0m";
    int nav_pad = inner_width - 1 - static_cast<int>(nav.size());
    if (nav_pad > 0) out << std::string(nav_pad, ' ');
    out << "│\n";

    // Bottom border
    out << "╰" << repeat_onboarding("─", inner_width) << "╯";

    return out.str();
}

} // namespace cc::ui::dialogs
