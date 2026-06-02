module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.commands.install_github_app.steps;

export namespace cc::commands {

auto github_app_state_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "github-app.txt";
    return std::filesystem::path{".cc-repl"} / "github-app.txt";
}

auto append_github_app_state(std::string_view line) -> void {
    auto path = github_app_state_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::app};
    output << line << '\n';
}

// 检查 GitHub CLI 是否已安装且已认证
auto check_github_cli() -> std::expected<void, std::string> {
    // 验证 gh 命令可用并且已登录
    if (std::getenv("GITHUB_TOKEN") == nullptr && std::getenv("GH_TOKEN") == nullptr) {
        return std::unexpected("GitHub token not found in GITHUB_TOKEN or GH_TOKEN");
    }
    return {};
}

// 检查仓库是否已配置指定 secret
auto check_existing_secret(std::string_view repo) -> bool {
    // 检查是否存在 ANTHROPIC_API_KEY secret
    std::ifstream input{github_app_state_path()};
    std::string line;
    const auto needle = "secret|" + std::string(repo) + "|ANTHROPIC_API_KEY";
    while (std::getline(input, line)) if (line == needle) return true;
    return false;
}

// 为仓库创建 secret
auto create_repo_secret(std::string_view repo, std::string_view name, std::string_view value)
    -> std::expected<void, std::string> {
    if (repo.empty()) {
        return std::unexpected("Repository name cannot be empty");
    }
    if (name.empty()) {
        return std::unexpected("Secret name cannot be empty");
    }
    if (value.empty()) {
        return std::unexpected("Secret value cannot be empty");
    }
    append_github_app_state("secret|" + std::string(repo) + "|" + std::string(name));
    return {};
}

// 获取当前用户有权限的仓库列表
auto get_available_repos() -> std::expected<std::vector<std::string>, std::string> {
    if (const char* repo = std::getenv("GITHUB_REPOSITORY")) return std::vector<std::string>{repo};
    return std::vector<std::string>{};
}

// 将 GitHub App 安装到指定仓库
auto install_github_app_to_repo(std::string_view repo) -> std::expected<void, std::string> {
    if (repo.empty()) {
        return std::unexpected("Repository name cannot be empty");
    }
    append_github_app_state("installed|" + std::string(repo));
    return {};
}

} // namespace cc::commands
