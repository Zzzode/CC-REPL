// C++23 Git Utilities Module
// Provides git CLI wrapper functions for repository inspection
module;

#include <cstdlib>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <array>
#include <cstdio>

export module cc.utils.git;

import cc.utils.error;
import cc.utils.bash_execution;

export namespace cc::utils::git {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
namespace fs = std::filesystem;


struct FileStatus {
    char index_status;
    char worktree_status;
    std::string path;
    std::string orig_path;
};


struct CommitInfo {
    std::string hash;
    std::string short_hash;
    std::string author;
    std::string date;
    std::string message;
};


struct GitRepoState {
    std::string commit_hash;
    std::string branch_name;
    std::string remote_url;
    bool is_head_on_remote;
    bool is_clean;
    int worktree_count;
};

struct CommandResult {
    bool success{false};
    std::string output;
};

// =========================================================================

// =========================================================================
namespace detail {


inline Result<std::string> exec_git(
    const std::string& args, const fs::path& cwd = {}) {
    std::string cmd = "git";
    if (!cwd.empty()) {
        cmd += std::format(" -C \"{}\"", cwd.string());
    }
    cmd += " " + args + " 2>/dev/null";

    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) {
        return std::unexpected(Error(ErrorCode::io_error,
            std::format("Failed to execute: {}", cmd)));
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }

    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0) {
        return std::unexpected(Error(ErrorCode::internal_error,
            std::format("Git command failed (exit {}): {}", status, cmd)));
    }


    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }
    return output;
}


inline std::vector<std::string> split_lines(std::string_view str) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < str.size()) {
        auto end = str.find('\n', start);
        if (end == std::string_view::npos) end = str.size();
        lines.emplace_back(str.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace detail

// =========================================================================

// =========================================================================

[[nodiscard]] inline CommandResult run_git_command(const std::string& args, const fs::path& cwd = {}) {
    auto result = detail::exec_git(args, cwd);
    if (!result) {
        return CommandResult{.success = false, .output = result.error().format()};
    }
    return CommandResult{.success = true, .output = *result};
}

[[nodiscard]] inline CommandResult clone_local(const std::string& source_cwd, const std::string& target_dir) {
    return run_git_command(std::format("clone --no-hardlinks \"{}\" \"{}\"", source_cwd, target_dir));
}

[[nodiscard]] inline CommandResult checkout(const std::string& repo_dir, const std::string& ref) {
    return run_git_command(std::format("checkout \"{}\"", ref), repo_dir);
}

[[nodiscard]] inline CommandResult worktree_add(
    const std::string& source_cwd,
    const std::string& target_dir,
    const std::string& ref) {
    return run_git_command(std::format("worktree add --detach \"{}\" \"{}\"", target_dir, ref), source_cwd);
}

[[nodiscard]] inline CommandResult worktree_remove(const std::string& source_cwd, const std::string& target_dir) {
    return run_git_command(std::format("worktree remove --force \"{}\"", target_dir), source_cwd);
}


[[nodiscard]] inline std::optional<fs::path> find_git_root(const fs::path& path) {
    auto result = detail::exec_git("rev-parse --show-toplevel", path);
    if (!result.has_value()) return std::nullopt;
    return fs::path(*result);
}


[[nodiscard]] inline std::optional<fs::path> find_git_root_fs(const fs::path& start_path) {
    fs::path current = start_path;
    if (current.empty()) {
        current = fs::current_path();
    }
    
    while (true) {
        fs::path git_path = current / ".git";
        if (fs::exists(git_path) && (fs::is_directory(git_path) || fs::is_regular_file(git_path))) {
            return current;
        }
        fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}


[[nodiscard]] inline std::string get_branch(const fs::path& cwd = {}) {
    auto result = detail::exec_git("rev-parse --abbrev-ref HEAD", cwd);
    return result.has_value() ? *result : "";
}


[[nodiscard]] inline std::vector<FileStatus> get_status(const fs::path& cwd = {}) {
    auto result = detail::exec_git("status --porcelain=v1", cwd);
    if (!result.has_value()) return {};

    std::vector<FileStatus> statuses;
    for (auto& line : detail::split_lines(*result)) {
        if (line.size() < 4) continue;

        FileStatus fs;
        fs.index_status = line[0];
        fs.worktree_status = line[1];


        auto path_part = std::string_view(line).substr(3);
        auto arrow_pos = path_part.find(" -> ");
        if (arrow_pos != std::string_view::npos) {
            fs.orig_path = std::string(path_part.substr(0, arrow_pos));
            fs.path = std::string(path_part.substr(arrow_pos + 4));
        } else {
            fs.path = std::string(path_part);
        }
        statuses.push_back(std::move(fs));
    }
    return statuses;
}


[[nodiscard]] inline std::vector<std::string> get_changed_files(const fs::path& cwd = {}) {
    auto statuses = get_status(cwd);
    std::vector<std::string> files;
    for (const auto& s : statuses) {
        files.push_back(s.path);
    }
    return files;
}


[[nodiscard]] inline std::string get_diff(bool staged = false, const fs::path& cwd = {}) {
    std::string args = staged ? "diff --cached" : "diff";
    auto result = detail::exec_git(args, cwd);
    return result.has_value() ? *result : "";
}


[[nodiscard]] inline std::vector<CommitInfo> get_log(
    std::size_t count = 10, const fs::path& cwd = {}) {

    auto cmd = std::format(
        "log -n {} --format=\"%H%x00%h%x00%an%x00%aI%x00%s%x01\"", count);
    auto result = detail::exec_git(cmd, cwd);
    if (!result.has_value()) return {};

    std::vector<CommitInfo> commits;
    std::string_view data = *result;


    std::size_t pos = 0;
    while (pos < data.size()) {
        auto record_end = data.find('\x01', pos);
        if (record_end == std::string_view::npos) break;

        auto record = data.substr(pos, record_end - pos);
        pos = record_end + 1;


        while (!record.empty() && record.front() == '\n') {
            record = record.substr(1);
        }
        if (record.empty()) continue;


        CommitInfo info;
        std::size_t field_start = 0;
        int field_idx = 0;
        for (std::size_t i = 0; i <= record.size(); ++i) {
            if (i == record.size() || record[i] == '\x00') {
                auto field = record.substr(field_start, i - field_start);
                switch (field_idx) {
                    case 0: info.hash = std::string(field); break;
                    case 1: info.short_hash = std::string(field); break;
                    case 2: info.author = std::string(field); break;
                    case 3: info.date = std::string(field); break;
                    case 4: info.message = std::string(field); break;
                }
                field_start = i + 1;
                ++field_idx;
            }
        }
        if (!info.hash.empty()) {
            commits.push_back(std::move(info));
        }
    }
    return commits;
}


[[nodiscard]] inline bool is_git_repo(const fs::path& cwd = {}) {
    auto result = detail::exec_git("rev-parse --is-inside-work-tree", cwd);
    return result.has_value() && *result == "true";
}


[[nodiscard]] inline std::vector<fs::path> get_worktree_paths(const fs::path& cwd = {}) {
    auto result = detail::exec_git("worktree list --porcelain", cwd);
    if (!result.has_value()) return {};

    std::vector<fs::path> paths;
    for (auto& line : detail::split_lines(*result)) {

        if (line.starts_with("worktree ")) {
            paths.emplace_back(line.substr(9));
        }
    }
    return paths;
}


[[nodiscard]] inline int get_worktree_count(const fs::path& cwd = {}) {
    return static_cast<int>(get_worktree_paths(cwd).size());
}


[[nodiscard]] inline std::string get_remote_url(
    std::string_view remote = "origin", const fs::path& cwd = {}) {
    auto result = detail::exec_git(
        std::format("remote get-url {}", remote), cwd);
    return result.has_value() ? *result : "";
}


[[nodiscard]] inline std::string get_head_hash(const fs::path& cwd = {}) {
    auto result = detail::exec_git("rev-parse HEAD", cwd);
    return result.has_value() ? *result : "";
}


[[nodiscard]] inline bool is_clean(const fs::path& cwd = {}, bool ignore_untracked = false) {
    std::string args = "--no-optional-locks status --porcelain";
    if (ignore_untracked) {
        args += " -uno";
    }
    auto result = detail::exec_git(args, cwd);
    return result.has_value() && result->empty();
}


[[nodiscard]] inline bool is_head_on_remote(const fs::path& cwd = {}) {
    auto result = detail::exec_git("rev-parse --verify @{u}", cwd);
    return result.has_value();
}


[[nodiscard]] inline int get_unpushed_commits_count(const fs::path& cwd = {}) {
    auto result = detail::exec_git("rev-list --count @{u}..HEAD", cwd);
    if (!result.has_value()) return 0;
    try {
        return std::stoi(*result);
    } catch (...) {
        return 0;
    }
}


[[nodiscard]] inline std::optional<std::string> normalize_git_remote_url(std::string_view url) {
    std::string trimmed(url);
    while (!trimmed.empty() && std::isspace(trimmed.back())) trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(trimmed.front())) trimmed.erase(0, 1);
    
    if (trimmed.empty()) return std::nullopt;
    

    if (trimmed.starts_with("git@")) {
        size_t colon_pos = trimmed.find(':');
        if (colon_pos != std::string::npos) {
            std::string host = trimmed.substr(4, colon_pos - 4);
            std::string path = trimmed.substr(colon_pos + 1);

            if (path.ends_with(".git")) {
                path.pop_back();
                path.pop_back();
                path.pop_back();
                path.pop_back();
            }
            return std::format("{}/{}", host, path);
        }
    }
    

    if (trimmed.starts_with("http://") || trimmed.starts_with("https://") || trimmed.starts_with("ssh://")) {
        std::string_view rest = trimmed;

        if (rest.starts_with("http://")) rest = rest.substr(7);
        else if (rest.starts_with("https://")) rest = rest.substr(8);
        else if (rest.starts_with("ssh://")) rest = rest.substr(6);
        

        size_t at_pos = std::string(rest).find('@');
        if (at_pos != std::string::npos) {
            rest = rest.substr(at_pos + 1);
        }
        
        std::string result(rest);

        if (result.ends_with(".git")) {
            result.pop_back();
            result.pop_back();
            result.pop_back();
            result.pop_back();
        }
        return result;
    }
    
    return std::nullopt;
}


[[nodiscard]] inline std::optional<GitRepoState> get_git_state(const fs::path& cwd = {}) {
    if (!is_git_repo(cwd)) return std::nullopt;
    
    GitRepoState state;
    state.commit_hash = get_head_hash(cwd);
    state.branch_name = get_branch(cwd);
    state.remote_url = get_remote_url("origin", cwd);
    state.is_head_on_remote = is_head_on_remote(cwd);
    state.is_clean = is_clean(cwd);
    state.worktree_count = get_worktree_count(cwd);
    
    return state;
}

} // namespace cc::utils::git
