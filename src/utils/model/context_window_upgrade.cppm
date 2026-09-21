module;
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.context_window_upgrade;

export namespace cc::utils {

// Suggest upgrade when usage exceeds 80% of context window
bool should_suggest_upgrade(std::size_t current_usage, std::size_t max_context) {
    if (max_context == 0) return false;
    double ratio = static_cast<double>(current_usage) / static_cast<double>(max_context);
    return ratio >= 0.8;
}

std::string get_upgrade_message() {
    return "You're approaching your context window limit. "
           "Upgrade to Pro for 1M token context, or use /compact to reduce context usage.";
}

std::vector<std::string> get_available_upgrades(std::string_view current_model) {
    std::vector<std::string> upgrades;

    if (current_model.find("haiku") != std::string_view::npos) {
        upgrades.push_back("claude-sonnet-4-20250514");
        upgrades.push_back("claude-opus-4-20250514");
    } else if (current_model.find("sonnet") != std::string_view::npos) {
        upgrades.push_back("claude-opus-4-20250514");
    }
    // opus has no further upgrade path

    return upgrades;
}

} // namespace cc::utils
