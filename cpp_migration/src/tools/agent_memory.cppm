/// @file agent_memory.cppm
/// @brief Agent persistent memory scope, path resolution and prompt helpers.
/// Migrated from src/tools/AgentTool/agentMemory.ts.
///
/// NOTE: The low-level helpers `agent_memory_dir`, `load_agent_memory_prompt`,
/// `agent_memory_scope_note` and `sanitize_agent_memory_component` already live
/// in `cc.tools.agent` (agent_tool.cppm).  This module re-exports the remaining
/// public API from the TS source (scope enum, security checks, entrypoint path,
/// human-readable scope display) so that the rest of the codebase does not need
/// to pull the entire Agent tool implementation.
module;

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.agent_memory;

import cc.utils.git;

export namespace cc::tools::agent_memory {

namespace fs = std::filesystem;

// ============================================================================
// Scope
// ============================================================================

/// Persistent agent memory scope.
///   - 'user'    : ~/.claude/agent-memory/<agentType>/
///   - 'project' : <cwd>/.claude/agent-memory/<agentType>/
///   - 'local'   : <cwd>/.claude/agent-memory-local/<agentType>/ (or remote mount)
enum class Scope {
    User,
    Project,
    Local,
};

/// Parse a string scope value.  Returns nullopt for unrecognised input.
[[nodiscard]] inline std::optional<Scope> parse_scope(std::string_view value) {
    if (value == "user")    return Scope::User;
    if (value == "project") return Scope::Project;
    if (value == "local")   return Scope::Local;
    return std::nullopt;
}

/// Convert a Scope value to its canonical string form.
[[nodiscard]] inline std::string_view to_string(Scope scope) {
    switch (scope) {
        case Scope::User:    return "user";
        case Scope::Project: return "project";
        case Scope::Local:   return "local";
    }
    return "user";
}

// ============================================================================
// Path helpers
// ============================================================================

/// Sanitise an agent type name for use as a directory component.
/// Replaces colons (invalid on Windows, used in plugin-namespaced agent
/// types like "my-plugin:my-agent") with dashes.
[[nodiscard]] inline std::string sanitize_agent_type_for_path(std::string_view agent_type) {
    std::string out;
    out.reserve(agent_type.size());
    for (char ch : agent_type) {
        out.push_back(ch == ':' ? '-' : ch);
    }
    return out.empty() ? std::string{"agent"} : out;
}

/// User-level agent memory base directory (typically ~/.claude/agent-memory/).
[[nodiscard]] inline fs::path memory_base_dir() {
    if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
        return fs::path{remote};
    }
    if (const char* configured = std::getenv("CLAUDE_CONFIG_DIR"); configured && *configured) {
        return fs::path{configured};
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path{home} / ".claude";
    }
    return fs::path{".claude"};
}

/// Returns the agent memory directory for a given agent type and scope.
///   - 'project' : <cwd>/.claude/agent-memory/<agentType>/
///   - 'local'   : <cwd>/.claude/agent-memory-local/<agentType>/ (or remote mount)
///   - 'user'    : <memoryBase>/agent-memory/<agentType>/
[[nodiscard]] inline fs::path agent_memory_dir(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto dir_name = sanitize_agent_type_for_path(agent_type);
    const auto cwd = working_dir ? *working_dir : fs::current_path();

    switch (scope) {
        case Scope::Project:
            return cwd / ".claude" / "agent-memory" / dir_name;
        case Scope::Local: {
            if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
                const auto git_root = cc::utils::git::find_git_root(cwd).value_or(cwd);
                const auto project_component = sanitize_agent_type_for_path(git_root.string());
                return fs::path{remote} / "projects" / project_component / "agent-memory-local" / dir_name;
            }
            return cwd / ".claude" / "agent-memory-local" / dir_name;
        }
        case Scope::User:
            return memory_base_dir() / "agent-memory" / dir_name;
    }
    return fs::path{};
}

/// Returns the canonical entrypoint file (MEMORY.md) inside an agent memory dir.
[[nodiscard]] inline fs::path agent_memory_entrypoint(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    return agent_memory_dir(agent_type, scope, working_dir) / "MEMORY.md";
}

// ============================================================================
// Security: path membership
// ============================================================================

/// Check if a file is within any agent memory directory (any scope).
/// SECURITY: The path is normalised before comparison to prevent path
/// traversal bypasses via `..` segments.
[[nodiscard]] inline bool is_agent_memory_path(
    const fs::path& absolute_path,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    std::error_code ec;
    const auto normalized = fs::weakly_canonical(absolute_path, ec);
    const fs::path& check = ec ? absolute_path : normalized;
    const auto check_str = check.string();

    const auto cwd = working_dir ? *working_dir : fs::current_path();
    const auto sep = std::string{1, fs::path::preferred_separator};

    // User scope
    {
        const auto base = (memory_base_dir() / "agent-memory").string() + sep;
        if (check_str.rfind(base, 0) == 0) return true;
    }

    // Project scope
    {
        const auto base = (cwd / ".claude" / "agent-memory").string() + sep;
        if (check_str.rfind(base, 0) == 0) return true;
    }

    // Local scope
    if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
        const auto marker = sep + "agent-memory-local" + sep;
        const auto projects_prefix = (fs::path{remote} / "projects").string() + sep;
        if (check_str.find(marker) != std::string::npos &&
            check_str.rfind(projects_prefix, 0) == 0) {
            return true;
        }
    } else {
        const auto base = (cwd / ".claude" / "agent-memory-local").string() + sep;
        if (check_str.rfind(base, 0) == 0) return true;
    }

    return false;
}

// ============================================================================
// Display
// ============================================================================

/// Return a human-readable description of where a given scope persists.
[[nodiscard]] inline std::string memory_scope_display(std::optional<Scope> scope) {
    if (!scope) return "None";
    switch (*scope) {
        case Scope::User:
            return "User (" + (memory_base_dir() / "agent-memory").string() + "/)";
        case Scope::Project:
            return "Project (.claude/agent-memory/)";
        case Scope::Local: {
            if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
                return std::string{"Local ("} + remote + "/projects/<project>/agent-memory-local/...)";
            }
            return "Local (.claude/agent-memory-local/...)";
        }
    }
    return "None";
}

} // namespace cc::tools::agent_memory
