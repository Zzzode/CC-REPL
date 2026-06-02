/// @file spinner_verbs.cppm
/// @brief Spinner verb list for loading messages.
/// Migrated from src/constants/spinnerVerbs.ts
module;

#include <string_view>
#include <array>
#include <vector>
#include <string>

export module cc.constants.spinner_verbs;

export namespace cc::constants::spinner_verbs {

/// Default spinner verbs (sorted alphabetically in original)
inline constexpr std::array SPINNER_VERBS = {
    std::string_view("Accomplishing"), std::string_view("Actioning"),
    std::string_view("Actualizing"), std::string_view("Architecting"),
    std::string_view("Baking"), std::string_view("Beaming"),
    std::string_view("Beboppin'"), std::string_view("Befuddling"),
    std::string_view("Billowing"), std::string_view("Blanching"),
    std::string_view("Bloviating"), std::string_view("Boogieing"),
    std::string_view("Boondoggling"), std::string_view("Booping"),
    std::string_view("Bootstrapping"), std::string_view("Brewing"),
    std::string_view("Bunning"), std::string_view("Burrowing"),
    std::string_view("Calculating"), std::string_view("Canoodling"),
    std::string_view("Caramelizing"), std::string_view("Cascading"),
    std::string_view("Catapulting"), std::string_view("Cerebrating"),
    std::string_view("Channeling"), std::string_view("Channelling"),
    std::string_view("Choreographing"), std::string_view("Churning"),
    std::string_view("Clauding"), std::string_view("Coalescing"),
    std::string_view("Cogitating"), std::string_view("Combobulating"),
    std::string_view("Composing"), std::string_view("Computing"),
    std::string_view("Concocting"), std::string_view("Considering"),
    std::string_view("Contemplating"), std::string_view("Cooking"),
    std::string_view("Crafting"), std::string_view("Creating"),
    std::string_view("Crunching"), std::string_view("Crystallizing"),
    std::string_view("Cultivating"), std::string_view("Thinking"),
    std::string_view("Working"),
};

/// Get spinner verbs (with optional user customization applied)
[[nodiscard]] inline std::vector<std::string_view> get_spinner_verbs() {
    // In production, would read from settings and merge/replace
    return std::vector<std::string_view>(SPINNER_VERBS.begin(), SPINNER_VERBS.end());
}

} // namespace cc::constants::spinner_verbs
