module;
#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <algorithm>

export module cc.ui.dialogs.help_v2;

export namespace cc::ui::dialogs {

[[nodiscard]] inline std::string repeat_help_v2(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) result += text;
    return result;
}

// A section in the help dialog
struct HelpSection {
    std::string title;
    std::vector<std::pair<std::string, std::string>> items; // key-description pairs
};

// Get general help sections (keyboard shortcuts, etc.)
inline auto get_general_help_sections() -> std::vector<HelpSection> {
    return {
        {"Navigation", {
            {"↑/↓",        "Navigate history"},
            {"Ctrl+C",     "Cancel current operation"},
            {"Ctrl+D",     "Exit"},
            {"Ctrl+L",     "Clear screen"},
            {"Tab",        "Autocomplete"},
            {"Shift+Tab",  "Previous suggestion"},
        }},
        {"Input", {
            {"Enter",      "Submit message"},
            {"Shift+Enter","New line"},
            {"Ctrl+U",     "Clear input"},
            {"Ctrl+W",     "Delete word"},
            {"Ctrl+A",     "Move to start"},
            {"Ctrl+E",     "Move to end"},
        }},
        {"General", {
            {"/help",      "Show this help"},
            {"/compact",   "Compact conversation"},
            {"/clear",     "Clear conversation"},
            {"/config",    "Edit configuration"},
            {"Esc",        "Dismiss dialog"},
        }},
    };
}

// Get commands help sections
inline auto get_commands_help_sections() -> std::vector<HelpSection> {
    return {
        {"Session Commands", {
            {"/compact",   "Summarize and compact the conversation"},
            {"/clear",     "Start a new conversation"},
            {"/resume",    "Resume a previous session"},
            {"/history",   "Show conversation history"},
        }},
        {"Configuration", {
            {"/config",    "Open configuration editor"},
            {"/model",     "Switch model"},
            {"/mode",      "Switch input mode"},
            {"/doctor",    "Run diagnostics"},
        }},
        {"Tools", {
            {"/commit",    "Create a git commit"},
            {"/review",    "Code review"},
            {"/mcp",       "Manage MCP servers"},
            {"/vim",       "Toggle vim mode"},
        }},
    };
}

// Render the help dialog with all sections
inline auto render_help_dialog(std::vector<HelpSection> sections,
                                int width, int height) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(40, width - 4);

    // Top border with title
    out << "╭─ \033[1mHelp\033[0m " << repeat_help_v2("─", std::max(0, inner_width - 7)) << "╮\n";

    int lines_rendered = 2; // top border + bottom border

    for (const auto& section : sections) {
        if (lines_rendered >= height - 2) break;

        // Section title
        out << "│" << std::string(inner_width, ' ') << "│\n";
        out << "│ \033[1;36m" << section.title << "\033[0m";
        int title_pad = inner_width - 1 - static_cast<int>(section.title.size());
        if (title_pad > 0) out << std::string(title_pad, ' ');
        out << "│\n";
        lines_rendered += 2;

        // Items
        for (const auto& [key, desc] : section.items) {
            if (lines_rendered >= height - 2) break;

            out << "│   \033[33m" << key << "\033[0m";
            // Align descriptions
            int key_len = static_cast<int>(key.size());
            int gap = 14 - key_len;
            if (gap > 0) out << std::string(gap, ' ');
            out << "\033[2m" << desc << "\033[0m";

            int total_used = 3 + std::max(key_len, 14) + static_cast<int>(desc.size());
            int right_pad = inner_width - total_used;
            if (right_pad > 0) out << std::string(right_pad, ' ');
            out << "│\n";
            ++lines_rendered;
        }
    }

    // Bottom border with hint
    std::string hint = "Press Esc to close";
    out << "├" << repeat_help_v2("─", inner_width) << "┤\n";
    out << "│ \033[2m" << hint << "\033[0m";
    int hint_pad = inner_width - 1 - static_cast<int>(hint.size());
    if (hint_pad > 0) out << std::string(hint_pad, ' ');
    out << "│\n";
    out << "╰" << repeat_help_v2("─", inner_width) << "╯";

    return out.str();
}

} // namespace cc::ui::dialogs
