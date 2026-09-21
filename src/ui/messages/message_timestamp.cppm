module;
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

export module cc.ui.messages.message_timestamp;

export namespace cc::ui::messages {

// Render an absolute timestamp (HH:MM:SS format)
inline auto render_timestamp(std::chrono::system_clock::time_point ts) -> std::string {
    auto time_t_val = std::chrono::system_clock::to_time_t(ts);
    std::tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);

    std::ostringstream out;
    out << std::put_time(&tm_buf, "%H:%M:%S");
    return out.str();
}

// Render a relative timestamp (e.g., "2m ago", "just now")
inline auto render_relative_timestamp(std::chrono::system_clock::time_point ts) -> std::string {
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ts).count();

    std::ostringstream out;
    out << "\033[2m";

    if (elapsed < 5) {
        out << "just now";
    } else if (elapsed < 60) {
        out << elapsed << "s ago";
    } else if (elapsed < 3600) {
        out << (elapsed / 60) << "m ago";
    } else if (elapsed < 86400) {
        out << (elapsed / 3600) << "h ago";
    } else {
        out << (elapsed / 86400) << "d ago";
    }

    out << "\033[0m";
    return out.str();
}

// Render a duration badge (e.g., for response time)
inline auto render_duration_badge(std::chrono::milliseconds duration) -> std::string {
    std::ostringstream out;
    auto ms = duration.count();

    out << "\033[2m⏱ ";
    if (ms < 1000) {
        out << ms << "ms";
    } else if (ms < 60000) {
        out << std::fixed << std::setprecision(1)
            << (static_cast<double>(ms) / 1000.0) << "s";
    } else {
        int minutes = static_cast<int>(ms / 60000);
        int seconds = static_cast<int>((ms % 60000) / 1000);
        out << minutes << "m " << seconds << "s";
    }
    out << "\033[0m";

    return out.str();
}

} // namespace cc::ui::messages
