module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <filesystem>
#include <fstream>

export module cc.commands.install_github_app.github_actions;

export namespace cc::commands {

using std::filesystem::path;


auto get_workflow_template() -> std::string {
    return R"yaml(name: Claude Code Review
on:
  pull_request:
    types: [opened, synchronize]

permissions:
  contents: read
  pull-requests: write

jobs:
  review:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Run Claude Code Review
        uses: anthropics/claude-code-action@v1
        with:
          anthropic_api_key: ${{ secrets.ANTHROPIC_API_KEY }}
)yaml";
}


auto generate_workflow_yaml(std::string_view model, bool auto_fix) -> std::string {
    std::string yaml = get_workflow_template();


    if (!model.empty()) {
        yaml += "          model: " + std::string(model) + "\n";
    }


    if (auto_fix) {
        yaml += "          auto_fix: true\n";
    }

    return yaml;
}


auto write_workflow_file(path repo_root, std::string_view content) -> std::expected<void, std::string> {
    auto workflow_dir = repo_root / ".github" / "workflows";


    std::error_code ec;
    std::filesystem::create_directories(workflow_dir, ec);
    if (ec) {
        return std::unexpected("Failed to create workflow directory: " + ec.message());
    }

    auto file_path = workflow_dir / "claude-review.yml";

    std::ofstream output{file_path, std::ios::trunc};
    if (!output) {
        return std::unexpected("Failed to open workflow file for writing: " + file_path.string());
    }
    output << content;
    if (!output) {
        return std::unexpected("Failed to write workflow file: " + file_path.string());
    }
    return {};
}


auto check_existing_workflow(path repo_root) -> std::optional<path> {
    auto workflow_path = repo_root / ".github" / "workflows" / "claude-review.yml";

    if (std::filesystem::exists(workflow_path)) {
        return workflow_path;
    }


    auto alt_path = repo_root / ".github" / "workflows" / "claude-code.yml";
    if (std::filesystem::exists(alt_path)) {
        return alt_path;
    }

    return std::nullopt;
}

} // namespace cc::commands
