module;

#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>

export module ui.components.pr_badge;

export namespace ui::components {

enum class PrReviewState {
    Approved,
    ChangesRequested,
    Pending,
    Merged
};

struct PrBadgeOptions {
    int number = 0;
    std::string url;
    std::optional<PrReviewState> review_state = std::nullopt;
    bool bold = false;
};

namespace detail {

std::optional<ftxui::Color> GetPrStatusColor(std::optional<PrReviewState> state) {
    using namespace ftxui;
    
    if (!state) {
        return std::nullopt;
    }
    
    switch (*state) {
        case PrReviewState::Approved:
            return Color::Green;
        case PrReviewState::ChangesRequested:
            return Color::Red;
        case PrReviewState::Pending:
            return Color::Yellow;
        case PrReviewState::Merged:
            return Color::Purple;
        default:
            return std::nullopt;
    }
}

} // namespace detail

ftxui::Element PrBadge(const PrBadgeOptions& options) {
    using namespace ftxui;
    
    auto status_color = detail::GetPrStatusColor(options.review_state);
    bool should_dim = !status_color && !options.bold;
    
    auto label = text("#" + std::to_string(options.number));
    if (status_color) {
        label = label | color(*status_color);
    }
    if (should_dim) {
        label = label | dim;
    }
    if (options.bold) {
        label = label | bold;
    }
    
    // Note: FTXUI doesn't have a Link component, so we'll just display the badge
    auto pr_label = text("PR");
    if (!options.bold) {
        pr_label = pr_label | dim;
    }

    auto badge = hbox({
        pr_label,
        text(" "),
        label
    });
    
    return badge;
}

} // namespace ui::components
