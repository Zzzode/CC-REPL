module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>

export module cc.commands.install_github_app.flow;

export namespace cc::commands {


enum class InstallStep {
    CheckGitHub,
    OAuth,
    InstallApp,
    ChooseRepo,
    CreateSecret,
    SetupWorkflow,
    Success
};


struct InstallState {
    InstallStep current_step;
    std::map<std::string, std::string> data;
    std::vector<std::string> errors;
};


auto advance_step(InstallState& state) -> std::expected<InstallStep, std::string> {
    switch (state.current_step) {
        case InstallStep::CheckGitHub:
            state.current_step = InstallStep::OAuth;
            return InstallStep::OAuth;
        case InstallStep::OAuth:
            state.current_step = InstallStep::InstallApp;
            return InstallStep::InstallApp;
        case InstallStep::InstallApp:
            state.current_step = InstallStep::ChooseRepo;
            return InstallStep::ChooseRepo;
        case InstallStep::ChooseRepo:
            state.current_step = InstallStep::CreateSecret;
            return InstallStep::CreateSecret;
        case InstallStep::CreateSecret:
            state.current_step = InstallStep::SetupWorkflow;
            return InstallStep::SetupWorkflow;
        case InstallStep::SetupWorkflow:
            state.current_step = InstallStep::Success;
            return InstallStep::Success;
        case InstallStep::Success:
            return std::unexpected("Already at final step");
    }
    return std::unexpected("Unknown step");
}


auto can_go_back(InstallState state) -> bool {
    return state.current_step != InstallStep::CheckGitHub;
}


auto get_step_description(InstallStep step) -> std::string {
    switch (step) {
        case InstallStep::CheckGitHub:
            return "Checking GitHub CLI availability";
        case InstallStep::OAuth:
            return "Authenticating with GitHub OAuth";
        case InstallStep::InstallApp:
            return "Installing GitHub App";
        case InstallStep::ChooseRepo:
            return "Choosing target repository";
        case InstallStep::CreateSecret:
            return "Creating repository secret";
        case InstallStep::SetupWorkflow:
            return "Setting up GitHub Actions workflow";
        case InstallStep::Success:
            return "Installation complete";
    }
    return "Unknown step";
}

} // namespace cc::commands
