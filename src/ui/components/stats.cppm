module;

#include <string>
#include <vector>
#include <format>
#include <ftxui/dom/elements.hpp>

export module ui.components.stats;

export namespace ui::components {

struct ModelUsage {
    std::string model_name;
    int input_tokens = 0;
    int output_tokens = 0;
};

struct DailyActivity {
    std::string date;
    int session_count = 0;
};

struct StatsData {
    int total_sessions = 0;
    int total_tokens = 0;
    int active_days = 0;
    int longest_streak = 0;
    int current_streak = 0;
    std::string favorite_model;
    std::string longest_session_duration;
    std::string peak_activity_day;
    std::string fun_fact;
    std::vector<ModelUsage> model_usage;
    std::vector<DailyActivity> daily_activity;
};

struct StatsOptions {
    StatsData data;
    std::string active_tab = "Overview"; // "Overview" or "Models"
    int selected_date_range = 0; // 0: all, 1: 7d, 2: 30d
};

namespace detail {

std::vector<std::string> date_range_labels = {"All time", "Last 7 days", "Last 30 days"};

ftxui::Element RenderDateRangeSelector(const StatsOptions& options) {
    using namespace ftxui;
    
    std::vector<Element> elements;
    for (size_t i = 0; i < date_range_labels.size(); ++i) {
        if (i > 0) {
            elements.push_back(text(" · ") | dim);
        }
        if (static_cast<int>(i) == options.selected_date_range) {
            elements.push_back(text(date_range_labels[i]) | bold | color(Color::CyanLight));
        } else {
            elements.push_back(text(date_range_labels[i]) | dim);
        }
    }
    
    return hbox(elements);
}

ftxui::Element RenderActivityHeatmap(const std::vector<DailyActivity>& activity) {
    using namespace ftxui;
    
    if (activity.empty()) {
        return emptyElement();
    }
    
    std::vector<Element> rows;
    for (const auto& day : activity) {
        int intensity = std::min(day.session_count, 5);
        Color heat_color;
        switch (intensity) {
            case 0: heat_color = Color::RGB(40, 40, 40); break;
            case 1: heat_color = Color::RGB(60, 100, 60); break;
            case 2: heat_color = Color::RGB(80, 140, 80); break;
            case 3: heat_color = Color::RGB(100, 180, 100); break;
            case 4: heat_color = Color::RGB(120, 220, 120); break;
            default: heat_color = Color::RGB(140, 255, 140); break;
        }
        rows.push_back(text("■") | color(heat_color));
    }
    
    return hbox({
        text("Activity: ") | dim,
        hbox(rows)
    });
}

} // namespace detail

ftxui::Element Stats(const StatsOptions& options) {
    using namespace ftxui;
    
    if (options.active_tab == "Overview") {
        std::vector<Element> elements;
        
        elements.push_back(text("📊 Stats Overview") | bold | color(Color::CyanLight));
        elements.push_back(separator());
        
        // Activity heatmap
        if (!options.data.daily_activity.empty()) {
            elements.push_back(detail::RenderActivityHeatmap(options.data.daily_activity));
            elements.push_back(separator());
        }
        
        // Date range
        elements.push_back(detail::RenderDateRangeSelector(options));
        elements.push_back(text(""));
        
        // Two column layout
        elements.push_back(hbox({
            vbox({
                hbox({
                    text("Favorite model: ") | dim,
                    text(options.data.favorite_model) | color(Color::Magenta) | bold
                }),
                hbox({
                    text("Total tokens: ") | dim,
                    text(std::format("{:L}", options.data.total_tokens)) | color(Color::CyanLight)
                }),
                hbox({
                    text("Total sessions: ") | dim,
                    text(std::to_string(options.data.total_sessions)) | color(Color::CyanLight)
                }),
                hbox({
                    text("Longest session: ") | dim,
                    text(options.data.longest_session_duration) | color(Color::CyanLight)
                })
            }) | flex,
            text("  "),
            vbox({
                hbox({
                    text("Active days: ") | dim,
                    text(std::format("{}", options.data.active_days)) | color(Color::CyanLight)
                }),
                hbox({
                    text("Longest streak: ") | dim,
                    text(std::format("{} days", options.data.longest_streak)) | color(Color::CyanLight) | bold
                }),
                hbox({
                    text("Current streak: ") | dim,
                    text(std::format("{} days", options.data.current_streak)) | color(Color::Green) | bold
                }),
                hbox({
                    text("Peak day: ") | dim,
                    text(options.data.peak_activity_day) | color(Color::CyanLight)
                })
            }) | flex
        }));
        
        // Fun fact
        if (!options.data.fun_fact.empty()) {
            elements.push_back(text(""));
            elements.push_back(text("💡 " + options.data.fun_fact) | color(Color::Cyan));
        }
        
        elements.push_back(text(""));
        elements.push_back(text("Press 'r' to cycle date ranges · Tab to switch tabs · Esc to close") | dim);
        
        return vbox(elements) | borderRounded;
    } else { // Models tab
        std::vector<Element> elements;
        
        elements.push_back(text("🤖 Model Usage") | bold | color(Color::Magenta));
        elements.push_back(separator());
        
        // Date range
        elements.push_back(detail::RenderDateRangeSelector(options));
        elements.push_back(text(""));
        
        if (options.data.model_usage.empty()) {
            elements.push_back(text("No model usage data available yet") | dim);
        } else {
            for (const auto& usage : options.data.model_usage) {
                int total_tokens = usage.input_tokens + usage.output_tokens;
                elements.push_back(vbox({
                    hbox({
                        text("● ") | color(Color::Magenta),
                        text(usage.model_name) | bold | color(Color::White),
                        text("  ") | flex,
                        text(std::format("{:L} tokens", total_tokens)) | color(Color::GrayLight)
                    }),
                    hbox({
                        text("  Input: ") | dim,
                        text(std::format("{:L}", usage.input_tokens)) | color(Color::Cyan),
                        text("  Output: ") | dim,
                        text(std::format("{:L}", usage.output_tokens)) | color(Color::Green)
                    })
                }));
                elements.push_back(text(""));
            }
        }
        
        elements.push_back(text(""));
        elements.push_back(text("Press 'r' to cycle date ranges · Tab to switch tabs · Esc to close") | dim);
        
        return vbox(elements) | borderRounded;
    }
}

} // namespace ui::components
