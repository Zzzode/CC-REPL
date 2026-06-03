// C++23 Memory Types and Versions Module
// Provides memory type definitions and version utilities
module;

#include <vector>
#include <string>
#include <string_view>
#include <optional>

export module cc.utils.memory;

import cc.utils.git;

export namespace cc::utils::memory {


enum class MemoryType {
    User,
    Project,
    Local,
    Managed,
    AutoMem,
    TeamMem
};


[[nodiscard]] inline constexpr std::string_view memory_type_to_string(MemoryType type) noexcept {
    switch (type) {
        case MemoryType::User: return "User";
        case MemoryType::Project: return "Project";
        case MemoryType::Local: return "Local";
        case MemoryType::Managed: return "Managed";
        case MemoryType::AutoMem: return "AutoMem";
        case MemoryType::TeamMem: return "TeamMem";
        default: return "Unknown";
    }
}


[[nodiscard]] inline auto get_memory_types() -> std::vector<MemoryType> {
    return {
        MemoryType::User,
        MemoryType::Project,
        MemoryType::Local,
        MemoryType::Managed,
        MemoryType::AutoMem,
        MemoryType::TeamMem
    };
}


[[nodiscard]] inline auto project_in_git_repo(std::string_view cwd) -> bool {
    return git::find_git_root(cwd).has_value();
}

} // namespace cc::utils::memory
