// C++23 Module: Layout engine with responsive adaptation and focus management
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.layout;

import cc.ui.panels;

export namespace cc::ui {

// Layout arrangement modes
enum class LayoutMode {
    SingleColumn,     // Full-width main content
    SplitHorizontal,  // Main content | side panel
    SplitVertical,    // Main content above, panel below
    Overlay           // Panel overlaid on main content
};

// Focus target for keyboard navigation between panes
enum class FocusTarget {
    PromptInput,    // The input area
    MessageList,    // Scrollable message history
    SidePanel,      // Active side panel
    BottomPanel,    // Bottom panel area
    Overlay         // Overlay panel
};

// Layout configuration for customizing sizes
struct LayoutConfig {
    LayoutMode mode{LayoutMode::SingleColumn};
    std::size_t sidebar_width{40};        // Hint for absolute sidebar width
    std::size_t panel_height{15};         // Lines tall for bottom panel
    std::size_t min_main_width{60};       // Minimum main content width
    std::size_t min_main_height{10};      // Minimum main content height
    double sidebar_ratio{0.3};            // Sidebar as a fraction of terminal width
    double panel_ratio{0.3};              // Ratio of bottom panel to total height
    std::size_t min_sidebar_width{20};    // Floor for the adaptive sidebar
    std::size_t max_sidebar_width{80};    // Ceiling for the adaptive sidebar
    bool show_status_bar{true};
    bool show_breadcrumb{true};

    // Compute a terminal-width-adaptive sidebar: target = ratio * width, then
    // clamped to [min_sidebar_width, min(max_sidebar_width, width - min_main_width)].
    // On terminals too narrow to honour min_main_width, the sidebar collapses to 0.
    [[nodiscard]] auto effective_sidebar_width(std::size_t term_width) const -> std::size_t {
        std::size_t target = static_cast<std::size_t>(static_cast<double>(term_width) * sidebar_ratio);
        std::size_t floor_by_main = term_width > min_main_width ? term_width - min_main_width : 0;
        std::size_t upper = std::min(max_sidebar_width, floor_by_main);
        std::size_t lower = min_sidebar_width > upper ? upper : min_sidebar_width;
        if (target < lower) target = lower;
        if (target > upper) target = upper;
        return target;
    }

    // Clamp panel height to reasonable bounds given terminal height
    [[nodiscard]] auto effective_panel_height(std::size_t term_height) const -> std::size_t {
        auto max_panel = term_height > min_main_height ? term_height - min_main_height : 0;
        return std::min(panel_height, max_panel);
    }
};

// Terminal dimensions snapshot
struct TerminalSize {
    std::size_t width{80};
    std::size_t height{24};

    [[nodiscard]] auto is_narrow() const -> bool { return width < 100; }
    [[nodiscard]] auto is_short() const -> bool { return height < 30; }
    [[nodiscard]] auto is_wide() const -> bool { return width >= 160; }
};

// Pane geometry (computed by layout engine)
struct PaneRect {
    std::size_t x{0};
    std::size_t y{0};
    std::size_t width{0};
    std::size_t height{0};

    [[nodiscard]] auto empty() const -> bool { return width == 0 || height == 0; }
};

// Status bar state
struct StatusBarState {
    std::string model_name;
    std::string session_id;
    std::size_t token_count{0};
    std::size_t message_count{0};
    std::optional<std::string> active_tool;
    bool is_streaming{false};

    [[nodiscard]] auto render() const -> ftxui::Element;

    // Format token count with K/M suffixes
    [[nodiscard]] auto formatted_tokens() const -> std::string {
        if (token_count < 1000) return std::format("{}", token_count);
        if (token_count < 1000000) return std::format("{:.1f}K", token_count / 1000.0);
        return std::format("{:.1f}M", token_count / 1000000.0);
    }
};

// Breadcrumb path for navigation context
struct Breadcrumb {
    std::vector<std::string> path; // e.g., ["Settings", "API Keys"]

    [[nodiscard]] auto render() const -> ftxui::Element;

    [[nodiscard]] auto formatted() const -> std::string {
        std::string result;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i > 0) result += " > ";
            result += path[i];
        }
        return result;
    }
};

// Application state needed for layout rendering
struct AppRenderState {
    ftxui::Element prompt_area;
    ftxui::Element message_area;
    std::optional<ftxui::Element> panel_content;
    StatusBarState status_bar;
    Breadcrumb breadcrumb;
    TerminalSize terminal_size;
};

// AppLayout: main layout engine managing pane arrangement
class AppLayout {
public:
    explicit AppLayout(LayoutConfig config = {}) : config_(config) {}

    // Set the layout mode
    auto set_mode(LayoutMode mode) -> void {
        config_.mode = mode;
        recompute_geometry();
    }

    // Set which panel is active in the side/bottom area
    auto set_active_panel(PanelType panel) -> void {
        active_panel_ = panel;
        if (config_.mode == LayoutMode::SingleColumn) {
            config_.mode = LayoutMode::SplitHorizontal;
        }
    }

    // Toggle sidebar visibility
    auto toggle_sidebar() -> void {
        sidebar_visible_ = !sidebar_visible_;
        if (!sidebar_visible_ && config_.mode == LayoutMode::SplitHorizontal) {
            config_.mode = LayoutMode::SingleColumn;
        } else if (sidebar_visible_ && config_.mode == LayoutMode::SingleColumn) {
            config_.mode = LayoutMode::SplitHorizontal;
        }
    }

    // Toggle bottom panel visibility
    auto toggle_panel() -> void {
        panel_visible_ = !panel_visible_;
        if (!panel_visible_ && config_.mode == LayoutMode::SplitVertical) {
            config_.mode = LayoutMode::SingleColumn;
        } else if (panel_visible_ && config_.mode == LayoutMode::SingleColumn) {
            config_.mode = LayoutMode::SplitVertical;
        }
    }

    // Set focus to a specific pane
    auto set_focus(FocusTarget target) -> void { focus_ = target; }

    // Cycle focus to next pane (Tab key)
    auto focus_next() -> void {
        switch (focus_) {
            case FocusTarget::PromptInput:
                focus_ = FocusTarget::MessageList; break;
            case FocusTarget::MessageList:
                focus_ = sidebar_visible_ ? FocusTarget::SidePanel : FocusTarget::PromptInput; break;
            case FocusTarget::SidePanel:
                focus_ = panel_visible_ ? FocusTarget::BottomPanel : FocusTarget::PromptInput; break;
            case FocusTarget::BottomPanel:
                focus_ = FocusTarget::PromptInput; break;
            case FocusTarget::Overlay:
                focus_ = FocusTarget::PromptInput; break;
        }
    }

    // Cycle focus to previous pane (Shift+Tab)
    auto focus_prev() -> void {
        switch (focus_) {
            case FocusTarget::PromptInput:
                focus_ = panel_visible_ ? FocusTarget::BottomPanel :
                         (sidebar_visible_ ? FocusTarget::SidePanel : FocusTarget::MessageList); break;
            case FocusTarget::MessageList:
                focus_ = FocusTarget::PromptInput; break;
            case FocusTarget::SidePanel:
                focus_ = FocusTarget::MessageList; break;
            case FocusTarget::BottomPanel:
                focus_ = sidebar_visible_ ? FocusTarget::SidePanel : FocusTarget::MessageList; break;
            case FocusTarget::Overlay:
                focus_ = FocusTarget::PromptInput; break;
        }
    }

    // Update terminal size and adapt layout responsively
    auto update_terminal_size(TerminalSize size) -> void {
        terminal_size_ = size;
        adapt_to_size();
        recompute_geometry();
    }

    // Main render method: compose all panes into final layout
    [[nodiscard]] auto render(const AppRenderState& state) const -> ftxui::Element;

    // Accessors
    [[nodiscard]] auto config() const -> const LayoutConfig& { return config_; }
    [[nodiscard]] auto mode() const -> LayoutMode { return config_.mode; }
    [[nodiscard]] auto focus() const -> FocusTarget { return focus_; }
    [[nodiscard]] auto sidebar_visible() const -> bool { return sidebar_visible_; }
    [[nodiscard]] auto panel_visible() const -> bool { return panel_visible_; }
    [[nodiscard]] auto main_rect() const -> PaneRect { return main_rect_; }
    [[nodiscard]] auto sidebar_rect() const -> PaneRect { return sidebar_rect_; }
    [[nodiscard]] auto panel_rect() const -> PaneRect { return panel_rect_; }

private:
    LayoutConfig config_;
    TerminalSize terminal_size_;
    FocusTarget focus_{FocusTarget::PromptInput};
    std::optional<PanelType> active_panel_;
    bool sidebar_visible_{false};
    bool panel_visible_{false};

    // Computed geometries
    PaneRect main_rect_;
    PaneRect sidebar_rect_;
    PaneRect panel_rect_;

    // Responsive adaptation: switch to single column on narrow terminals
    auto adapt_to_size() -> void {
        if (terminal_size_.is_narrow()) {
            // Force single column for narrow terminals
            if (config_.mode == LayoutMode::SplitHorizontal) {
                config_.mode = LayoutMode::Overlay;
            }
        }
        if (terminal_size_.is_short()) {
            // Reduce panel height for short terminals
            config_.panel_height = std::min(config_.panel_height, terminal_size_.height / 3);
        }
        // Wide terminals can accommodate larger sidebar
        if (terminal_size_.is_wide()) {
            config_.sidebar_width = std::max(config_.sidebar_width, std::size_t{50});
        }
    }

    // Recompute pane rectangles based on current mode and size
    auto recompute_geometry() -> void {
        auto w = terminal_size_.width;
        auto h = terminal_size_.height;
        std::size_t status_h = config_.show_status_bar ? 1 : 0;
        std::size_t bread_h = config_.show_breadcrumb ? 1 : 0;
        std::size_t available_h = h > (status_h + bread_h) ? h - status_h - bread_h : h;

        switch (config_.mode) {
            case LayoutMode::SingleColumn:
                main_rect_ = {0, bread_h, w, available_h};
                sidebar_rect_ = {};
                panel_rect_ = {};
                break;
            case LayoutMode::SplitHorizontal: {
                auto sw = config_.effective_sidebar_width(w);
                main_rect_ = {0, bread_h, w - sw, available_h};
                sidebar_rect_ = {w - sw, bread_h, sw, available_h};
                panel_rect_ = {};
                break;
            }
            case LayoutMode::SplitVertical: {
                auto ph = config_.effective_panel_height(available_h);
                main_rect_ = {0, bread_h, w, available_h - ph};
                sidebar_rect_ = {};
                panel_rect_ = {0, bread_h + available_h - ph, w, ph};
                break;
            }
            case LayoutMode::Overlay:
                main_rect_ = {0, bread_h, w, available_h};
                sidebar_rect_ = {w / 4, available_h / 4, w / 2, available_h / 2};
                panel_rect_ = {};
                break;
        }
    }
};

// Factory: create default layout for given terminal size
[[nodiscard]] auto create_default_layout(TerminalSize size) -> AppLayout {
    LayoutConfig config;
    if (size.is_narrow()) {
        config.mode = LayoutMode::SingleColumn;
    } else if (size.is_wide()) {
        config.mode = LayoutMode::SplitHorizontal;
        config.sidebar_width = 50;
    }
    AppLayout layout(config);
    layout.update_terminal_size(size);
    return layout;
}

} // namespace cc::ui
