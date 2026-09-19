/// @file paths.cppm
/// @brief Memory directory path resolution and management.
/// Migrated from src/memdir/paths.ts, memoryTypes.ts
module;

#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <optional>
#include <cstdlib>
#include <cstdio>
#include <cctype>

export module cc.memdir.paths;

export namespace cc::memdir {

/// Memory file types
enum class MemoryType : std::uint8_t {
    ProjectMemory,   // LOOM.md at project root
    UserMemory,      // ~/.loom/LOOM.md
    TreeMemory,      // LOOM.md at ancestor directories
    TeamMemory,      // Team shared memories
};

/// A resolved memory file location
struct MemoryPath {
    std::filesystem::path path;
    MemoryType type;
    bool exists = false;
};

/// Get the user-level memory file path (~/.loom/LOOM.md)
[[nodiscard]] inline std::filesystem::path get_user_memory_path() {
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "~");
    return home / ".loom" / "LOOM.md";
}

/// Get the project-level memory file path
[[nodiscard]] inline std::filesystem::path get_project_memory_path(
    const std::filesystem::path& project_root
) {
    return project_root / "LOOM.md";
}

/// Get all ancestor LOOM.md paths between cwd and filesystem root
[[nodiscard]] inline std::vector<MemoryPath> get_tree_memory_paths(
    const std::filesystem::path& cwd,
    const std::filesystem::path& project_root
) {
    std::vector<MemoryPath> paths;
    auto current = cwd;
    
    while (current != project_root && current.has_parent_path() && current != current.parent_path()) {
        auto memory_file = current / "LOOM.md";
        if (std::filesystem::exists(memory_file)) {
            paths.push_back(MemoryPath{
                .path = memory_file,
                .type = MemoryType::TreeMemory,
                .exists = true,
            });
        }
        current = current.parent_path();
    }
    
    return paths;
}

/// Get team memory directory path
[[nodiscard]] inline std::filesystem::path get_team_memory_dir(
    const std::filesystem::path& project_root
) {
    return project_root / ".loom" / "team-memory";
}

/// Resolve all memory paths for the current session
[[nodiscard]] inline std::vector<MemoryPath> resolve_all_memory_paths(
    const std::filesystem::path& cwd,
    const std::filesystem::path& project_root
) {
    std::vector<MemoryPath> all;
    
    // User memory
    auto user_path = get_user_memory_path();
    all.push_back(MemoryPath{
        .path = user_path,
        .type = MemoryType::UserMemory,
        .exists = std::filesystem::exists(user_path),
    });
    
    // Project memory
    auto proj_path = get_project_memory_path(project_root);
    all.push_back(MemoryPath{
        .path = proj_path,
        .type = MemoryType::ProjectMemory,
        .exists = std::filesystem::exists(proj_path),
    });
    
    // Tree memories
    auto tree = get_tree_memory_paths(cwd, project_root);
    all.insert(all.end(), tree.begin(), tree.end());
    
    return all;
}

// ============================================================================
// Auto-memory path layer (faithful port of src/memdir/paths.ts)
//
// Canonical per-project auto-memory directory:
//   <LOOM_CONFIG_DIR or $HOME/.loom>/projects/<sanitized-git-root>/memory/
// with MEMORY.md as the always-loaded index. An explicit override
// (LOOM_COWORK_MEMORY_PATH_OVERRIDE) replaces the whole computation.
// ===========================================================================

/// Non-alphanumeric bytes become '-' (TS sanitizePath). Kept simple/stable —
/// the TS long-name hash suffix is omitted because the C++ side only needs a
/// deterministic, collision-resistant-enough per-project key.
[[nodiscard]] inline std::string sanitize_memory_key(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= 'a' && uc <= 'z') ||
                        (uc >= 'A' && uc <= 'Z') ||
                        (uc >= '0' && uc <= '9');
        out.push_back(ok ? c : '-');
    }
    return out;
}

/// Find the canonical git root for a path (all worktrees of a repo share one
/// memory dir); falls back to the path itself when not inside a git repo.
[[nodiscard]] inline std::filesystem::path find_canonical_git_root(
    const std::filesystem::path& start) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(start, ec);
    if (ec) abs = start;
    std::string cmd =
        "git -C \"" + abs.string() + "\" rev-parse --show-toplevel 2>/dev/null";
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    if (!out.empty()) return std::filesystem::path(out);
    return abs;
}

/// Resolve the config home: $LOOM_CONFIG_DIR else $HOME/.loom.
[[nodiscard]] inline std::filesystem::path loom_config_home() {
    if (const char* dir = std::getenv("LOOM_CONFIG_DIR"); dir && *dir) {
        return std::filesystem::path(dir);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".loom";
    }
    return std::filesystem::path(".loom");
}

/// Whether auto-memory is enabled. TS enablement chain:
/// LOOM_DISABLE_AUTO_MEMORY (1/true → off) wins.
[[nodiscard]] inline bool is_auto_memory_enabled() {
    const char* disable = std::getenv("LOOM_DISABLE_AUTO_MEMORY");
    if (disable) {
        std::string_view v(disable);
        if (v == "1" || v == "true" || v == "TRUE") return false;
        if (v == "0" || v == "false" || v == "FALSE") return true;
    }
    return true;
}

/// Canonical auto-memory directory for the given working/project root.
/// Returns nullopt when auto-memory is disabled. Honors the full-path
/// override used by SDK/cowork embeddings.
[[nodiscard]] inline std::optional<std::filesystem::path> get_auto_mem_path(
    const std::filesystem::path& project_root) {
    if (!is_auto_memory_enabled()) return std::nullopt;

    if (const char* ov = std::getenv("LOOM_COWORK_MEMORY_PATH_OVERRIDE");
        ov && *ov) {
        return std::filesystem::path(ov);
    }

    auto base = find_canonical_git_root(project_root);
    auto key = sanitize_memory_key(base.string());
    auto path = loom_config_home() / "projects" / key / "memory";
    return path;
}

/// The MEMORY.md index inside the auto-memory directory.
[[nodiscard]] inline std::optional<std::filesystem::path>
get_auto_mem_entrypoint(const std::filesystem::path& project_root) {
    auto dir = get_auto_mem_path(project_root);
    if (!dir) return std::nullopt;
    return *dir / "MEMORY.md";
}

// ============================================================================
// Session memory (summary.md) for context compaction
//
// TS REF: src/utils/permissions/filesystem.ts:262 getSessionMemoryDir /
// :269 getSessionMemoryPath — <config_home>/projects/<sanitized-cwd>/
// <sessionId>/session-memory/summary.md. Unlike the long-term auto-memory
// above, this file is scoped to one session and accumulates the summaries
// produced at each compaction so resumed/continuation runs do not start
// blind after old messages are dropped.
// ============================================================================

/// Per-project projects key (same sanitization as the auto-memory key, but
/// derived from the cwd directly — TS getProjectDir(getCwd()) does not walk
/// to the git root here).
[[nodiscard]] inline std::filesystem::path get_project_state_dir(
    const std::filesystem::path& cwd) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(cwd, ec);
    if (ec) abs = cwd;
    return loom_config_home() / "projects" /
           sanitize_memory_key(abs.string());
}

[[nodiscard]] inline std::filesystem::path get_session_memory_dir(
    const std::filesystem::path& cwd,
    std::string_view session_id) {
    return get_project_state_dir(cwd) /
           std::filesystem::path(std::string(session_id)) /
           "session-memory";
}

[[nodiscard]] inline std::filesystem::path get_session_memory_path(
    const std::filesystem::path& cwd,
    std::string_view session_id) {
    return get_session_memory_dir(cwd, session_id) / "summary.md";
}

} // namespace cc::memdir
