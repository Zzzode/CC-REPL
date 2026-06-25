module;
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permission_views;

import cc.ui.layout;

export namespace cc::ui::permissions {

// --- Permission decision types ---
enum class PermissionDecision {
    Allow,
    AllowAlways,
    Deny,
    DenyAlways,
    EditAndAllow
};

// --- Tool types that require permission ---
enum class ToolType {
    FileEdit,
    FileWrite,
    FileRead,
    Bash,
    PowerShell,
    WebFetch,
    AskUserQuestion,
    Glob,
    Grep,
    NotebookEdit,
    Skill,
    EnterPlanMode,
    ExitPlanMode,
    ReviewArtifact,
    Workflow,
    Monitor,
    Unknown
};

// Convert tool type to display name
[[nodiscard]] inline auto tool_type_display_name(ToolType type) -> std::string_view {
    switch (type) {
        case ToolType::FileEdit:         return "File Edit";
        case ToolType::FileWrite:        return "File Write";
        case ToolType::FileRead:         return "File Read";
        case ToolType::Bash:             return "Bash Command";
        case ToolType::PowerShell:       return "PowerShell Command";
        case ToolType::WebFetch:         return "Web Fetch";
        case ToolType::AskUserQuestion:  return "Ask User Question";
        case ToolType::Glob:             return "Glob Search";
        case ToolType::Grep:             return "Grep Search";
        case ToolType::NotebookEdit:     return "Notebook Edit";
        case ToolType::Skill:            return "Skill Execution";
        case ToolType::EnterPlanMode:    return "Enter Plan Mode";
        case ToolType::ExitPlanMode:     return "Exit Plan Mode";
        case ToolType::ReviewArtifact:   return "Review Artifact";
        case ToolType::Workflow:         return "Workflow Script";
        case ToolType::Monitor:          return "Monitor Tool";
        case ToolType::Unknown:          return "Unknown Tool";
    }
    return "Unknown Tool";
}

// --- Tool-specific metadata ---
struct BashCommandInfo {
    std::string command;
    std::optional<std::string> working_directory;
    std::optional<int> timeout_seconds;
};

struct FileEditInfo {
    std::string file_path;
    std::string old_string;
    std::string new_string;
};

struct FileWriteInfo {
    std::string file_path;
    std::string content;
};

struct WebFetchInfo {
    std::string url;
    std::optional<std::string> prompt;
};

struct SkillInfo {
    std::string skill_name;
    std::optional<std::string> description;
};

struct PowerShellInfo {
    std::string command;
    std::optional<std::string> working_directory;
};

struct AskUserQuestionInfo {
    std::string question;
    std::vector<std::string> options;
};

// Unified tool metadata variant
using ToolMetadata = std::variant<
    BashCommandInfo,
    FileEditInfo,
    FileWriteInfo,
    WebFetchInfo,
    SkillInfo,
    PowerShellInfo,
    AskUserQuestionInfo,
    std::string  // fallback: raw JSON/description for unknown tools
>;

// --- Permission request props ---
struct PermissionRequestProps {
    ToolType tool_type;
    ToolMetadata metadata;
    std::function<void(PermissionDecision)> on_done;
};

// --- Worker badge (for teammate/agent permission requests) ---
struct WorkerBadgeProps {
    std::string agent_name;
    std::string agent_color;  // color name from palette
};

// --- Permission routing ---

// Determine which tool type a tool name maps to
[[nodiscard]] inline auto classify_tool(std::string_view tool_name) -> ToolType {
    if (tool_name == "FileEditTool" || tool_name == "file_edit") return ToolType::FileEdit;
    if (tool_name == "FileWriteTool" || tool_name == "file_write") return ToolType::FileWrite;
    if (tool_name == "FileReadTool" || tool_name == "file_read") return ToolType::FileRead;
    if (tool_name == "BashTool" || tool_name == "bash") return ToolType::Bash;
    if (tool_name == "PowerShellTool" || tool_name == "powershell") return ToolType::PowerShell;
    if (tool_name == "WebFetchTool" || tool_name == "web_fetch") return ToolType::WebFetch;
    if (tool_name == "AskUserQuestionTool" || tool_name == "ask_user") return ToolType::AskUserQuestion;
    if (tool_name == "GlobTool" || tool_name == "glob") return ToolType::Glob;
    if (tool_name == "GrepTool" || tool_name == "grep") return ToolType::Grep;
    if (tool_name == "NotebookEditTool" || tool_name == "notebook_edit") return ToolType::NotebookEdit;
    if (tool_name == "SkillTool" || tool_name == "skill") return ToolType::Skill;
    if (tool_name == "EnterPlanModeTool" || tool_name == "enter_plan_mode") return ToolType::EnterPlanMode;
    if (tool_name == "ExitPlanModeV2Tool" || tool_name == "exit_plan_mode") return ToolType::ExitPlanMode;
    if (tool_name == "ReviewArtifactTool" || tool_name == "review_artifact") return ToolType::ReviewArtifact;
    if (tool_name == "WorkflowTool" || tool_name == "workflow") return ToolType::Workflow;
    if (tool_name == "MonitorTool" || tool_name == "monitor") return ToolType::Monitor;
    return ToolType::Unknown;
}

// --- Rendering helpers ---

// Render a bash command preview with syntax highlighting
[[nodiscard]] inline auto render_bash_command_preview(const BashCommandInfo& info,
                                                      int terminal_width) -> std::string {
    std::string result;
    result += "\033[1;33m$ \033[0m";
    auto cmd = info.command;
    int max_cmd_len = terminal_width - 4;
    if (static_cast<int>(cmd.size()) > max_cmd_len && max_cmd_len > 3) {
        cmd = cmd.substr(0, static_cast<std::size_t>(max_cmd_len - 3)) + "...";
    }
    result += cmd;
    if (info.working_directory.has_value()) {
        result += "\n\033[2m  in: " + *info.working_directory + "\033[0m";
    }
    return result;
}

// Render a file edit preview showing old/new strings
[[nodiscard]] inline auto render_file_edit_preview(const FileEditInfo& info,
                                                    int) -> std::string {
    std::string result;
    result += "\033[1m" + info.file_path + "\033[0m\n";
    result += "\033[31m- " + info.old_string.substr(0, 80) + "\033[0m\n";
    result += "\033[32m+ " + info.new_string.substr(0, 80) + "\033[0m";
    return result;
}

// Render a file write preview
[[nodiscard]] inline auto render_file_write_preview(const FileWriteInfo& info,
                                                     int) -> std::string {
    std::string result;
    result += "\033[1m" + info.file_path + "\033[0m\n";
    auto preview = info.content.substr(0, 200);
    auto newlines = std::count(preview.begin(), preview.end(), '\n');
    result += "\033[2m(" + std::to_string(info.content.size()) + " bytes, ~"
           + std::to_string(newlines + 1) + " lines)\033[0m";
    return result;
}

// Render the full permission request view
[[nodiscard]] inline auto render_permission_request(const PermissionRequestProps& props,
                                                     int terminal_width)
    -> std::expected<std::string, std::string> {
    std::string result;

    // Header with tool name
    result += "\033[1;33m⚠ Permission Request: \033[0m";
    result += "\033[1m" + std::string(tool_type_display_name(props.tool_type)) + "\033[0m\n\n";

    // Tool-specific content
    if (auto* bash = std::get_if<BashCommandInfo>(&props.metadata)) {
        result += render_bash_command_preview(*bash, terminal_width);
    } else if (auto* edit = std::get_if<FileEditInfo>(&props.metadata)) {
        result += render_file_edit_preview(*edit, terminal_width);
    } else if (auto* write = std::get_if<FileWriteInfo>(&props.metadata)) {
        result += render_file_write_preview(*write, terminal_width);
    } else if (auto* fetch = std::get_if<WebFetchInfo>(&props.metadata)) {
        result += "\033[4m" + fetch->url + "\033[0m";
    } else if (auto* skill = std::get_if<SkillInfo>(&props.metadata)) {
        result += "\033[36m" + skill->skill_name + "\033[0m";
        if (skill->description.has_value()) {
            result += "\n\033[2m" + *skill->description + "\033[0m";
        }
    } else if (auto* ps = std::get_if<PowerShellInfo>(&props.metadata)) {
        result += "\033[1;34mPS> \033[0m" + ps->command;
    } else if (auto* ask = std::get_if<AskUserQuestionInfo>(&props.metadata)) {
        result += ask->question;
    } else if (auto* fallback = std::get_if<std::string>(&props.metadata)) {
        result += *fallback;
    }

    // Action hints
    result += "\n\n\033[2m[y] Allow  [n] Deny  [a] Always allow  [d] Always deny\033[0m";

    return result;
}

// --- FTXUI Component factories ---

// Create an interactive FTXUI Component for the permission request dialog
[[nodiscard]] auto make_permission_request_component(
    PermissionRequestProps props,
    int terminal_width) -> ftxui::Component;

// Create a permission request component with a worker badge
[[nodiscard]] auto make_worker_permission_component(
    PermissionRequestProps props,
    WorkerBadgeProps badge,
    int terminal_width) -> ftxui::Component;

} // namespace cc::ui::permissions
