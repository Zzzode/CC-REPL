module;
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

export module cc.ui.messages.message_row;

export namespace cc::ui::messages {

// Configuration for rendering a single message row
struct MessageRowConfig {
    std::string role;
    std::string content;
    std::optional<std::chrono::system_clock::time_point> timestamp;
    std::optional<std::string> model;
    bool is_streaming = false;
};

// Render a role badge (colored label for the message author)
inline auto render_role_badge(std::string_view role) -> std::string {
    std::ostringstream out;

    if (role == "user" || role == "human") {
        out << "\033[34m⦿ You\033[0m";
    } else if (role == "assistant") {
        out << "\033[35m◈ Assistant\033[0m";
    } else if (role == "system") {
        out << "\033[33m⚙ System\033[0m";
    } else if (role == "tool") {
        out << "\033[36m⚡ Tool\033[0m";
    } else {
        out << "\033[2m○ " << role << "\033[0m";
    }

    return out.str();
}

// Render a complete message row with role, content, and metadata
inline auto render_message_row(MessageRowConfig config, int width) -> std::string {
    std::ostringstream out;

    // Header line: role badge + optional model + optional timestamp
    out << render_role_badge(config.role);

    if (config.model.has_value()) {
        out << " \033[2m(" << config.model.value() << ")\033[0m";
    }

    if (config.timestamp.has_value()) {
        auto time_t_val = std::chrono::system_clock::to_time_t(config.timestamp.value());
        std::tm tm_buf{};
        localtime_r(&time_t_val, &tm_buf);
        out << " \033[2m" << std::put_time(&tm_buf, "%H:%M") << "\033[0m";
    }

    // Streaming indicator
    if (config.is_streaming) {
        out << " \033[36m▍\033[0m";
    }

    out << "\n";

    // Content body with left margin
    const std::string indent = "  ";
    std::istringstream content_stream(config.content);
    std::string line;
    while (std::getline(content_stream, line)) {
        // Simple word-wrap for long lines
        while (static_cast<int>(line.size()) > width - 2) {
            out << indent << line.substr(0, width - 2) << "\n";
            line = line.substr(width - 2);
        }
        out << indent << line << "\n";
    }

    return out.str();
}

} // namespace cc::ui::messages
