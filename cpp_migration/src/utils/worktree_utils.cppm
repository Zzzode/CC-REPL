module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <filesystem>
#include <cstdio>
#include <array>
#include <sstream>

export module cc.utils.worktree_utils;

export namespace cc::utils {

namespace fs = std::filesystem;

// Worktree information (re-declared to avoid module dependency)
struct WorktreeInfo {
    fs::path directory;
    std::string branch;
    std::string commit;
    bool is_main = false;
};

// Create a new worktree for the given branch
inline std::expected<WorktreeInfo, std::string>
create_worktree(const fs::path& repo_root, std::string_view branch,
                std::optional<fs::path> directory = std::nullopt) {
    // Determine target directory
    fs::path target_dir;
    if (directory) {
        target_dir = *directory;
    } else {
        // Default: create sibling directory named after the branch
        std::string safe_branch(branch);
        // Replace slashes with dashes for directory name
        for (auto& c : safe_branch) {
            if (c == '/') c = '-';
        }
        target_dir = repo_root.parent_path() / (repo_root.filename().string() + "-" + safe_branch);
    }

    // Build the git worktree add command
    std::string cmd = "git -C " + repo_root.string() + " worktree add "
                    + target_dir.string() + " " + std::string(branch) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to execute git worktree add");
    }

    std::string output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        // Trim output for error message
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return std::unexpected("git worktree add failed: " + output);
    }

    // Get the commit hash for the branch
    std::string hash_cmd = "git -C " + target_dir.string() + " rev-parse HEAD 2>/dev/null";
    FILE* hash_pipe = popen(hash_cmd.c_str(), "r");
    std::string commit;
    if (hash_pipe) {
        if (fgets(buffer.data(), buffer.size(), hash_pipe) != nullptr) {
            commit = buffer.data();
            while (!commit.empty() && commit.back() == '\n') commit.pop_back();
        }
        pclose(hash_pipe);
    }

    return WorktreeInfo{
        .directory = target_dir,
        .branch = std::string(branch),
        .commit = commit,
        .is_main = false
    };
}

// Remove a worktree directory
inline std::expected<void, std::string> remove_worktree(const fs::path& worktree_dir) {
    if (!fs::exists(worktree_dir)) {
        return std::unexpected("Worktree directory does not exist: " + worktree_dir.string());
    }

    // Find the repo root from the worktree
    std::string root_cmd = "git -C " + worktree_dir.string() +
                           " rev-parse --git-common-dir 2>/dev/null";
    FILE* pipe = popen(root_cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to determine repository root");
    }

    std::array<char, 1024> buffer{};
    std::string git_common_dir;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        git_common_dir = buffer.data();
    }
    pclose(pipe);
    while (!git_common_dir.empty() && git_common_dir.back() == '\n')
        git_common_dir.pop_back();

    // Use git worktree remove
    std::string cmd = "git worktree remove " + worktree_dir.string() + " --force 2>&1";
    pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to execute git worktree remove");
    }

    std::string output;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return std::unexpected("git worktree remove failed: " + output);
    }

    return {};
}

// Prune stale worktree references
inline size_t prune_worktrees(const fs::path& repo_root) {
    // Get list of worktrees before pruning
    std::string list_cmd = "git -C " + repo_root.string() + " worktree list --porcelain 2>/dev/null";
    FILE* pipe = popen(list_cmd.c_str(), "r");
    size_t before_count = 0;
    if (pipe) {
        std::array<char, 1024> buffer{};
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            std::string line(buffer.data());
            if (line.starts_with("worktree ")) before_count++;
        }
        pclose(pipe);
    }

    // Run prune
    std::string prune_cmd = "git -C " + repo_root.string() + " worktree prune 2>/dev/null";
    system(prune_cmd.c_str());

    // Get count after pruning
    pipe = popen(list_cmd.c_str(), "r");
    size_t after_count = 0;
    if (pipe) {
        std::array<char, 1024> buffer{};
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            std::string line(buffer.data());
            if (line.starts_with("worktree ")) after_count++;
        }
        pclose(pipe);
    }

    return (before_count > after_count) ? (before_count - after_count) : 0;
}

} // namespace cc::utils
