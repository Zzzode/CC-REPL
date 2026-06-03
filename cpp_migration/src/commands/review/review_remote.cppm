module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>

export module cc.commands.review.review_remote;

export namespace cc::commands {


struct ReviewComment {
    int line;
    std::string comment;
};

auto get_pr_diff(std::string_view pr_url) -> std::expected<std::string, std::string>;


auto review_remote_pr(std::string_view pr_url) -> std::expected<std::string, std::string> {
    if (pr_url.empty()) {
        return std::unexpected("PR URL cannot be empty");
    }
    auto diff = get_pr_diff(pr_url);
    if (!diff) return std::unexpected(diff.error());
    return "Remote PR review prepared for " + std::string(pr_url) + " (" + std::to_string(diff->size()) + " diff bytes).";
}


auto get_pr_diff(std::string_view pr_url) -> std::expected<std::string, std::string> {
    if (pr_url.empty()) {
        return std::unexpected("PR URL cannot be empty");
    }
    return "diff --git a/remote b/remote\n# PR source: " + std::string(pr_url) + "\n";
}


auto post_review_comments(std::string_view pr_url, std::vector<ReviewComment> comments)
    -> std::expected<void, std::string> {
    if (pr_url.empty()) {
        return std::unexpected("PR URL cannot be empty");
    }
    if (comments.empty()) {
        return {};
    }
    return {};
}

} // namespace cc::commands
