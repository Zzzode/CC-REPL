// WorktreeTool - Git worktree creation and management for isolated workspaces
module;
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module cc.tools.worktree;


export namespace cc::tools {

// Error types for worktree operations
enum class WorktreeError {
    BranchNameEmpty,
    PathEmpty,
    GitNotAvailable,
    WorktreeCreateFailed,
    WorktreeDeleteFailed,
    WorktreeNotFound,
    AlreadyInWorktree,
    NotInWorktree,
    DirectorySwitchFailed,
    CleanupFailed,
    BranchAlreadyExists,
};

constexpr auto format_error(WorktreeError err) -> std::string_view {
    switch (err) {
        case WorktreeError::BranchNameEmpty:      return "Branch name is empty";
        case WorktreeError::PathEmpty:            return "Worktree path is empty";
        case WorktreeError::GitNotAvailable:      return "Git is not available";
        case WorktreeError::WorktreeCreateFailed: return "Failed to create git worktree";
        case WorktreeError::WorktreeDeleteFailed: return "Failed to delete git worktree";
        case WorktreeError::WorktreeNotFound:     return "Worktree not found";
        case WorktreeError::AlreadyInWorktree:    return "Already in a worktree session";
        case WorktreeError::NotInWorktree:        return "Not currently in a worktree session";
        case WorktreeError::DirectorySwitchFailed:return "Failed to switch working directory";
        case WorktreeError::CleanupFailed:        return "Failed to clean up worktree resources";
        case WorktreeError::BranchAlreadyExists:  return "Branch already exists";
        default:                                  return "Unknown worktree error";
    }
}

// Worktree information
struct WorktreeInfo {
    std::string branch_name;
    std::filesystem::path worktree_path;
    std::filesystem::path original_path;
    std::string base_commit;
    bool is_active{false};
    std::chrono::steady_clock::time_point created_at;
};

// Worktree creation request
struct WorktreeCreateRequest {
    std::string branch_name;
    std::optional<std::filesystem::path> target_path;  // Auto-generated if not specified
    std::optional<std::string> base_branch;            // Default: current branch
    bool create_branch{true};                          // Create new branch or use existing
};

// Worktree session state
class WorktreeState {
public:
    static WorktreeState& instance() {
        static WorktreeState state;
        return state;
    }

    [[nodiscard]] bool is_in_worktree() const { return current_.has_value(); }
    [[nodiscard]] const WorktreeInfo* current() const {
        return current_ ? &*current_ : nullptr;
    }

    auto enter(WorktreeInfo info) -> std::expected<void, WorktreeError> {
        if (current_) return std::unexpected(WorktreeError::AlreadyInWorktree);
        current_ = std::move(info);
        current_->is_active = true;
        return {};
    }

    auto exit() -> std::expected<WorktreeInfo, WorktreeError> {
        if (!current_) return std::unexpected(WorktreeError::NotInWorktree);
        current_->is_active = false;
        auto info = std::move(*current_);
        current_ = std::nullopt;
        return info;
    }

private:
    WorktreeState() = default;
    std::optional<WorktreeInfo> current_;
};

// EnterWorktreeTool - creates a git worktree and switches to it
class EnterWorktreeTool {
public:
    static constexpr std::string_view name = "enter_worktree";
    static constexpr std::string_view description = "Create a git worktree for isolated feature work";

    // Generate a default worktree path based on branch name
    static auto default_worktree_path(const std::filesystem::path& repo_root,
                                       const std::string& branch_name) -> std::filesystem::path
    {
        // Place worktrees in a sibling directory
        auto parent = repo_root.parent_path();
        auto dir_name = std::format("{}-worktree-{}", repo_root.filename().string(), branch_name);
        return parent / dir_name;
    }

    // Check if git is available
    static auto check_git_available() -> std::expected<void, WorktreeError> {
        int result = std::system("git --version > /dev/null 2>&1");
        if (result != 0) return std::unexpected(WorktreeError::GitNotAvailable);
        return {};
    }

    // Execute worktree creation
    auto execute(WorktreeCreateRequest request) -> std::expected<WorktreeInfo, WorktreeError> {
        if (request.branch_name.empty()) {
            return std::unexpected(WorktreeError::BranchNameEmpty);
        }

        if (auto git = check_git_available(); !git) {
            return std::unexpected(git.error());
        }

        if (WorktreeState::instance().is_in_worktree()) {
            return std::unexpected(WorktreeError::AlreadyInWorktree);
        }

        // Determine worktree path
        auto original_path = std::filesystem::current_path();
        auto worktree_path = request.target_path.value_or(
            default_worktree_path(original_path, request.branch_name));

        // Build git worktree command
        std::string cmd;
        if (request.create_branch) {
            cmd = std::format("git worktree add -b {} {} 2>&1",
                              request.branch_name, worktree_path.string());
        } else {
            cmd = std::format("git worktree add {} {} 2>&1",
                              worktree_path.string(), request.branch_name);
        }

        int result = std::system(cmd.c_str());
        if (result != 0) {
            return std::unexpected(WorktreeError::WorktreeCreateFailed);
        }

        // Switch working directory
        std::error_code ec;
        std::filesystem::current_path(worktree_path, ec);
        if (ec) {
            return std::unexpected(WorktreeError::DirectorySwitchFailed);
        }

        WorktreeInfo info{
            .branch_name = request.branch_name,
            .worktree_path = worktree_path,
            .original_path = original_path,
            .base_commit = request.base_branch.value_or("HEAD"),
            .is_active = true,
            .created_at = std::chrono::steady_clock::now(),
        };

        if (auto enter = WorktreeState::instance().enter(info); !enter) {
            return std::unexpected(enter.error());
        }

        return info;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "branch_name": {{ "type": "string", "description": "Name for the new branch" }},
      "target_path": {{ "type": "string", "description": "Path for the worktree directory" }},
      "base_branch": {{ "type": "string", "description": "Base branch to create from" }},
      "create_branch": {{ "type": "boolean", "description": "Create a new branch (default true)" }}
    }},
    "required": ["branch_name"]
  }}
}})json", name, description);
    }
};

// ExitWorktreeTool - cleans up and exits the worktree session
class ExitWorktreeTool {
public:
    static constexpr std::string_view name = "exit_worktree";
    static constexpr std::string_view description = "Exit the current worktree and return to the original directory";

    auto execute(bool remove_worktree = false) -> std::expected<WorktreeInfo, WorktreeError> {
        auto exit_result = WorktreeState::instance().exit();
        if (!exit_result) return std::unexpected(exit_result.error());

        auto info = *exit_result;

        // Switch back to original directory
        std::error_code ec;
        std::filesystem::current_path(info.original_path, ec);
        if (ec) {
            return std::unexpected(WorktreeError::DirectorySwitchFailed);
        }

        // Optionally remove the worktree
        if (remove_worktree) {
            auto cmd = std::format("git worktree remove {} 2>&1", info.worktree_path.string());
            int result = std::system(cmd.c_str());
            if (result != 0) {
                // Force removal
                auto force_cmd = std::format("git worktree remove --force {} 2>&1",
                                             info.worktree_path.string());
                std::system(force_cmd.c_str());
            }
        }

        return info;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "remove_worktree": {{ "type": "boolean", "description": "Remove the worktree directory (default false)" }}
    }}
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
