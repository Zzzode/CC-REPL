/// @file auth_flows.cppm
/// @brief Authentication flow UI components (login, OAuth)
module;
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.components.auth_flows;
export namespace cc::ui::components {
using namespace ftxui;
enum class AuthStep { Initial, BrowserOpened, WaitingForCode, Verifying, Complete, Failed };
struct AuthFlowState { AuthStep step{AuthStep::Initial}; std::optional<std::string> url; std::optional<std::string> error; };
[[nodiscard]] inline Element render_auth_flow(const AuthFlowState& state) {
    auto step_text = [&]() -> std::string {
        switch (state.step) {
            case AuthStep::Initial: return "Starting authentication...";
            case AuthStep::BrowserOpened: return "Browser opened - please log in";
            case AuthStep::WaitingForCode: return "Waiting for authorization code...";
            case AuthStep::Verifying: return "Verifying...";
            case AuthStep::Complete: return "Authentication complete!";
            case AuthStep::Failed: return "Authentication failed";
        } return "";
    }();
    std::vector<Element> elements;
    elements.push_back(text(step_text) | bold);
    if (state.url) elements.push_back(text("  URL: " + *state.url) | dim);
    if (state.error) elements.push_back(text("  Error: " + *state.error) | color(Color::Red));
    return vbox(elements);
}
} // namespace
