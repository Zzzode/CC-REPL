// User command message - displays slash commands executed by the user
module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_user_command;

export namespace cc::ui::messages::user_command {
using namespace ftxui;

/// Data for a user command message
struct UserCommandData {
    std::string command_name;              // e.g., "help", "config", "model"
    std::vector<std::string> args;         // Command arguments
    std::optional<std::string> output;     // Command output/result
    bool success = true;                   // Whether command succeeded
};

/// Render a user command message
[[nodiscard]] inline Element render(const UserCommandData& data) {
    Elements elements;
    
    // Command line
    Elements cmd_parts;
    cmd_parts.push_back(text("/") | color(Color::Cyan) | bold);
    cmd_parts.push_back(text(data.command_name) | color(Color::Cyan) | bold);
    
    for (const auto& arg : data.args) {
        cmd_parts.push_back(text(" " + arg) | color(Color::White));
    }
    
    elements.push_back(hbox(cmd_parts));
    
    // Output
    if (data.output && !data.output->empty()) {
        auto output_color = data.success ? Color::GrayLight : Color::Red;
        
        // Split output into lines, limit display
        std::string_view out = *data.output;
        int lines = 0;
        size_t pos = 0;
        while (pos < out.size() && lines < 10) {
            auto nl = out.find('\n', pos);
            std::string line;
            if (nl == std::string_view::npos) {
                line = std::string(out.substr(pos));
                pos = out.size();
            } else {
                line = std::string(out.substr(pos, nl - pos));
                pos = nl + 1;
            }
            elements.push_back(text("  " + line) | color(output_color) | dim);
            ++lines;
        }
        if (pos < out.size()) {
            elements.push_back(text("  ...") | dim);
        }
    }
    
    return vbox(elements);
}

} // namespace cc::ui::messages::user_command
