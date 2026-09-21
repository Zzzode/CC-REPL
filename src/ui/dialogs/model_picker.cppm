module;
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

export module cc.ui.dialogs.model_picker;

export namespace cc::ui::dialogs {

[[nodiscard]] inline std::string repeat_model_picker(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// A selectable model option
struct ModelOption {
    std::string id;
    std::string display_name;
    std::string tier;      // "standard", "pro", "enterprise"
    bool available = true;
};

// Render detailed information about a model
inline auto render_model_details(ModelOption option) -> std::string {
    std::ostringstream out;

    out << "\033[1m" << option.display_name << "\033[0m\n";
    out << "  ID:   " << option.id << "\n";
    out << "  Tier: ";

    if (option.tier == "pro") {
        out << "\033[33m⭐ Pro\033[0m";
    } else if (option.tier == "enterprise") {
        out << "\033[35m💎 Enterprise\033[0m";
    } else {
        out << "\033[32m● Standard\033[0m";
    }
    out << "\n";

    if (!option.available) {
        out << "  \033[31m⚠ Currently unavailable\033[0m";
    }

    return out.str();
}

// Render the model picker list
inline auto render_model_picker(std::vector<ModelOption> options,
                                 int selected_index,
                                 int width) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(30, width - 4);

    // Title
    out << "╭" << repeat_model_picker("─", inner_width) << "╮\n";
    out << "│ \033[1mSelect Model\033[0m"
        << std::string(std::max(0, inner_width - 13), ' ') << "│\n";
    out << "├" << repeat_model_picker("─", inner_width) << "┤\n";

    // Model list
    for (size_t i = 0; i < options.size(); ++i) {
        const auto& opt = options[i];
        bool selected = (static_cast<int>(i) == selected_index);

        out << "│ ";

        // Selection indicator
        if (selected) {
            out << "\033[36m❯\033[0m ";
        } else {
            out << "  ";
        }

        // Availability indicator
        if (!opt.available) {
            out << "\033[2m";
        } else if (selected) {
            out << "\033[1m";
        }

        // Model name
        out << opt.display_name;

        if (!opt.available) {
            out << " (unavailable)";
        }

        out << "\033[0m";

        // Tier badge on right
        std::string tier_badge;
        if (opt.tier == "pro") tier_badge = "⭐";
        else if (opt.tier == "enterprise") tier_badge = "💎";

        int used = 4 + static_cast<int>(opt.display_name.size());
        if (!opt.available) used += 14;
        int gap = inner_width - used - static_cast<int>(tier_badge.size()) - 1;
        if (gap > 0) out << std::string(gap, ' ');
        if (!tier_badge.empty()) out << tier_badge;
        out << " │\n";
    }

    // Bottom border
    out << "╰" << repeat_model_picker("─", inner_width) << "╯";

    // Details panel for selected model
    if (selected_index >= 0 && selected_index < static_cast<int>(options.size())) {
        out << "\n\n" << render_model_details(options[selected_index]);
    }

    return out.str();
}

} // namespace cc::ui::dialogs
