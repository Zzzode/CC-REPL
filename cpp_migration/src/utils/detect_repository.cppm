module;

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>
#include <cstdio>
#include <array>

export module cc.utils.detect_repository;

export namespace cc::utils {

namespace fs = std::filesystem;

// Information about a detected repository
struct RepoInfo {
    fs::path root;
    std::string type;                   // "git", "hg", "svn", etc.
    std::optional<std::string> branch;
    std::optional<std::string> remote_url;
};

// Detect the repository root starting from a directory
inline std::optional<fs::path> detect_repo_root(fs::path start = fs::current_path()) {
    fs::path current = fs::absolute(start);
    while (true) {
        // Check for various VCS directories
        if (fs::exists(current / ".git")) return current;
        if (fs::exists(current / ".hg")) return current;
        if (fs::exists(current / ".svn")) return current;

        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

// Check if a path is inside a git repository
inline bool is_git_repo(const fs::path& path) {
    fs::path current = fs::absolute(path);
    while (true) {
        if (fs::exists(current / ".git")) return true;
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return false;
}

// Get detailed repository information
inline std::optional<RepoInfo> get_repo_info(fs::path start = fs::current_path()) {
    fs::path current = fs::absolute(start);

    while (true) {
        if (fs::exists(current / ".git")) {
            RepoInfo info;
            info.root = current;
            info.type = "git";

            // Get current branch
            auto cmd_branch = "git -C " + current.string() + " branch --show-current 2>/dev/null";
            FILE* pipe = popen(cmd_branch.c_str(), "r");
            if (pipe) {
                std::array<char, 256> buffer{};
                if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                    std::string branch(buffer.data());
                    while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r'))
                        branch.pop_back();
                    if (!branch.empty()) info.branch = branch;
                }
                pclose(pipe);
            }

            // Get remote URL
            auto cmd_remote = "git -C " + current.string() + " remote get-url origin 2>/dev/null";
            pipe = popen(cmd_remote.c_str(), "r");
            if (pipe) {
                std::array<char, 512> buffer{};
                if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                    std::string url(buffer.data());
                    while (!url.empty() && (url.back() == '\n' || url.back() == '\r'))
                        url.pop_back();
                    if (!url.empty()) info.remote_url = url;
                }
                pclose(pipe);
            }

            return info;
        }

        if (fs::exists(current / ".hg")) {
            RepoInfo info;
            info.root = current;
            info.type = "hg";
            return info;
        }

        if (fs::exists(current / ".svn")) {
            RepoInfo info;
            info.root = current;
            info.type = "svn";
            return info;
        }

        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }

    return std::nullopt;
}

} // namespace cc::utils
