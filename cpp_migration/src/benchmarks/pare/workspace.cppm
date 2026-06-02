module;

#include <string>
#include <functional>
#include <filesystem>
#include <fstream>

export module cc.benchmarks.pare.workspace;

import cc.utils.process;
import cc.utils.git;

export namespace cc::benchmarks::pare {

namespace fs = std::filesystem;

enum class WorkspaceMode {
    TmpGit,
    Worktree,
    Current
};

struct BenchmarkWorkspace {
    std::string dir;
    std::function<void()> cleanup;
};

inline std::optional<BenchmarkWorkspace> create_isolated_workspace(
    WorkspaceMode mode,
    const std::string& source_cwd) {
    
    if (mode == WorkspaceMode::Current) {
        return BenchmarkWorkspace{
            .dir = source_cwd,
            .cleanup = []() {}
        };
    }
    
    fs::path temp_root = fs::temp_directory_path() / ("pare-bench-" + std::to_string(std::time(nullptr)));
    try {
        fs::create_directories(temp_root);
    } catch (...) {
        return std::nullopt;
    }
    
    fs::path repo_dir = temp_root / "repo";
    
    if (mode == WorkspaceMode::TmpGit) {
        auto result = cc::utils::git::clone_local(source_cwd, repo_dir.string());
        if (!result.success) {
            fs::remove_all(temp_root);
            return std::nullopt;
        }
        
        result = cc::utils::git::checkout(repo_dir.string(), "HEAD");
        if (!result.success) {
            fs::remove_all(temp_root);
            return std::nullopt;
        }
        
        return BenchmarkWorkspace{
            .dir = repo_dir.string(),
            .cleanup = [temp_root]() {
                try {
                    fs::remove_all(temp_root);
                } catch (...) {}
            }
        };
    }
    
    auto result = cc::utils::git::worktree_add(source_cwd, repo_dir.string(), "HEAD");
    if (!result.success) {
        fs::remove_all(temp_root);
        return std::nullopt;
    }
    
    return BenchmarkWorkspace{
        .dir = repo_dir.string(),
        .cleanup = [temp_root, source_cwd, repo_dir]() {
            try {
                cc::utils::git::worktree_remove(source_cwd, repo_dir.string());
            } catch (...) {}
            try {
                fs::remove_all(temp_root);
            } catch (...) {}
        }
    };
}

} // namespace cc::benchmarks::pare
