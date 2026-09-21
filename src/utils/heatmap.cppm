module;
#include <chrono>
#include <span>
#include <string>
#include <vector>

export module cc.utils.heatmap;

export namespace cc::utils {

using TimePoint = std::chrono::system_clock::time_point;

struct HeatmapData {
    std::vector<std::vector<int>> grid; // 7 rows (days) × N columns (weeks)
    int max_value;
};

// Generate a GitHub-style activity heatmap from timestamps
HeatmapData generate_activity_heatmap(std::span<TimePoint> timestamps, int weeks) {
    HeatmapData data;
    data.grid.resize(7, std::vector<int>(weeks, 0)); // 7 days per week
    data.max_value = 0;

    if (timestamps.empty()) return data;

    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm{};
    localtime_r(&now_t, &now_tm);

    // Calculate the start of the heatmap period
    auto period_start = now - std::chrono::hours(24 * 7 * weeks);

    for (auto& tp : timestamps) {
        if (tp < period_start || tp > now) continue;

        auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        localtime_r(&t, &tm);

        // Calculate week offset from start
        auto days_from_start = std::chrono::duration_cast<std::chrono::hours>(tp - period_start).count() / 24;
        int week_col = static_cast<int>(days_from_start) / 7;
        int day_row = tm.tm_wday; // 0=Sunday, 6=Saturday

        if (week_col >= 0 && week_col < weeks && day_row >= 0 && day_row < 7) {
            data.grid[day_row][week_col]++;
            data.max_value = std::max(data.max_value, data.grid[day_row][week_col]);
        }
    }

    return data;
}

// Render heatmap as ANSI-colored text
std::string render_heatmap_ansi(const HeatmapData& data) {
    if (data.grid.empty() || data.grid[0].empty()) return "";

    // Intensity levels using block characters and colors
    auto get_block = [&data](int value) -> std::string {
        if (value == 0) return "\033[38;5;236m░\033[0m"; // Dark gray
        if (data.max_value == 0) return "\033[38;5;236m░\033[0m";

        double ratio = static_cast<double>(value) / static_cast<double>(data.max_value);
        if (ratio < 0.25) return "\033[38;5;22m▒\033[0m";  // Dark green
        if (ratio < 0.50) return "\033[38;5;28m▓\033[0m";  // Medium green
        if (ratio < 0.75) return "\033[38;5;34m█\033[0m";  // Light green
        return "\033[38;5;46m█\033[0m";                     // Bright green
    };

    static const char* day_labels[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    std::string result;
    int cols = static_cast<int>(data.grid[0].size());

    for (int row = 0; row < 7; ++row) {
        result += day_labels[row];
        result += " ";
        for (int col = 0; col < cols; ++col) {
            result += get_block(data.grid[row][col]);
        }
        result += "\n";
    }

    return result;
}

} // namespace cc::utils
