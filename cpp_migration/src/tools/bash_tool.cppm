// BashTool - Executes shell commands with process lifecycle management
module;

#include <array>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.tools.bash;

import cc.utils.process;
import cc.utils.error;
import cc.utils.async;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::bash {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::process::ProcessOptions;
using cc::utils::process::ProcessResult;
using cc::utils::async::EventLoop;
using cc::utils::async::Task;
using cc::utils::Result;

// =========================================================================
// Bash Tool Configuration and Types
// =========================================================================

/// Dangerous command patterns that require explicit user approval
constexpr std::array<std::string_view, 10> kDangerousPatterns = {
    "rm -rf",
    "mkfs",
    "dd if=",
    "> /dev/sd",
    "chmod -R 777",
    ":(){ :|:& };:",
    "shutdown",
    "reboot",
    "init 0",
    "git push --force"
};

/// Command types for classification and UI display
enum class CommandType {
    ReadOnly,   // cat, ls, grep (read-only operations)
    Write,      // echo >, sed -i (modify files)
    Execute,    // npm, make, cargo (execute programs)
    Dangerous,  // rm -rf, format (potentially destructive)
    Unknown
};

/// Input parameters for BashTool
struct BashToolInput {
    std::string command;
    std::optional<std::string> cwd;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::string> description;
    bool run_in_background = false;
    bool dangerously_disable_sandbox = false;

    /// Parse from JSON using yyjson for proper escape handling
    static std::expected<BashToolInput, std::string> from_json(std::string_view json) {
        using namespace cc::utils::json;
        auto doc = parse(json);
        if (!doc) {
            return std::unexpected("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return std::unexpected("Expected JSON object");
        }

        BashToolInput input;

        // Extract command (required)
        auto cmd_node = root.get("command");
        if (!cmd_node.is_str()) {
            return std::unexpected("Missing 'command' field");
        }
        input.command = std::string(cmd_node.as_str());

        // Extract cwd (optional)
        auto cwd_node = root.get("cwd");
        if (cwd_node.is_str()) {
            input.cwd = std::string(cwd_node.as_str());
        }

        // Extract timeout (optional)
        auto timeout_node = root.get("timeout");
        if (timeout_node.is_num()) {
            input.timeout = std::chrono::milliseconds(static_cast<int64_t>(timeout_node.as_int()));
        }

        // Extract description (optional)
        auto desc_node = root.get("description");
        if (desc_node.is_str()) {
            input.description = std::string(desc_node.as_str());
        }

        // Extract run_in_background (optional)
        auto bg_node = root.get("run_in_background");
        if (bg_node.is_bool()) {
            input.run_in_background = bg_node.as_bool();
        }

        // Extract dangerously_disable_sandbox (optional)
        auto sandbox_node = root.get("dangerously_disable_sandbox");
        if (sandbox_node.is_bool()) {
            input.dangerously_disable_sandbox = sandbox_node.as_bool();
        }

        if (input.command.empty()) {
            return std::unexpected("Missing 'command' field");
        }

        return input;
    }
};

/// Output from BashTool execution
struct BashToolOutput {
    std::string stdout;
    std::string stderr;
    int exit_code = 0;
    bool interrupted = false;
    bool is_image = false;
    std::optional<std::string> background_task_id;
    std::optional<std::string> return_code_interpretation;
    bool no_output_expected = false;
    std::optional<std::string> persisted_output_path;
};

// =========================================================================
// Command Classification and Validation
// =========================================================================

/// Classify a command by type
[[nodiscard]] CommandType classify_command(std::string_view command) noexcept {
    std::string_view base_cmd = command;
    auto space = command.find(' ');
    if (space != std::string::npos) {
        base_cmd = command.substr(0, space);
    }
    
    // Read-only commands
    static const std::unordered_set<std::string_view> kReadCmds = {
        "cat", "head", "tail", "less", "more", "wc", "stat", "file", "strings",
        "ls", "tree", "du", "find", "grep", "rg", "ag", "ack", "locate", "which", "whereis"
    };
    if (kReadCmds.contains(base_cmd)) {
        return CommandType::ReadOnly;
    }
    
    // Write commands
    static const std::unordered_set<std::string_view> kWriteCmds = {
        "echo", "tee", "sed", "awk", "touch", "mkdir", "rmdir", "cp", "mv", "ln"
    };
    if (kWriteCmds.contains(base_cmd)) {
        return CommandType::Write;
    }
    
    // Check for dangerous patterns
    for (auto pattern : kDangerousPatterns) {
        if (command.find(pattern) != std::string::npos) {
            return CommandType::Dangerous;
        }
    }
    
    return CommandType::Execute;
}

/// Check if command requires sandbox
[[nodiscard]] bool should_use_sandbox(std::string_view command) noexcept {
    auto type = classify_command(command);
    return type == CommandType::Dangerous || type == CommandType::Execute;
}

/// Check if command is silent (expected no output)
[[nodiscard]] bool is_silent_command(std::string_view command) noexcept {
    static const std::unordered_set<std::string_view> kSilentCmds = {
        "mv", "cp", "rm", "mkdir", "rmdir", "chmod", "chown", "chgrp",
        "touch", "ln", "cd", "export", "unset", "wait"
    };
    
    auto space = command.find(' ');
    auto base_cmd = space != std::string::npos ? command.substr(0, space) : command;
    return kSilentCmds.contains(base_cmd);
}

// =========================================================================
// BashTool Implementation
// =========================================================================

/// BashTool - Executes shell commands with safety checks
class BashTool {
public:
    static constexpr std::string_view kName = "Bash";
    static constexpr std::string_view kDescription = 
        "Execute shell commands in a terminal session. Use for running CLI tools, "
        "installing dependencies, or executing build commands.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "command",
                        .type = "string",
                        .description = "The shell command to execute",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "cwd",
                        .type = "string",
                        .description = "Working directory for execution (optional)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "description",
                        .type = "string",
                        .description = "Clear description of what this command does",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "timeout",
                        .type = "number",
                        .description = "Timeout in milliseconds (default: 120000)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "run_in_background",
                        .type = "boolean",
                        .description = "Run command in background (optional)",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::Execute,
            .category = "execution"
        };
    }
    
    explicit BashTool(EventLoop& loop = EventLoop::default_loop())
        : loop_(loop) {}
    
    /// Permission mode for the tool
    enum class PermissionMode {
        Ask,        // Always ask user for confirmation
        AutoAllow,  // Auto-allow safe commands, ask for dangerous
        YoloMode    // Allow everything without asking
    };
    
    void set_permission_mode(PermissionMode mode) { permission_mode_ = mode; }
    void set_allowed_directories(std::vector<std::string> dirs) { 
        allowed_directories_ = std::move(dirs); 
    }
    
    /// Check if execution is allowed based on permission mode and command type
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        auto parsed = BashToolInput::from_json(input.json());
        if (!parsed) return false;
        
        auto cmd_type = classify_command(parsed->command);
        
        switch (permission_mode_) {
            case PermissionMode::YoloMode:
                return true;
            case PermissionMode::AutoAllow:
                // Auto-allow read-only and simple write commands
                // Block dangerous commands (require external approval)
                return cmd_type != CommandType::Dangerous;
            case PermissionMode::Ask:
            default:
                // In ask mode, only auto-allow read-only
                return cmd_type == CommandType::ReadOnly;
        }
    }
    
    /// Check if command is dangerous and needs explicit confirmation
    [[nodiscard]] bool requires_confirmation(std::string_view command) const {
        auto cmd_type = classify_command(command);
        if (permission_mode_ == PermissionMode::YoloMode) return false;
        return cmd_type == CommandType::Dangerous;
    }
    
    /// Validate working directory is within allowed paths
    [[nodiscard]] bool is_directory_allowed(std::string_view cwd) const {
        if (allowed_directories_.empty()) return true;  // No restriction
        for (const auto& dir : allowed_directories_) {
            if (cwd.starts_with(dir)) return true;
        }
        return false;
    }
    
    /// Execute a command synchronously
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_input = BashToolInput::from_json(input.json());
        if (!parsed_input) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        return execute_internal(*parsed_input);
    }
    
    /// Execute command asynchronously (coroutine)
    [[nodiscard]] Task<Result<ToolResult>> execute_async(const ToolInput& input) {
        auto parsed_input = BashToolInput::from_json(input.json());
        if (!parsed_input) {
            co_return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        co_return co_await execute_async_internal(*parsed_input);
    }
    
private:
    EventLoop& loop_;
    PermissionMode permission_mode_ = PermissionMode::AutoAllow;
    std::vector<std::string> allowed_directories_;
    std::unordered_map<std::string, std::shared_ptr<ProcessResult>> background_tasks_;
    
    /// Internal synchronous execution
    Result<ToolResult> execute_internal(const BashToolInput& input) {
        try {
            // Check directory permissions
            if (input.cwd && !is_directory_allowed(*input.cwd)) {
                return ToolResult::error(std::format(
                    "Directory '{}' is not in the allowed directories list", *input.cwd));
            }
            
            // Check if command requires confirmation
            if (requires_confirmation(input.command)) {
                return ToolResult::error(std::format(
                    "Command requires user confirmation: {}", input.command));
            }
            
            ProcessOptions opts;
            opts.command = "/bin/sh";
            opts.args = {"-c", input.command};
            opts.cwd = input.cwd.value_or("");
            opts.timeout = input.timeout.value_or(std::chrono::seconds(120));
            
            // Execute via popen: use "sh -c <command>" (single-layer wrapping)
            std::string shell_cmd = input.command;
            std::string stdout_data;
            std::string stderr_data;
            int exit_code = 0;
            
            // Change to working directory if specified
            std::string full_cmd;
            if (input.cwd && !input.cwd->empty()) {
                full_cmd = std::format("cd {} && {}", *input.cwd, shell_cmd);
            } else {
                full_cmd = shell_cmd;
            }
            
            FILE* pipe = popen(full_cmd.c_str(), "r");
            if (!pipe) {
                return ToolResult::error(std::format("Failed to execute command: {}", input.command));
            }
            
            std::array<char, 4096> buffer;
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                stdout_data += buffer.data();
            }
            
            exit_code = pclose(pipe);
            exit_code = WEXITSTATUS(exit_code);
            
            // Truncate output if too long (30000 char limit)
            constexpr size_t kMaxOutput = 30000;
            if (stdout_data.size() > kMaxOutput) {
                constexpr size_t kHalf = kMaxOutput / 2;
                stdout_data = stdout_data.substr(0, kHalf) 
                    + "\n\n... [output truncated, " 
                    + std::to_string(stdout_data.size() - kMaxOutput)
                    + " bytes omitted] ...\n\n"
                    + stdout_data.substr(stdout_data.size() - kHalf);
            }
            
            BashToolOutput output{
                .stdout = stdout_data,
                .stderr = stderr_data,
                .exit_code = exit_code,
                .interrupted = false,
                .no_output_expected = is_silent_command(input.command)
            };
            
            return format_result(output);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Execution error: {}", e.what()));
        }
    }
    
    /// Internal async execution using libuv process
    Task<Result<ToolResult>> execute_async_internal(const BashToolInput& input) {
        if (input.run_in_background) {
            // Background execution not fully implemented yet
            co_return ToolResult::success("Background execution coming soon");
        }
        
        try {
            ProcessOptions opts;
            opts.command = "/bin/sh";
            opts.args = {"-c", input.command};
            opts.cwd = input.cwd.value_or("");
            opts.timeout = input.timeout.value_or(std::chrono::seconds(120));
            
            auto result = co_await cc::utils::process::spawn_process(opts, loop_);
            if (!result) {
                co_return ToolResult::error(std::format("Process execution failed: {}", 
                    result.error().message()));
            }
            
            BashToolOutput output{
                .stdout = result->stdout_data,
                .stderr = result->stderr_data,
                .exit_code = result->exit_code,
                .interrupted = result->timed_out,
                .no_output_expected = is_silent_command(input.command)
            };
            
            co_return format_result(output);
            
        } catch (const std::exception& e) {
            co_return ToolResult::error(std::format("Execution error: {}", e.what()));
        }
    }
    
    /// Format the BashToolOutput into a ToolResult
    [[nodiscard]] ToolResult format_result(const BashToolOutput& output) {
        std::string result_text;
        
        if (is_error(output)) {
            result_text = "Command failed";
            if (!output.stderr.empty()) {
                result_text += ":\n" + output.stderr;
            }
            if (!output.stdout.empty()) {
                result_text += "\nOutput:\n" + output.stdout;
            }
            result_text += std::format("\nExit code: {}", output.exit_code);
            return ToolResult::error(result_text);
        }
        
        if (!output.stdout.empty()) {
            result_text = output.stdout;
        } else if (!output.stderr.empty()) {
            result_text = output.stderr;
        } else if (output.no_output_expected) {
            result_text = "Command completed successfully";
        } else {
            result_text = "(no output)";
        }
        
        if (output.return_code_interpretation) {
            result_text += "\n\n" + *output.return_code_interpretation;
        }
        
        return ToolResult::success(result_text);
    }
    
    /// Check if output represents an error
    [[nodiscard]] bool is_error(const BashToolOutput& output) const noexcept {
        return output.exit_code != 0 && !output.interrupted;
    }
};

} // namespace cc::tools::bash

// Export main tool class
export namespace cc::tools {
    using cc::tools::bash::BashTool;

    /// Factory: create BashTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_bash_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            BashTool tool_;
            cc::core::ToolDefinition def_ = BashTool::definition();

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>();
    }
}
