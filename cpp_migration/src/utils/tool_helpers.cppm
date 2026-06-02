module;

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.utils.tool_helpers;

import cc.utils.json;

export namespace cc::utils {

using cc::utils::json::JsonVal;

// ─── Tool Error Formatting ───────────────────────────────────────────────────

/// Format an error into a human-readable string (with truncation for large errors)
std::string format_tool_error(std::string_view error_message);

/// Format an error with exit code and stdout/stderr parts
std::string format_shell_error(int exit_code, std::string_view stderr_out, std::string_view stdout_out, bool interrupted);

/// Get the constituent error parts from an error
std::vector<std::string> get_error_parts(std::string_view message, std::string_view stderr_out, std::string_view stdout_out);

/// Format a validation path (e.g., ["todos", 0, "activeForm"] => "todos[0].activeForm")
std::string format_validation_path(const std::vector<std::string>& path);

/// Format validation errors into a human-readable error message
std::string format_validation_errors(const std::vector<std::pair<std::vector<std::string>, std::string>>& errors);

// ─── Tool Pool Management ────────────────────────────────────────────────────

/// Filter mode for tool pools
enum class ToolFilterMode {
    normal,
    coordinator
};

/// Tool permission context mode
enum class ToolPermissionMode {
    normal,
    auto_mode,
    plan_mode
};

/// Minimal tool info for pool management
struct ToolInfo {
    std::string name;
    std::string description;
    bool should_defer = false;
    bool is_mcp_tool = false;
};

/// Check if a tool name matches a PR activity subscription tool
bool is_pr_activity_subscription_tool(std::string_view name);

/// Apply coordinator tool filter to a tool list
std::vector<ToolInfo> apply_coordinator_tool_filter(const std::vector<ToolInfo>& tools);

/// Merge and filter tools from multiple sources, applying coordinator filtering if needed
std::vector<ToolInfo> merge_and_filter_tools(
    const std::vector<ToolInfo>& initial_tools,
    const std::vector<ToolInfo>& assembled,
    ToolPermissionMode mode);

// ─── Tool Result Storage ─────────────────────────────────────────────────────

/// Subdirectory name for tool results within a session
inline constexpr std::string_view TOOL_RESULTS_SUBDIR = "tool-results";

/// XML tag used to wrap persisted output messages
inline constexpr std::string_view PERSISTED_OUTPUT_TAG = "<persisted-output>";
inline constexpr std::string_view PERSISTED_OUTPUT_CLOSING_TAG = "</persisted-output>";

/// Message used when tool result content was cleared without persisting
inline constexpr std::string_view TOOL_RESULT_CLEARED_MESSAGE = "[Old tool result content cleared]";

/// Resolve the effective persistence threshold for a tool
size_t get_persistence_threshold(std::string_view tool_name, size_t declared_max_result_size_chars);

/// Persist a tool result to disk if it exceeds the threshold.
/// Returns the path to the persisted file, or empty if not persisted.
std::expected<std::optional<std::string>, std::string> persist_tool_result(
    std::string_view tool_name,
    std::string_view content,
    std::string_view session_id);

/// Read a persisted tool result back from disk
std::expected<std::string, std::string> read_persisted_result(std::string_view path);

// ─── Tool Schema Cache ───────────────────────────────────────────────────────

/// Cached tool schema entry
struct CachedToolSchema {
    std::string name;
    std::string description;
    JsonVal input_schema;    // JSON schema for tool input
    bool strict = false;
    bool eager_input_streaming = false;
};

/// Get the tool schema cache (session-scoped)
std::unordered_map<std::string, CachedToolSchema>& get_tool_schema_cache();

/// Clear the tool schema cache
void clear_tool_schema_cache();

// ─── Tool Search / Discovery ─────────────────────────────────────────────────

/// Default percentage of context window at which to auto-enable tool search
inline constexpr int DEFAULT_AUTO_TOOL_SEARCH_PERCENTAGE = 10;

/// Check if tool search should be enabled based on configuration and tool count
bool should_enable_tool_search(
    size_t mcp_tool_token_count,
    size_t context_window_size);

/// Check if a tool is deferred (loaded on demand via ToolSearch)
bool is_deferred_tool(const ToolInfo& tool);

/// Format a deferred tool line for the tool search listing
std::string format_deferred_tool_line(const ToolInfo& tool);

// ─── Render Tool Lookup ──────────────────────────────────────────────────────

/// Resolve a tool for UI rendering, even when runtime filtering hides it.
/// Checks active tools, then REPL primitive tools, then Script primitive tools.
std::optional<ToolInfo> find_renderable_tool_by_name(
    const std::vector<ToolInfo>& tools,
    std::string_view name);

// ─── Embedded Tools ──────────────────────────────────────────────────────────

/// Whether this build has bfs/ugrep embedded in the binary.
/// When true: find/grep are shadowed by shell functions, and dedicated
/// Glob/Grep tools are removed from the tool registry.
bool has_embedded_search_tools();

/// Path to the binary that contains the embedded search tools
std::string embedded_search_tools_binary_path();

} // namespace cc::utils
