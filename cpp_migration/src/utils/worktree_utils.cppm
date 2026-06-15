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
import cc.utils.bash_execution;

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

    auto pipe_cap = cc::utils::bash::exec_capture(cmd.c_str());
    if (!pipe_cap) {
        return std::unexpected("Failed to execute git worktree add");
    }
    std::array<char, 512> buffer{};
    std::string output = std::move(pipe_cap->output);
    int status = pipe_cap->status;
    if (status != 0) {
        // Trim output for error message
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return std::unexpected("git worktree add failed: " + output);
    }

    // Get the commit hash for the branch
    std::string hash_cmd = "git -C " + target_dir.string() + " rev-parse HEAD 2>/dev/null";
    auto hash_cap = cc::utils::bash::exec_capture(hash_cmd);
    std::string commit;
    if (hash_cap && !hash_cap->output.empty()) {
        commit = std::move(hash_cap->output);
        while (!commit.empty() && commit.back() == '\n') commit.pop_back();
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
    auto root_cap = cc::utils::bash::exec_capture(root_cmd);
    if (!root_cap) {
        return std::unexpected("Failed to determine repository root");
    }

    std::array<char, 1024> buffer{};
    std::string git_common_dir = std::move(root_cap->output);
    while (!git_common_dir.empty() && git_common_dir.back() == '\n')
        git_common_dir.pop_back();

    // Use git worktree remove
    std::string cmd = "git worktree remove " + worktree_dir.string() + " --force 2>&1";
    auto remove_cap = cc::utils::bash::exec_capture(cmd);
    if (!remove_cap) {
        return std::unexpected("Failed to execute git worktree remove");
    }

    std::string output = std::move(remove_cap->output);
    int status = remove_cap->status;
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
    auto before_cap = cc::utils::bash::exec_capture(list_cmd);
    size_t before_count = 0;
    if (before_cap) {
        std::istringstream iss(before_cap->output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.starts_with("worktree ")) before_count++;
        }
    }

    // Run prune
    std::string prune_cmd = "git -C " + repo_root.string() + " worktree prune 2>/dev/null";
    system(prune_cmd.c_str());

    // Get count after pruning
    auto after_cap = cc::utils::bash::exec_capture(list_cmd);
    size_t after_count = 0;
    if (after_cap) {
        std::istringstream iss(after_cap->output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.starts_with("worktree ")) after_count++;
        }
    }

    return (before_count > after_count) ? (before_count - after_count) : 0;
}

} // namespace cc::utils
