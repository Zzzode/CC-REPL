module;

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <array>

export module cc.utils.git_filesystem;

export namespace cc::utils {

namespace fs = std::filesystem;

// Find the git repository root by traversing upward from start directory
inline std::optional<fs::path> find_git_root(fs::path start = fs::current_path()) {
    fs::path current = fs::absolute(start);
    while (true) {
        if (fs::exists(current / ".git")) {
            return current;
        }
        auto parent = current.parent_path();
        if (parent == current) break; // Reached filesystem root
        current = parent;
    }
    return std::nullopt;
}

// Get the .git directory for a repository root
inline fs::path get_git_dir(const fs::path& repo_root) {
    auto git_path = repo_root / ".git";

    // Handle gitdir: references (worktrees, submodules)
    if (fs::is_regular_file(git_path)) {
        std::ifstream file(git_path);
        std::string line;
        if (std::getline(file, line) && line.starts_with("gitdir: ")) {
            fs::path gitdir(line.substr(8));
            if (gitdir.is_relative()) {
                return fs::canonical(repo_root / gitdir);
            }
            return gitdir;
        }
    }

    return git_path;
}

// Get the current HEAD reference (e.g., "refs/heads/main")
inline std::optional<std::string> get_head_ref() {
    auto root = find_git_root();
    if (!root) return std::nullopt;

    auto head_file = get_git_dir(*root) / "HEAD";
    std::ifstream file(head_file);
    std::string line;
    if (!std::getline(file, line)) return std::nullopt;

    // "ref: refs/heads/branch" format
    if (line.starts_with("ref: ")) {
        return line.substr(5);
    }

    // Detached HEAD — return the commit hash directly
    return line;
}

// Get the current branch name (short form)
inline std::optional<std::string> get_current_branch() {
    auto ref = get_head_ref();
    if (!ref) return std::nullopt;

    const std::string prefix = "refs/heads/";
    if (ref->starts_with(prefix)) {
        return ref->substr(prefix.size());
    }

    // Detached HEAD state
    return std::nullopt;
}

// Get the commit hash for a given ref (default HEAD)
inline std::optional<std::string> get_commit_hash(std::string_view ref = "HEAD") {
    std::string cmd = "git rev-parse " + std::string(ref) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::array<char, 64> buffer{};
    std::string hash;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        hash = buffer.data();
    }
    int status = pclose(pipe);
    if (status != 0 || hash.empty()) return std::nullopt;

    // Trim newline
    while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) {
        hash.pop_back();
    }
    return hash.empty() ? std::nullopt : std::optional(std::move(hash));
}

// Check if the current directory is inside a git work tree
inline bool is_inside_work_tree() {
    std::string cmd = "git rev-parse --is-inside-work-tree 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    std::array<char, 16> buffer{};
    std::string result;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
    }
    pclose(pipe);
    return result.starts_with("true");
}

// Check if the repository is a bare repository
inline bool is_bare_repo() {
    std::string cmd = "git rev-parse --is-bare-repository 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    std::array<char, 16> buffer{};
    std::string result;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
    }
    pclose(pipe);
    return result.starts_with("true");
}

} // namespace cc::utils
