/**
 * Context management module — builds and maintains the system prompt
 * and context window budget for LLM interactions.
 *
 * Assembles project context (CLAUDE.md), git state, environment info,
 * tool descriptions, MCP tools, skills, and handles context compression
 * when approaching token limits.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>

export module cc.hooks.context;


namespace fs = std::filesystem;

export namespace cc::core {
struct Config {
    [[nodiscard]] std::optional<std::size_t> context_window_size() const { return std::nullopt; }
};
}

export namespace cc::tools {
struct ToolInfo {
    [[nodiscard]] std::string name() const { return {}; }
    [[nodiscard]] std::string description() const { return {}; }
};

struct Registry {
    [[nodiscard]] std::vector<std::shared_ptr<ToolInfo>> all_tools() const { return {}; }
};
}

export namespace cc::skills {
struct SkillInfo {
    std::string name;
    std::string description;
};
}

export namespace cc::services {
struct McpToolDescription {
    std::string name;
    std::string server_name;
    std::string description;
};

struct McpManager {
    [[nodiscard]] std::vector<McpToolDescription> tool_descriptions() const { return {}; }
};
}

namespace cc::utils {
[[nodiscard]] inline std::optional<std::string> read_file_to_string(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}
}

export namespace cc::hooks {

/**
 * Token budget tracking for the context window.
 */
struct ContextBudget {
    std::size_t max_tokens = 200'000;     // Model's context window size
    std::size_t reserved_for_output = 16'000;  // Reserved for assistant response
    std::size_t system_prompt_tokens = 0;
    std::size_t conversation_tokens = 0;

    // Approximate available tokens for new messages
    [[nodiscard]]
    auto available() const -> std::size_t {
        auto used = system_prompt_tokens + conversation_tokens + reserved_for_output;
        return (used >= max_tokens) ? 0 : (max_tokens - used);
    }

    // Whether context is approaching limits (within 10% of capacity)
    [[nodiscard]]
    auto needs_compression() const -> bool {
        auto threshold = max_tokens * 9 / 10;
        return (system_prompt_tokens + conversation_tokens) >= threshold;
    }

    // Utilization percentage (0-100)
    [[nodiscard]]
    auto utilization_percent() const -> int {
        auto used = system_prompt_tokens + conversation_tokens;
        return static_cast<int>(used * 100 / max_tokens);
    }
};

/**
 * Git context snapshot — captures current repository state.
 */
struct GitContext {
    std::optional<std::string> branch;
    std::optional<std::string> status_summary;  // e.g., "3 modified, 1 untracked"
    std::optional<std::string> recent_commits;  // Last few commit onelines
    bool is_repo = false;
};

/**
 * Environment context — runtime environment details.
 */
struct EnvironmentContext {
    std::string os_name;
    std::string shell;
    std::string cwd;
    std::string username;
    std::optional<std::string> project_name;  // Derived from directory name or config
};

/**
 * Initialization parameters for ContextManager (aggregate init).
 */
struct ContextManagerInit {
    const cc::core::Config& config;
    const cc::tools::Registry& tool_registry;
    const std::vector<cc::skills::SkillInfo>& skills;
    const cc::services::McpManager* mcp_manager;  // nullable if no MCP configured
};

/**
 * ContextManager — orchestrates assembly of the full system prompt.
 *
 * Responsible for:
 *  - Loading project-specific context (CLAUDE.md)
 *  - Gathering git status
 *  - Building tool description blocks
 *  - Injecting skill content
 *  - Managing context window budget
 *  - Triggering compression when needed
 */
class ContextManager {
public:
    explicit ContextManager(ContextManagerInit init)
        : config_{init.config}
        , tool_registry_{init.tool_registry}
        , skills_{init.skills}
        , mcp_manager_{init.mcp_manager}
    {
        // Initialize budget from model config
        budget_.max_tokens = config_.context_window_size().value_or(200'000);
    }

    /**
     * Build the complete system prompt by assembling all context sources.
     * This is called before each LLM query.
     */
    [[nodiscard]]
    auto build_system_prompt() const -> std::string {
        std::string prompt;
        prompt.reserve(32'000);  // Pre-allocate reasonable initial capacity

        // Core identity and instructions
        append_section(prompt, "Identity", build_identity_section());

        // Project-specific context (CLAUDE.md if present)
        auto project_ctx = load_project_context();
        if (project_ctx.has_value()) {
            append_section(prompt, "Project Context", project_ctx.value());
        }

        // Environment context
        append_section(prompt, "Environment", build_environment_section());

        // Git context
        auto git_ctx = gather_git_context();
        if (git_ctx.is_repo) {
            append_section(prompt, "Git Status", format_git_context(git_ctx));
        }

        // Tool descriptions
        append_section(prompt, "Available Tools", build_tools_section());

        // MCP tool descriptions (if any)
        if (mcp_manager_) {
            auto mcp_section = build_mcp_tools_section();
            if (!mcp_section.empty()) {
                append_section(prompt, "MCP Tools", mcp_section);
            }
        }

        // Skill content injection
        auto skills_section = build_skills_section();
        if (!skills_section.empty()) {
            append_section(prompt, "Skills", skills_section);
        }

        // Update token estimate (rough: 1 token ≈ 4 chars for English)
        budget_.system_prompt_tokens = prompt.size() / 4;

        return prompt;
    }

    /**
     * Check if context compression should be triggered.
     */
    [[nodiscard]]
    auto should_compress() const -> bool {
        return budget_.needs_compression();
    }

    /**
     * Compress conversation history to reclaim context window space.
     * Returns a summarized version of the conversation suitable for re-injection.
     */
    [[nodiscard]]
    auto compress_conversation(std::string_view conversation_history) const -> std::string {
        // Compression strategy: keep first message, last N messages,
        // and summarize the middle section
        auto target_size = conversation_history.size() / 3;  // Aim for ~33% of original

        // Simple truncation-based compression (real impl would use LLM summarization)
        if (conversation_history.size() <= target_size) {
            return std::string{conversation_history};
        }

        std::string compressed;
        compressed.reserve(target_size);

        // Keep header portion
        auto header_size = target_size / 2;
        compressed += conversation_history.substr(0, header_size);
        compressed += "\n\n[... earlier conversation summarized ...]\n\n";

        // Keep tail portion
        auto tail_start = conversation_history.size() - (target_size / 2);
        compressed += conversation_history.substr(tail_start);

        return compressed;
    }

    /**
     * Update conversation token count after a new message exchange.
     */
    void update_conversation_tokens(std::size_t token_count) {
        budget_.conversation_tokens = token_count;
    }

    /**
     * Get current context budget status.
     */
    [[nodiscard]]
    auto budget() const -> const ContextBudget& { return budget_; }

private:
    const cc::core::Config& config_;
    const cc::tools::Registry& tool_registry_;
    const std::vector<cc::skills::SkillInfo>& skills_;
    const cc::services::McpManager* mcp_manager_;
    mutable ContextBudget budget_;

    /**
     * Build the identity/role section of the system prompt.
     */
    [[nodiscard]]
    static auto build_identity_section() -> std::string {
        return "You are an AI assistant with access to tools for reading, writing, "
               "and executing code. You help users with software development tasks "
               "by understanding their codebase and making precise changes.";
    }

    /**
     * Load project context from CLAUDE.md in the working directory or parents.
     * Searches upward from cwd until a CLAUDE.md is found or root is reached.
     */
    [[nodiscard]]
    auto load_project_context() const -> std::optional<std::string> {
        auto cwd = fs::current_path();

        // Walk up directory tree looking for CLAUDE.md
        for (auto dir = cwd; dir != dir.root_path(); dir = dir.parent_path()) {
            auto claude_md = dir / "CLAUDE.md";
            if (fs::exists(claude_md)) {
                auto content = cc::utils::read_file_to_string(claude_md);
                if (content.has_value() && !content->empty()) {
                    return content;
                }
            }
        }
        return std::nullopt;
    }

    /**
     * Build environment context section.
     */
    [[nodiscard]]
    auto build_environment_section() const -> std::string {
        auto env = gather_environment();
        std::string section;
        section += std::format("- OS: {}\n", env.os_name);
        section += std::format("- Shell: {}\n", env.shell);
        section += std::format("- Working Directory: {}\n", env.cwd);
        if (env.project_name.has_value()) {
            section += std::format("- Project: {}\n", env.project_name.value());
        }
        return section;
    }

    /**
     * Gather current environment information.
     */
    [[nodiscard]]
    static auto gather_environment() -> EnvironmentContext {
        EnvironmentContext env;

        #if defined(__APPLE__)
            env.os_name = "macOS";
        #elif defined(__linux__)
            env.os_name = "Linux";
        #else
            env.os_name = "Unknown";
        #endif

        // Read shell from environment
        if (auto shell_env = std::getenv("SHELL")) {
            env.shell = shell_env;
        } else {
            env.shell = "/bin/sh";
        }

        env.cwd = fs::current_path().string();

        // Derive project name from current directory
        env.project_name = fs::current_path().filename().string();

        if (auto user = std::getenv("USER")) {
            env.username = user;
        }

        return env;
    }

    /**
     * Gather git repository context (branch, status).
     */
    [[nodiscard]]
    static auto gather_git_context() -> GitContext {
        GitContext ctx;

        // Check if we're in a git repo
        if (!fs::exists(".git") && !fs::exists(fs::current_path() / ".git")) {
            ctx.is_repo = false;
            return ctx;
        }
        ctx.is_repo = true;

        // Read branch from HEAD (avoids spawning git subprocess)
        auto head_path = fs::path{".git"} / "HEAD";
        if (fs::exists(head_path)) {
            auto content = cc::utils::read_file_to_string(head_path);
            if (content.has_value()) {
                constexpr std::string_view ref_prefix = "ref: refs/heads/";
                if (content->starts_with(ref_prefix)) {
                    // Strip prefix and trailing newline
                    ctx.branch = content->substr(ref_prefix.size());
                    if (ctx.branch->ends_with('\n')) {
                        ctx.branch->pop_back();
                    }
                }
            }
        }

        return ctx;
    }

    /**
     * Format git context into a human-readable section.
     */
    [[nodiscard]]
    static auto format_git_context(const GitContext& ctx) -> std::string {
        std::string section;
        if (ctx.branch.has_value()) {
            section += std::format("- Branch: {}\n", ctx.branch.value());
        }
        if (ctx.status_summary.has_value()) {
            section += std::format("- Status: {}\n", ctx.status_summary.value());
        }
        return section;
    }

    /**
     * Build tool descriptions section from the tool registry.
     */
    [[nodiscard]]
    auto build_tools_section() const -> std::string {
        std::string section;
        for (const auto& tool : tool_registry_.all_tools()) {
            section += std::format("## {}\n{}\n\n", tool->name(), tool->description());
        }
        return section;
    }

    /**
     * Build MCP tools section from connected MCP servers.
     */
    [[nodiscard]]
    auto build_mcp_tools_section() const -> std::string {
        if (!mcp_manager_) return {};

        std::string section;
        for (const auto& tool : mcp_manager_->tool_descriptions()) {
            section += std::format("## {} (MCP: {})\n{}\n\n",
                                   tool.name, tool.server_name, tool.description);
        }
        return section;
    }

    /**
     * Build skills section with available skill descriptions.
     */
    [[nodiscard]]
    auto build_skills_section() const -> std::string {
        if (skills_.empty()) return {};

        std::string section;
        for (const auto& skill : skills_) {
            section += std::format("- **{}**: {}\n", skill.name, skill.description);
        }
        return section;
    }

    /**
     * Append a labeled section to the prompt with consistent formatting.
     */
    static void append_section(std::string& prompt, std::string_view title, std::string_view content) {
        if (content.empty()) return;
        prompt += std::format("\n# {}\n{}\n", title, content);
    }
};

} // namespace cc::hooks
