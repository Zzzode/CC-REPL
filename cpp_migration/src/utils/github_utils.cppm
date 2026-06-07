module;

#include <chrono>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <httplib.h>

export module cc.utils.github_utils;

import cc.utils.json;


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

[[nodiscard]] inline std::string api_base_url() {
    if (auto* value = std::getenv("CC_REPL_GITHUB_API_BASE_URL"); value && value[0]) return value;
    if (auto* value = std::getenv("GITHUB_API_URL"); value && value[0]) return value;
    return "https://api.github.com";
}

struct ApiResponse {
    int status{0};
    std::string body;
    std::string link_header;

    [[nodiscard]] bool is_ok() const {
        return status >= 200 && status < 300;
    }
};

[[nodiscard]] inline std::optional<std::string> next_path_from_link_header(std::string_view link_header) {
    std::size_t start = 0;
    while (start < link_header.size()) {
        auto end = link_header.find(',', start);
        if (end == std::string_view::npos) end = link_header.size();
        auto part = link_header.substr(start, end - start);
        if (part.find("rel=\"next\"") != std::string_view::npos || part.find("rel=next") != std::string_view::npos) {
            auto left = part.find('<');
            auto right = part.find('>', left == std::string_view::npos ? 0 : left + 1);
            if (left != std::string_view::npos && right != std::string_view::npos && right > left + 1) {
                std::string url(part.substr(left + 1, right - left - 1));
                if (url.starts_with('/')) return url;

                auto base = api_base_url();
                while (!base.empty() && base.back() == '/') base.pop_back();
                if (url.starts_with(base)) {
                    auto path = url.substr(base.size());
                    return path.empty() ? std::string("/") : path;
                }

                if (auto scheme = url.find("://"); scheme != std::string::npos) {
                    auto path_start = url.find('/', scheme + 3);
                    if (path_start != std::string::npos) return url.substr(path_start);
                }
            }
        }
        start = end + 1;
    }
    return std::nullopt;
}

[[nodiscard]] inline httplib::Headers auth_headers(std::string_view token) {
    return {
        {"Authorization", "Bearer " + std::string(token)},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
    };
}

[[nodiscard]] inline std::expected<ApiResponse, std::string> get_api(
    std::string_view path,
    std::string_view token
) {
    auto base = api_base_url();
    while (!base.empty() && base.back() == '/') base.pop_back();
    httplib::Client client(base);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    client.set_follow_location(true);
    auto response = client.Get(std::string(path), auth_headers(token));
    if (!response) return std::unexpected(std::string(httplib::to_string(response.error())));
    return ApiResponse{
        .status = response->status,
        .body = response->body,
        .link_header = response->get_header_value("Link"),
    };
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

namespace github_detail {
[[nodiscard]] inline GitHubAuthStatus auth_status_for_response(int status) {
    if (status == 401) return GitHubAuthStatus::token_expired;
    if (status == 403 || status == 429) return GitHubAuthStatus::rate_limited;
    return GitHubAuthStatus::authenticated;
}

    [[nodiscard]] inline std::optional<PrComment> parse_issue_comment(json::JsonVal item) {
    if (!item.is_obj()) return std::nullopt;
    auto id = item.get("id");
    auto body = item.get("body");
    if (!id.is_num() || !body.is_str()) return std::nullopt;
    auto user = item.get("user");
    std::string author = "unknown";
    if (user.is_obj() && user.get("login").is_str()) author = std::string(user.get("login").as_str());
    return PrComment{
        .id = static_cast<int>(id.as_int()),
        .author = std::move(author),
        .body = std::string(body.as_str()),
        .path = {},
        .line = 0,
        .resolved = false,
        .created_at = std::chrono::system_clock::now(),
    };
}

[[nodiscard]] inline std::optional<PrComment> parse_review_comment(json::JsonVal item) {
    auto parsed = parse_issue_comment(item);
    if (!parsed) return std::nullopt;
    auto path = item.get("path");
    if (path.is_str()) parsed->path = std::string(path.as_str());
    auto line = item.get("line");
    if (line.is_num()) parsed->line = static_cast<int>(line.as_int());
    return parsed;
}
} // namespace github_detail


class GitHubUtils {
    std::string token_;
    GitHubAuthStatus auth_status_{GitHubAuthStatus::not_configured};

    [[nodiscard]] auto ensure_auth() -> std::expected<void, std::string> {
        if (auth_status_ != GitHubAuthStatus::authenticated) {
            (void)check_auth();
        }
        if (auth_status_ != GitHubAuthStatus::authenticated || token_.empty()) {
            return std::unexpected("GitHub token not configured");
        }
        return {};
    }

    [[nodiscard]] auto get_api(std::string_view path) -> std::expected<github_detail::ApiResponse, std::string> {
        if (auto auth = ensure_auth(); !auth) return std::unexpected(auth.error());
        auto response = github_detail::get_api(path, token_);
        if (!response) return std::unexpected(response.error());
        if (!response->is_ok()) {
            auth_status_ = github_detail::auth_status_for_response(response->status);
            return std::unexpected(std::format("GitHub API request failed with status {}", response->status));
        }
        return *response;
    }

    [[nodiscard]] auto get_api_pages(std::string path, std::size_t max_pages = 100)
        -> std::vector<github_detail::ApiResponse> {
        std::vector<github_detail::ApiResponse> pages;
        for (std::size_t page = 0; page < max_pages && !path.empty(); ++page) {
            auto response = get_api(path);
            if (!response) break;
            auto next = github_detail::next_path_from_link_header(response->link_header);
            pages.push_back(std::move(*response));
            path = next.value_or(std::string{});
        }
        return pages;
    }

public:

    [[nodiscard]] auto check_auth() -> GitHubAuthStatus {

        if (auto* t = std::getenv("GH_TOKEN"); t && t[0]) { token_ = t; auth_status_ = GitHubAuthStatus::authenticated; }
        else if (auto* t2 = std::getenv("GITHUB_TOKEN"); t2 && t2[0]) { token_ = t2; auth_status_ = GitHubAuthStatus::authenticated; }
        else { auth_status_ = GitHubAuthStatus::not_configured; }
        return auth_status_;
    }
    

    [[nodiscard]] auto get_current_user() -> std::expected<GitHubUser, std::string> {
        auto response = get_api("/user");
        if (!response) return std::unexpected(response.error());
        auto parsed = json::parse(response->body);
        if (!parsed || !parsed->root().is_obj()) return std::unexpected("GitHub API returned invalid user JSON");
        auto root = parsed->root();
        auto login = root.get("login");
        if (!login.is_str() || login.as_str().empty()) {
            return std::unexpected("GitHub API user response is missing login");
        }
        GitHubUser user;
        user.login = std::string(login.as_str());
        auto name = root.get("name");
        user.name = name.is_str() && !name.as_str().empty() ? std::string(name.as_str()) : user.login;
        auto email = root.get("email");
        user.email = email.is_str() ? std::string(email.as_str()) : std::string{};
        auto avatar = root.get("avatar_url");
        user.avatar_url = avatar.is_str() ? std::string(avatar.as_str()) : std::string{};
        return user;
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
        auto repo = detect_repo();
        if (!repo) return {};

        std::vector<PrComment> comments;
        auto append_comments = [&](std::string_view path, bool review_comments) {
            for (const auto& response : get_api_pages(std::string(path))) {
                auto parsed = json::parse(response.body);
                if (!parsed || !parsed->root().is_arr()) continue;
                parsed->root().iter([&](json::JsonVal item) {
                    auto comment = review_comments
                        ? github_detail::parse_review_comment(item)
                        : github_detail::parse_issue_comment(item);
                    if (comment) comments.push_back(std::move(*comment));
                });
            }
        };

        append_comments(
            std::format("/repos/{}/issues/{}/comments", repo->full_name, pr_number),
            false);
        append_comments(
            std::format("/repos/{}/pulls/{}/comments", repo->full_name, pr_number),
            true);
        return comments;
    }
    
    [[nodiscard]] auto is_authenticated() const -> bool {
        return auth_status_ == GitHubAuthStatus::authenticated;
    }

    [[nodiscard]] auto auth_status() const -> GitHubAuthStatus {
        return auth_status_;
    }
};

} // namespace cc::utils
