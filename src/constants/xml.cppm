/// @file xml.cppm
/// @brief XML tag name constants used in message protocol.
/// Migrated from src/constants/xml.ts
module;

#include <string_view>
#include <array>

export module cc.constants.xml;

export namespace cc::constants::xml {

// Skill/command metadata tags
inline constexpr std::string_view COMMAND_NAME_TAG = "command-name";
inline constexpr std::string_view COMMAND_MESSAGE_TAG = "command-message";
inline constexpr std::string_view COMMAND_ARGS_TAG = "command-args";

// Terminal/bash tags
inline constexpr std::string_view BASH_INPUT_TAG = "bash-input";
inline constexpr std::string_view BASH_STDOUT_TAG = "bash-stdout";
inline constexpr std::string_view BASH_STDERR_TAG = "bash-stderr";
inline constexpr std::string_view LOCAL_COMMAND_STDOUT_TAG = "local-command-stdout";
inline constexpr std::string_view LOCAL_COMMAND_STDERR_TAG = "local-command-stderr";
inline constexpr std::string_view LOCAL_COMMAND_CAVEAT_TAG = "local-command-caveat";

/// All terminal-related tags
inline constexpr std::array<std::string_view, 6> TERMINAL_OUTPUT_TAGS = {
    BASH_INPUT_TAG, BASH_STDOUT_TAG, BASH_STDERR_TAG,
    LOCAL_COMMAND_STDOUT_TAG, LOCAL_COMMAND_STDERR_TAG, LOCAL_COMMAND_CAVEAT_TAG,
};

inline constexpr std::string_view TICK_TAG = "tick";

// Task notification tags
inline constexpr std::string_view TASK_NOTIFICATION_TAG = "task-notification";
inline constexpr std::string_view TASK_ID_TAG = "task-id";
inline constexpr std::string_view TOOL_USE_ID_TAG = "tool-use-id";
inline constexpr std::string_view TASK_TYPE_TAG = "task-type";
inline constexpr std::string_view OUTPUT_FILE_TAG = "output-file";
inline constexpr std::string_view STATUS_TAG = "status";
inline constexpr std::string_view SUMMARY_TAG = "summary";
inline constexpr std::string_view REASON_TAG = "reason";
inline constexpr std::string_view WORKTREE_TAG = "worktree";
inline constexpr std::string_view WORKTREE_PATH_TAG = "worktreePath";
inline constexpr std::string_view WORKTREE_BRANCH_TAG = "worktreeBranch";

// Ultraplan/review tags
inline constexpr std::string_view ULTRAPLAN_TAG = "ultraplan";
inline constexpr std::string_view REMOTE_REVIEW_TAG = "remote-review";
inline constexpr std::string_view REMOTE_REVIEW_PROGRESS_TAG = "remote-review-progress";

// Inter-agent communication tags
inline constexpr std::string_view TEAMMATE_MESSAGE_TAG = "teammate-message";
inline constexpr std::string_view CHANNEL_MESSAGE_TAG = "channel-message";
inline constexpr std::string_view CHANNEL_TAG = "channel";
inline constexpr std::string_view CROSS_SESSION_MESSAGE_TAG = "cross-session-message";

// Fork tags
inline constexpr std::string_view FORK_BOILERPLATE_TAG = "fork-boilerplate";
inline constexpr std::string_view FORK_DIRECTIVE_PREFIX = "Your directive: ";

// Common slash command argument patterns
inline constexpr std::array<std::string_view, 3> COMMON_HELP_ARGS = {"help", "-h", "--help"};
inline constexpr std::array<std::string_view, 13> COMMON_INFO_ARGS = {
    "list", "show", "display", "current", "view", "get", "check",
    "describe", "print", "version", "about", "status", "?",
};

} // namespace cc::constants::xml
