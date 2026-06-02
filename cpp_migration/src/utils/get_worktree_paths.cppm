module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdio>
#include <array>
#include <sstream>

export module cc.utils.get_worktree_paths;

export namespace cc::utils {

namespace fs = std::filesystem;

// Information about a git worktree
struct Worktree {
    fs::path directory;
    std::string branch;
    std::string commit;
    bool is_main = false;
};

// List all worktrees for a repository
inline std::vector<Worktree> list_worktrees(const fs::path& repo_root) {
    std::vector<Worktree> worktrees;

    std::string cmd = "git -C " + repo_root.string() + " worktree list --porcelain 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return worktrees;

    std::array<char, 1024> buffer{};
    std::string output;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);

    // Parse porcelain format:
    // worktree /path/to/dir
    // HEAD <commit>
    // branch refs/heads/branch-name
    // (blank line separates entries)
    std::istringstream stream(output);
    std::string line;
    Worktree current;
    bool has_entry = false;

    while (std::getline(stream, line)) {
        if (line.starts_with("worktree ")) {
            if (has_entry) {
                worktrees.push_back(std::move(current));
                current = Worktree{};
            }
            current.directory = line.substr(9);
            has_entry = true;
            // First worktree listed is the main one
            if (worktrees.empty()) current.is_main = true;
        } else if (line.starts_with("HEAD ")) {
            current.commit = line.substr(5);
        } else if (line.starts_with("branch ")) {
            std::string ref = line.substr(7);
            // Strip refs/heads/ prefix
            const std::string prefix = "refs/heads/";
            if (ref.starts_with(prefix)) {
                current.branch = ref.substr(prefix.size());
            } else {
                current.branch = ref;
            }
        } else if (line == "bare") {
            current.is_main = true;
        } else if (line == "detached") {
            current.branch = "(detached)";
        }
    }

    if (has_entry) {
        worktrees.push_back(std::move(current));
    }

    return worktrees;
}

// Get the worktree for the current working directory
inline std::optional<Worktree> get_current_worktree() {
    std::string cmd = "git rev-parse --show-toplevel 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::array<char, 1024> buffer{};
    std::string toplevel;
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        toplevel = buffer.data();
    }
    pclose(pipe);

    while (!toplevel.empty() && (toplevel.back() == '\n' || toplevel.back() == '\r'))
        toplevel.pop_back();
    if (toplevel.empty()) return std::nullopt;

    fs::path toplevel_path(toplevel);
    auto worktrees = list_worktrees(toplevel_path);

    for (const auto& wt : worktrees) {
        if (fs::equivalent(wt.directory, toplevel_path)) {
            return wt;
        }
    }

    // Current directory might be in a linked worktree
    fs::path cwd = fs::current_path();
    for (const auto& wt : worktrees) {
        if (cwd.string().starts_with(wt.directory.string())) {
            return wt;
        }
    }

    return std::nullopt;
}

// Resolve the worktree path for a given branch name
inline std::optional<fs::path> resolve_worktree_path(const fs::path& repo_root,
                                                      std::string_view branch) {
    auto worktrees = list_worktrees(repo_root);
    for (const auto& wt : worktrees) {
        if (wt.branch == branch) {
            return wt.directory;
        }
    }
    return std::nullopt;
}

} // namespace cc::utils
