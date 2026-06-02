module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>
#include <span>

export module cc.ui.dialogs.quick_open;

export namespace cc::ui::dialogs {

[[nodiscard]] inline std::string repeat_quick_open(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// An item in the quick-open palette
struct QuickOpenItem {
    std::string label;
    std::string description;
    std::optional<std::string> shortcut;
    std::string category;
};

// Filter items based on a fuzzy query string
inline auto filter_items(std::span<QuickOpenItem> items,
                         std::string_view query) -> std::vector<QuickOpenItem> {
    if (query.empty()) {
        return std::vector<QuickOpenItem>(items.begin(), items.end());
    }

    std::vector<QuickOpenItem> results;

    for (const auto& item : items) {
        // Simple substring matching (case-insensitive)
        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            return s;
        };

        std::string lower_label = to_lower(item.label);
        std::string lower_desc = to_lower(item.description);
        std::string lower_query = to_lower(std::string(query));

        // Check if query chars appear in order (fuzzy match)
        bool match = false;
        size_t qi = 0;
        for (size_t li = 0; li < lower_label.size() && qi < lower_query.size(); ++li) {
            if (lower_label[li] == lower_query[qi]) ++qi;
        }
        if (qi == lower_query.size()) match = true;

        // Also check description
        if (!match) {
            qi = 0;
            for (size_t li = 0; li < lower_desc.size() && qi < lower_query.size(); ++li) {
                if (lower_desc[li] == lower_query[qi]) ++qi;
            }
            if (qi == lower_query.size()) match = true;
        }

        if (match) {
            results.push_back(item);
        }
    }

    return results;
}

// Render the quick-open palette dialog
inline auto render_quick_open(std::vector<QuickOpenItem> items,
                              std::string_view query,
                              int selected,
                              int width,
                              int height) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(30, width - 4);
    int max_visible = std::max(3, height - 6);

    // Top border
    out << "╭" << repeat_quick_open("─", inner_width) << "╮\n";

    // Search input
    out << "│ \033[36m❯\033[0m ";
    if (query.empty()) {
        out << "\033[2mType to search...\033[0m";
        int pad = inner_width - 21;
        if (pad > 0) out << std::string(pad, ' ');
    } else {
        out << query;
        int pad = inner_width - 3 - static_cast<int>(query.size());
        if (pad > 0) out << std::string(pad, ' ');
    }
    out << "│\n";
    out << "├" << repeat_quick_open("─", inner_width) << "┤\n";

    // Item list
    if (items.empty()) {
        out << "│ \033[2mNo results\033[0m"
            << std::string(std::max(0, inner_width - 11), ' ') << "│\n";
    } else {
        // Calculate visible window
        int start = 0;
        if (selected >= max_visible) {
            start = selected - max_visible + 1;
        }
        int end = std::min(static_cast<int>(items.size()), start + max_visible);

        std::string current_category;
        for (int i = start; i < end; ++i) {
            const auto& item = items[i];

            // Category header
            if (item.category != current_category) {
                current_category = item.category;
                out << "│ \033[2m── " << current_category << " ──\033[0m";
                int cat_pad = inner_width - 7 - static_cast<int>(current_category.size());
                if (cat_pad > 0) out << std::string(cat_pad, ' ');
                out << "│\n";
            }

            // Item row
            bool is_selected = (i == selected);
            out << "│ ";
            if (is_selected) {
                out << "\033[36m❯\033[0m \033[1m" << item.label << "\033[0m";
            } else {
                out << "  " << item.label;
            }

            // Shortcut on right side
            int used = 4 + static_cast<int>(item.label.size());
            if (item.shortcut.has_value()) {
                int gap = inner_width - used - static_cast<int>(item.shortcut->size()) - 2;
                if (gap > 0) out << std::string(gap, ' ');
                out << "\033[2m" << item.shortcut.value() << "\033[0m ";
            } else {
                int gap = inner_width - used;
                if (gap > 0) out << std::string(gap, ' ');
            }
            out << "│\n";

            // Description (only for selected item)
            if (is_selected && !item.description.empty()) {
                out << "│   \033[2m" << item.description << "\033[0m";
                int desc_pad = inner_width - 3 - static_cast<int>(item.description.size());
                if (desc_pad > 0) out << std::string(desc_pad, ' ');
                out << "│\n";
            }
        }

        // Scroll indicators
        if (start > 0) {
            out << "│ \033[2m  ↑ " << start << " more\033[0m"
                << std::string(std::max(0, inner_width - 10), ' ') << "│\n";
        }
        int remaining = static_cast<int>(items.size()) - end;
        if (remaining > 0) {
            out << "│ \033[2m  ↓ " << remaining << " more\033[0m"
                << std::string(std::max(0, inner_width - 10), ' ') << "│\n";
        }
    }

    // Bottom border
    out << "╰" << repeat_quick_open("─", inner_width) << "╯";

    return out.str();
}

} // namespace cc::ui::dialogs
