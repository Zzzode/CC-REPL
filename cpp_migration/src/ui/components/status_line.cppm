module;

#include <string>
#include <ftxui/dom/elements.hpp>

export module ui.components.status_line;

export namespace ui::components {

enum class ConnectionStatus {
    Connected,
    Disconnected,
    Reconnecting
};

struct StatusLineOptions {
    std::string left_text;
    std::string center_text;
    std::string right_text;
    std::string model_name;
    std::string current_path;
    ConnectionStatus connection_status = ConnectionStatus::Connected;
    bool show_model_info = true;
};

ftxui::Element StatusLine(const StatusLineOptions& options) {
    using namespace ftxui;
    
    // Connection indicator
    Element conn_indicator;
    std::string status_text;
    switch (options.connection_status) {
        case ConnectionStatus::Connected:
            conn_indicator = text("●") | color(Color::Green);
            status_text = "Connected";
            break;
        case ConnectionStatus::Reconnecting:
            conn_indicator = text("●") | color(Color::Yellow);
            status_text = "Reconnecting...";
            break;
        case ConnectionStatus::Disconnected:
            conn_indicator = text("●") | color(Color::Red);
            status_text = "Disconnected";
            break;
    }
    
    std::vector<Element> elements;
    elements.push_back(conn_indicator);
    elements.push_back(text(" " + status_text) | dim);
    
    if (!options.left_text.empty()) {
        elements.push_back(text(" | " + options.left_text) | dim);
    }
    
    if (options.show_model_info && !options.model_name.empty()) {
        elements.push_back(text(" | Model: ") | dim);
        elements.push_back(text(options.model_name) | color(Color::Magenta));
    }
    
    if (!options.current_path.empty()) {
        elements.push_back(text(" | ") | dim);
        elements.push_back(text(options.current_path) | color(Color::Cyan));
    }
    
    // Center text
    if (!options.center_text.empty()) {
        elements.push_back(filler());
        elements.push_back(text(options.center_text) | color(Color::CyanLight));
    }
    
    // Right text
    if (!options.right_text.empty()) {
        elements.push_back(filler());
        elements.push_back(text(options.right_text) | dim);
    }
    
    // Build status line
    return hbox(elements) | bgcolor(Color::RGB(30, 30, 30)) | borderLight;
}

} // namespace ui::components
