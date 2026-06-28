/// @file file_index.cppm
/// @brief AT-01: repo-wide file index for @-mention suggestions. The C++ port
/// previously listed only the immediate parent directory via directory_iterator,
/// so "@readme" found nothing unless readme sat in cwd. This mirrors TS, which
/// fuzzy-matches the whole repo via the file index.
///
/// Backed by `git ls-files` (tracked) + `git ls-files --others --exclude-standard`
/// (untracked, gitignored excluded). Cached per-cwd and refreshed when
/// .git/index mtime changes, so we don't fork a git subprocess on every
/// keystroke and new/deleted files appear without a restart.
module;

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.ui.prompt.file_index;

import cc.utils.bash_execution;

export namespace cc::ui::prompt::file_index {

namespace fs = std::filesystem;
namespace bash = cc::utils::bash;

namespace detail {

/// Run `git -C <cwd> <args>` and return the trimmed, deduped line list. Empty
/// on any failure (non-repo, git missing). @a cwd comes from the app's own cwd
/// and is not user-controlled, so it is safe to embed directly.
[[nodiscard]] inline std::vector<std::string> run_git(const fs::path& cwd,
                                                      std::string_view args) {
    std::vector<std::string> out;
    const std::string cmd =
        "git -C " + cwd.string() + " " + std::string(args) + " 2>/dev/null";
    FILE* pipe = bash::popen_spawn(cmd.c_str());
    if (!pipe) return out;

    std::array<char, 4096> buffer{};
    std::string stream;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        stream += buffer.data();
    }
    bash::pclose_spawn(pipe);

    std::unordered_set<std::string> seen;
    std::size_t pos = 0;
    while (pos < stream.size()) {
        const std::size_t nl = stream.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? stream.substr(pos)
            : stream.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? stream.size() : nl + 1;
        // Trim trailing CR.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (seen.insert(line).second) out.push_back(std::move(line));
    }
    return out;
}

struct CacheEntry {
    std::vector<std::string> files;
    fs::file_time_type index_mtime{};
    bool has_mtime = false;
};

}  // namespace detail

/// AT-01: return the cached repo-wide file list (tracked + non-gitignored
/// untracked) for @a cwd_text. Refreshed when .git/index mtime changes. Empty
/// for non-repo directories (callers fall back to directory scanning).
[[nodiscard]] inline std::vector<std::string> collect_repo_files(
    std::string_view cwd_text) {
    static std::unordered_map<std::string, detail::CacheEntry> cache;
    static std::mutex mu;

    const fs::path cwd = cwd_text.empty()
        ? fs::current_path()
        : fs::path(std::string(cwd_text));
    const std::string key = cwd.string();

    std::error_code ec;
    const fs::path git_index = cwd / ".git" / "index";
    const auto idx_mtime = fs::last_write_time(git_index, ec);
    const bool idx_ok = !ec;

    {
        std::lock_guard lk(mu);
        const auto it = cache.find(key);
        if (it != cache.end() && !it->second.files.empty()) {
            bool stale = false;
            if (idx_ok && it->second.has_mtime &&
                idx_mtime != it->second.index_mtime) {
                stale = true;
            }
            if (!stale) return it->second.files;
        }
    }

    auto tracked = detail::run_git(cwd, "ls-files");
    auto untracked = detail::run_git(cwd, "ls-files --others --exclude-standard");
    // Merge preserving order; tracked first, deduped.
    std::unordered_set<std::string> seen;
    std::vector<std::string> merged;
    merged.reserve(tracked.size() + untracked.size());
    auto push_new = [&](std::vector<std::string>& src) {
        for (auto& f : src) {
            if (seen.insert(f).second) merged.push_back(std::move(f));
        }
    };
    push_new(tracked);
    push_new(untracked);

    // Fallback: if git produced nothing (not a git repo / git missing), walk
    // the directory tree so @-mentions still work outside a repo (TS parity —
    // TS @file degrades to an fs scan when there's no repo index). Bounded +
    // skips VCS/build/heavy dirs so a temp/test cwd stays cheap.
    if (merged.empty()) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 cwd, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            const auto name = entry.path().filename().string();
            if (entry.is_directory(ec) &&
                (name == ".git" || name == "node_modules" || name == "build" ||
                 name == "_deps" || name == ".claude" || name == "__pycache__" ||
                 name == ".next" || name == "target" || name == "dist")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(ec)) continue;
            auto rel = fs::relative(entry.path(), cwd, ec);
            if (ec) continue;
            auto rel_str = rel.string();
            if (seen.insert(rel_str).second) merged.push_back(std::move(rel_str));
            if (merged.size() >= 5000) break;  // cap for very large trees
        }
    }

    std::lock_guard lk(mu);
    auto& entry = cache[key];
    entry.files = merged;  // keep a copy in the cache
    if (idx_ok) {
        entry.index_mtime = idx_mtime;
        entry.has_mtime = true;
    }
    return merged;
}

}  // namespace cc::ui::prompt::file_index
