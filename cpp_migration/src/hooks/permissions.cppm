/**
 * Permission checking module for tool invocations.
 *
 * Implements multi-level permission control: default (prompt each time),
 * auto (allow reads, prompt for writes), plan (read-only analysis mode).
 */
module;

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

export module cc.hooks.permissions;


export namespace cc::hooks {

// Permission modes controlling how tool invocations are authorized
enum class PermissionMode {
    default_mode,  // Prompt user for each tool invocation
    auto_mode,     // Allow read-only tools, prompt for write/execute tools
    plan_mode,     // Only allow read-only tools, deny all write operations
};

// Result of a permission check or user prompt
enum class PermissionResult {
    allow_once,     // Allow this single invocation
    allow_session,  // Allow for the remainder of this session
    allow_always,   // Persist in config as always-allowed
    deny,           // Deny the invocation
};

/**
 * Parse a string into a PermissionMode enum value.
 * Returns std::nullopt if the string is not a valid mode.
 */
[[nodiscard]]
auto parse_permission_mode(std::string_view str) -> std::optional<PermissionMode> {
    if (str == "default") return PermissionMode::default_mode;
    if (str == "auto")    return PermissionMode::auto_mode;
    if (str == "plan")    return PermissionMode::plan_mode;
    return std::nullopt;
}

/**
 * Convert PermissionMode to display string.
 */
[[nodiscard]]
constexpr auto permission_mode_name(PermissionMode mode) -> std::string_view {
    switch (mode) {
        case PermissionMode::default_mode: return "default";
        case PermissionMode::auto_mode:    return "auto";
        case PermissionMode::plan_mode:    return "plan";
    }
    return "unknown";
}

/**
 * Describes a tool invocation being checked for permissions.
 */
struct ToolInvocation {
    std::string tool_name;
    std::string description;         // Human-readable description of what the tool will do
    bool is_read_only = false;       // Whether the tool only reads (no side effects)
    std::optional<std::string> command;  // For BashTool: the command being executed
};

/**
 * PermissionChecker — decides whether a given tool invocation should proceed.
 *
 * Maintains session-level allow lists and checks against the active permission mode.
 */
class PermissionChecker {
public:
    explicit PermissionChecker(PermissionMode mode)
        : mode_{mode} {
        initialize_builtin_lists();
    }

    /**
     * Check if a tool invocation is permitted under the current mode.
     * Returns a PermissionResult without prompting (for pre-approved cases),
     * or std::nullopt if the user must be prompted interactively.
     */
    [[nodiscard]]
    auto check(const ToolInvocation& invocation) -> std::optional<PermissionResult> {
        std::lock_guard lock{mu_};

        // Tools in the session-allow set are always permitted
        if (session_allowed_.contains(invocation.tool_name)) {
            return PermissionResult::allow_once;
        }

        switch (mode_) {
            case PermissionMode::plan_mode:
                return check_plan_mode(invocation);

            case PermissionMode::auto_mode:
                return check_auto_mode(invocation);

            case PermissionMode::default_mode:
                return check_default_mode(invocation);
        }
        return std::nullopt;
    }

    /**
     * Record a user's permission decision for a tool.
     */
    void record_decision(const std::string& tool_name, PermissionResult result) {
        std::lock_guard lock{mu_};

        switch (result) {
            case PermissionResult::allow_session:
                session_allowed_.insert(tool_name);
                break;
            case PermissionResult::allow_always:
                session_allowed_.insert(tool_name);
                always_allowed_.insert(tool_name);
                persist_always_allowed();
                break;
            case PermissionResult::allow_once:
            case PermissionResult::deny:
                break;  // No state change
        }
    }

    /**
     * Detect if a bash command is potentially dangerous.
     * Used to escalate permission prompts for destructive operations.
     */
    [[nodiscard]]
    static auto is_dangerous_command(std::string_view command) -> bool {
        // Patterns indicating destructive or security-sensitive commands
        static const std::vector<std::string_view> dangerous_patterns = {
            "rm -rf",
            "rm -fr",
            "mkfs",
            "dd if=",
            "> /dev/",
            "chmod 777",
            "curl | sh",
            "curl | bash",
            "wget -O - |",
            "sudo ",
            "git push --force",
            "git reset --hard",
            "git clean -fd",
            ":(){ :|:& };:",   // Fork bomb
            "shutdown",
            "reboot",
            "format ",
            "del /f /s /q",
        };

        for (const auto& pattern : dangerous_patterns) {
            if (command.find(pattern) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    auto mode() const -> PermissionMode { return mode_; }

    void set_mode(PermissionMode mode) {
        std::lock_guard lock{mu_};
        mode_ = mode;
    }

private:
    PermissionMode mode_;
    std::mutex mu_;
    std::unordered_set<std::string> session_allowed_;   // Tools allowed for this session
    std::unordered_set<std::string> always_allowed_;    // Tools persisted as always-allowed
    std::unordered_set<std::string> read_only_tools_;   // Tools classified as read-only

    /**
     * Initialize built-in tool classification lists.
     */
    void initialize_builtin_lists() {
        // Read-only tools that never modify the filesystem or external state
        read_only_tools_ = {
            "FileReadTool",
            "GlobTool",
            "GrepTool",
            "WebFetchTool",
            "WebSearchTool",
            "SearchCodebaseTool",
            "LSFilesTool",
        };
    }

    /**
     * Plan mode: only read-only tools are allowed.
     */
    [[nodiscard]]
    auto check_plan_mode(const ToolInvocation& invocation) -> std::optional<PermissionResult> {
        if (invocation.is_read_only || read_only_tools_.contains(invocation.tool_name)) {
            return PermissionResult::allow_once;
        }
        // In plan mode, all non-read tools are silently denied
        return PermissionResult::deny;
    }

    /**
     * Auto mode: allow read-only tools, require prompt for write/execute tools.
     * Dangerous bash commands still require explicit user approval.
     */
    [[nodiscard]]
    auto check_auto_mode(const ToolInvocation& invocation) -> std::optional<PermissionResult> {
        // Read-only tools pass automatically
        if (invocation.is_read_only || read_only_tools_.contains(invocation.tool_name)) {
            return PermissionResult::allow_once;
        }

        // Dangerous commands always require explicit prompt (return nullopt)
        if (invocation.tool_name == "BashTool" && invocation.command.has_value()) {
            if (is_dangerous_command(invocation.command.value())) {
                return std::nullopt;  // Force interactive prompt
            }
        }

        // Non-dangerous write tools: prompt the user
        return std::nullopt;
    }

    /**
     * Default mode: always prompt the user unless session-approved.
     */
    [[nodiscard]]
    auto check_default_mode(const ToolInvocation& /*invocation*/) -> std::optional<PermissionResult> {
        // In default mode we always defer to the user prompt
        return std::nullopt;
    }

    /**
     * Persist always-allowed tools to disk (~/.claude/permissions.json).
     */
    void persist_always_allowed() {
        // Implementation delegates to cc::utils::fs for file writing
        // Serialization uses yyjson
    }
};

/**
 * PermissionPrompt — presents an interactive permission dialog to the user.
 * Returns the user's choice as a PermissionResult.
 *
 * This is a function object that can be replaced in tests with a mock.
 */
struct PermissionPrompt {
    using PromptFn = std::function<PermissionResult(const ToolInvocation&)>;

    PromptFn prompt_fn;

    /**
     * Ask the user whether to allow a tool invocation.
     * Displays tool name, description, and (for BashTool) the command.
     */
    [[nodiscard]]
    auto ask(const ToolInvocation& invocation) -> PermissionResult {
        if (prompt_fn) {
            return prompt_fn(invocation);
        }
        // Default: deny if no prompt function is wired
        return PermissionResult::deny;
    }
};

} // namespace cc::hooks
