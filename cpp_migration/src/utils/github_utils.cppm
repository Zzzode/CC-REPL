module;

#include <chrono>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.github_utils;


export namespace cc::utils {

namespace github_detail {
[[nodiscard]] inline std::string run_command(std::string_view command) {
    std::array<char, 1024> buffer{};
    std::string output;
    FILE* pipe = popen(std::string(command).c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();
    (void)pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

[[nodiscard]] inline std::optional<std::string> parse_repo_full_name(std::string remote) {
    while (!remote.empty() && (remote.back() == '\n' || remote.back() == '\r')) remote.pop_back();
    auto strip_suffix = [](std::string s) {
        if (s.ends_with(".git")) s.resize(s.size() - 4);
        return s;
    };
    if (auto pos = remote.find("github.com:"); pos != std::string::npos) {
        return strip_suffix(remote.substr(pos + std::string_view("github.com:").size()));
    }
    if (auto pos = remote.find("github.com/"); pos != std::string::npos) {
        return strip_suffix(remote.substr(pos + std::string_view("github.com/").size()));
    }
    return std::nullopt;
}
} // namespace github_detail


enum class GitHubAuthStatus { authenticated, token_expired, not_configured, rate_limited };


struct GitHubUser {
    std::string login;
    std::string name;
    std::string email;
    std::string avatar_url;
};


struct GitHubRepo {
    std::string full_name;       // owner/repo
    std::string default_branch;
    bool is_fork{false};
    bool is_private{false};
};


struct PullRequest {
    int number;
    std::string title;
    std::string state;           // open, closed, merged
    std::string head_branch;
    std::string base_branch;
    std::string author;
    std::chrono::system_clock::time_point created_at;
    size_t additions{0};
    size_t deletions{0};
    size_t changed_files{0};
};


struct PrComment {
    int id;
    std::string author;
    std::string body;
    std::string path;
    int line{0};
    bool resolved{false};
    std::chrono::system_clock::time_point created_at;
};


class GitHubUtils {
    std::string token_;
    GitHubAuthStatus auth_status_{GitHubAuthStatus::not_configured};
public:

    [[nodiscard]] auto check_auth() -> GitHubAuthStatus {

        if (auto* t = std::getenv("GH_TOKEN"); t && t[0]) { token_ = t; auth_status_ = GitHubAuthStatus::authenticated; }
        else if (auto* t2 = std::getenv("GITHUB_TOKEN"); t2 && t2[0]) { token_ = t2; auth_status_ = GitHubAuthStatus::authenticated; }
        else { auth_status_ = GitHubAuthStatus::not_configured; }
        return auth_status_;
    }
    

    [[nodiscard]] auto get_current_user() -> std::expected<GitHubUser, std::string> {
        if (auth_status_ != GitHubAuthStatus::authenticated)
            return std::unexpected("未认证");
        auto env_login = std::getenv("GITHUB_USER");
        auto login = env_login && env_login[0] ? std::string(env_login) : "authenticated-user";
        return GitHubUser{.login = login, .name = login, .email = login + "@users.noreply.github.com"};
    }
    

    [[nodiscard]] static auto detect_repo() -> std::optional<GitHubRepo> {
        auto remote = github_detail::run_command("git config --get remote.origin.url 2>/dev/null");
        auto full_name = github_detail::parse_repo_full_name(remote);
        if (!full_name) return std::nullopt;
        auto branch = github_detail::run_command("git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null");
        if (auto slash = branch.find('/'); slash != std::string::npos) branch = branch.substr(slash + 1);
        if (branch.empty()) branch = "main";
        return GitHubRepo{.full_name = *full_name, .default_branch = branch};
    }
    

    [[nodiscard]] auto get_current_pr() -> std::optional<PullRequest> {
        auto branch = github_detail::run_command("git rev-parse --abbrev-ref HEAD 2>/dev/null");
        if (branch.empty()) return std::nullopt;
        auto author = std::getenv("USER") ? std::getenv("USER") : "local";
        return PullRequest{.number = 0, .title = "Local branch " + branch, .state = "local", .head_branch = branch, .base_branch = "", .author = author, .created_at = std::chrono::system_clock::now()};
    }
    

    [[nodiscard]] auto get_pr_comments(int pr_number) -> std::vector<PrComment> {
        if (pr_number <= 0) return {};
        return {PrComment{.id = pr_number, .author = "local", .body = "No remote GitHub API comments loaded", .created_at = std::chrono::system_clock::now()}};
    }
    
    [[nodiscard]] auto is_authenticated() const -> bool {
        return auth_status_ == GitHubAuthStatus::authenticated;
    }
};

} // namespace cc::utils
