/// @file paths.cppm
/// @brief Config-directory and memory-file resolution policy.
///
/// Two cascades live here, and they are the ONLY place either is spelled out.
/// Before this module the same lookup was reimplemented in eight walkers
/// (query_engine, hooks/context, memdir/memory, memdir/paths,
/// utils/system_directories, config/settings, hooks/shell_hooks, and the
/// memory commands), each with its own hardcoded filename. A change to any of
/// them silently applied to one code path and not the others; keeping the
/// order in one exported constant is what makes that impossible.
///
/// ## Config directory cascade
///   $LOOM_CONFIG_DIR  (explicit override, wins outright)
///   $HOME/.loom       (current name)
///   $HOME/.agents     (interop: the AGENTS.md ecosystem's convention)
///   $HOME/.claude     (the pre-rename name, so existing data keeps working)
///   $HOME/.loom       (fallback when none exists: we create rather than
///                      inherit a directory we did not choose)
///
/// ## Memory file cascade
///   LOOM.md -> AGENTS.md -> CLAUDE.md
///
/// The memory cascade is resolved PER DIRECTORY while walking up, not by
/// searching the whole tree for one name before trying the next. So the
/// nearest directory wins even on a weaker name: a CLAUDE.md in the project
/// root beats a LOOM.md five levels up. This is the behaviour the AGENTS.md
/// ecosystem settled on, and the alternative would let a distant file of the
/// preferred name override the file sitting next to the code being edited.
module;

#include <cstdlib>

export module cc.constants.paths;

import std;

export namespace cc::constants::paths {

/// The product's own config directory, as a dot-directory under $HOME.
inline constexpr std::string_view kConfigDirName = ".loom";

/// Config-directory candidates, highest priority first. All are dot-dirs
/// under $HOME. Order is load-bearing: see the file comment.
inline constexpr std::array<std::string_view, 3> kConfigDirCandidates = {
    ".loom",
    ".agents",
    ".claude",
};

/// Memory file names, highest priority first. Order is load-bearing: see the
/// file comment.
inline constexpr std::array<std::string_view, 3> kMemoryFileCandidates = {
    "LOOM.md",
    "AGENTS.md",
    "CLAUDE.md",
};

/// True when `name` is one of the recognised memory file names.
[[nodiscard]] inline bool is_memory_file_name(std::string_view name) {
    for (const auto candidate : kMemoryFileCandidates) {
        if (name == candidate) return true;
    }
    return false;
}

/// $HOME, or "/tmp" when unset (matching the rest of the codebase, which
/// prefers a writable directory over failing).
[[nodiscard]] inline std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    return (home && *home) ? std::filesystem::path{home} : std::filesystem::path{"/tmp"};
}

/// Resolve the config directory by cascade, under an explicit home root.
/// Returns the highest-priority candidate that EXISTS; when none exist,
/// returns the preferred path (`.loom`).
///
/// READ-ONLY intent: this is for finding configuration that already exists.
/// For the directory to WRITE into, use config_home_write() -- writing into a
/// directory we did not create is a different decision (see below).
[[nodiscard]] inline std::filesystem::path config_home_read_under(
    const std::filesystem::path& home) {
    if (const char* env = std::getenv("LOOM_CONFIG_DIR"); env && *env) {
        return std::filesystem::path{env};
    }
    std::error_code ec;
    for (const auto name : kConfigDirCandidates) {
        auto candidate = home / name;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return home / kConfigDirName;
}

/// Resolve the config directory to READ from, under $HOME.
[[nodiscard]] inline std::filesystem::path config_home_read() {
    return config_home_read_under(home_dir());
}

/// The config directory to WRITE into: $LOOM_CONFIG_DIR, else `~/.loom`.
///
/// Deliberately narrower than the read cascade. Reading a legacy `~/.claude`
/// is safe and is the point of the cascade; writing there is not the same
/// act. If a user has only `~/.claude`, writing our `sessions/`, `plugins/`
/// and `settings.json` into it would interleave our state with another
/// tool's, in a directory the user did not choose for us -- and `~/.claude`
/// already has its own `sessions/` for the other tool to collide with. So we
/// create our own directory instead, and leave theirs alone.
[[nodiscard]] inline std::filesystem::path config_home_write() {
    if (const char* env = std::getenv("LOOM_CONFIG_DIR"); env && *env) {
        return std::filesystem::path{env};
    }
    return home_dir() / kConfigDirName;
}

/// Every config-directory candidate that exists, highest priority first.
/// Use this when the caller wants to read from ALL of them (e.g. skill
/// discovery, where a user may legitimately have skills in more than one
/// location) rather than pick one.
[[nodiscard]] inline std::vector<std::filesystem::path> existing_config_homes() {
    std::vector<std::filesystem::path> found;
    if (const char* env = std::getenv("LOOM_CONFIG_DIR"); env && *env) {
        found.emplace_back(env);
        return found;
    }
    const auto home = home_dir();
    std::error_code ec;
    for (const auto name : kConfigDirCandidates) {
        auto candidate = home / name;
        if (std::filesystem::exists(candidate, ec)) {
            found.push_back(candidate);
        }
    }
    return found;
}

/// The memory file inside `dir`, by cascade, or nullopt when none exists.
/// Non-recursive: only this directory is considered.
[[nodiscard]] inline std::optional<std::filesystem::path> memory_file_in(
    const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto name : kMemoryFileCandidates) {
        auto candidate = dir / name;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// Walk up from `start_dir` looking for a memory file, applying the name
/// cascade within each directory. Returns the nearest match, or nullopt.
[[nodiscard]] inline std::optional<std::filesystem::path> find_memory_file(
    const std::filesystem::path& start_dir) {
    std::error_code ec;
    auto current = std::filesystem::absolute(start_dir, ec);
    if (ec) current = start_dir;

    while (true) {
        if (auto found = memory_file_in(current)) {
            return found;
        }
        auto parent = current.parent_path();
        if (parent == current) break;  // filesystem root
        current = parent;
    }
    return std::nullopt;
}

/// The project's memory file path for `root`, preferring an existing file and
/// otherwise the highest-priority name (so a caller reporting "where memory
/// would go" names the file we would actually create).
[[nodiscard]] inline std::filesystem::path project_memory_path(
    const std::filesystem::path& root) {
    if (auto found = memory_file_in(root)) {
        return *found;
    }
    return root / std::string{kMemoryFileCandidates.front()};
}

/// The user-level memory file path: the config dir's memory file. Prefers an
/// existing file across the whole READ cascade so a user's
/// `~/.claude/CLAUDE.md` is still found after the rename. Falls back to the
/// WRITE directory, since that is where a new memory file would be created.
[[nodiscard]] inline std::filesystem::path user_memory_path() {
    if (auto found = find_memory_file(config_home_read())) {
        return *found;
    }
    return config_home_write() / std::string{kMemoryFileCandidates.front()};
}

}  // namespace cc::constants::paths
