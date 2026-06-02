/// @file paths.cppm
/// @brief Memory directory path resolution and management.
/// Migrated from src/memdir/paths.ts, memoryTypes.ts
module;

#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <optional>

export module cc.memdir.paths;

export namespace cc::memdir {

/// Memory file types
enum class MemoryType : std::uint8_t {
    ProjectMemory,   // CLAUDE.md at project root
    UserMemory,      // ~/.claude/CLAUDE.md
    TreeMemory,      // CLAUDE.md at ancestor directories
    TeamMemory,      // Team shared memories
};

/// A resolved memory file location
struct MemoryPath {
    std::filesystem::path path;
    MemoryType type;
    bool exists = false;
};

/// Get the user-level memory file path (~/.claude/CLAUDE.md)
[[nodiscard]] inline std::filesystem::path get_user_memory_path() {
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "~");
    return home / ".claude" / "CLAUDE.md";
}

/// Get the project-level memory file path
[[nodiscard]] inline std::filesystem::path get_project_memory_path(
    const std::filesystem::path& project_root
) {
    return project_root / "CLAUDE.md";
}

/// Get all ancestor CLAUDE.md paths between cwd and filesystem root
[[nodiscard]] inline std::vector<MemoryPath> get_tree_memory_paths(
    const std::filesystem::path& cwd,
    const std::filesystem::path& project_root
) {
    std::vector<MemoryPath> paths;
    auto current = cwd;
    
    while (current != project_root && current.has_parent_path() && current != current.parent_path()) {
        auto memory_file = current / "CLAUDE.md";
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
    return project_root / ".claude" / "team-memory";
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

} // namespace cc::memdir
