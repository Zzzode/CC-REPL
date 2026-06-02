// FileWriteTool - Creates or overwrites files with safety checks
module;

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.tools.file_write;

import cc.utils.file;
import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::file_write {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;

namespace fs = std::filesystem;

// =========================================================================
// FileWriteTool Configuration and Types
// =========================================================================

/// Write mode for file operations
enum class WriteMode {
    Create,     // Create only, fail if exists
    Overwrite,  // Overwrite existing file
    Append      // Append to existing file
};

/// FileWriteTool input parameters
struct FileWriteInput {
    fs::path file_path;
    std::string content;
    WriteMode mode = WriteMode::Overwrite;
    bool create_parents = true;
    bool create_backup = true;
    bool binary_mode = false;
    std::optional<fs::perms> permissions;

    /// Parse from JSON using yyjson for proper escape handling
    static std::expected<FileWriteInput, std::string> from_json(std::string_view json) {
        using namespace cc::utils::json;
        auto doc = parse(json);
        if (!doc) {
            return std::unexpected("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return std::unexpected("Expected JSON object");
        }

        FileWriteInput input;

        // Extract file_path (required)
        auto path_node = root.get("file_path");
        if (!path_node.is_str()) {
            return std::unexpected("Missing 'file_path' field");
        }
        input.file_path = std::string(path_node.as_str());

        // Extract content (required for create/overwrite)
        auto content_node = root.get("content");
        if (content_node.is_str()) {
            input.content = std::string(content_node.as_str());
        }

        // Extract mode (optional)
        auto mode_node = root.get("mode");
        if (mode_node.is_str()) {
            auto mode_str = mode_node.as_str();
            if (mode_str == "create") input.mode = WriteMode::Create;
            else if (mode_str == "append") input.mode = WriteMode::Append;
            else input.mode = WriteMode::Overwrite;
        }

        // Extract create_parents (optional)
        auto parents_node = root.get("create_parents");
        if (parents_node.is_bool()) {
            input.create_parents = parents_node.as_bool();
        }

        // Extract create_backup (optional)
        auto backup_node = root.get("create_backup");
        if (backup_node.is_bool()) {
            input.create_backup = backup_node.as_bool();
        }

        if (input.file_path.empty()) {
            return std::unexpected("Missing 'file_path' field");
        }

        return input;
    }
};

/// FileWriteTool output result
struct FileWriteOutput {
    fs::path written_path;
    std::uint64_t bytes_written = 0;
    bool created_new = false;
    std::optional<fs::path> backup_path;
    bool parent_dirs_created = false;
};

// =========================================================================
// FileWriteTool Implementation
// =========================================================================

/// FileWriteTool - Writes files with safety checks
class FileWriteTool {
public:
    static constexpr std::string_view kName = "Write";
    static constexpr std::string_view kDescription = 
        "Write content to a file. Creates parent directories if needed.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "file_path",
                        .type = "string",
                        .description = "Absolute path to the file to write",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "content",
                        .type = "string",
                        .description = "Content to write to the file",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "mode",
                        .type = "string",
                        .description = "Write mode: create, overwrite, or append",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::Write,
            .category = "filesystem"
        };
    }
    
    FileWriteTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        // Always allow - permission checks would be implemented in production
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_input = FileWriteInput::from_json(input.json());
        if (!parsed_input) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        return execute_internal(*parsed_input);
    }
    
private:
    std::unordered_map<std::string, std::vector<fs::path>> backup_history_;
    
    /// Internal execution
    Result<ToolResult> execute_internal(const FileWriteInput& input) {
        try {
            // Validate path (basic safety check)
            if (input.file_path.empty()) {
                return ToolResult::error("Invalid empty file path");
            }
            
            FileWriteOutput output;
            output.written_path = input.file_path;
            
            // Check if file exists
            bool file_exists = fs::exists(input.file_path);
            output.created_new = !file_exists;
            
            // Validate mode
            if (input.mode == WriteMode::Create && file_exists) {
                return ToolResult::error(
                    std::format("File already exists: {}", input.file_path.string())
                );
            }
            
            // Create parent directories if needed
            if (input.create_parents) {
                auto parent = input.file_path.parent_path();
                if (!parent.empty() && !fs::exists(parent)) {
                    fs::create_directories(parent);
                    output.parent_dirs_created = true;
                }
            }
            
            // Create backup if needed
            if (input.create_backup && file_exists && input.mode == WriteMode::Overwrite) {
                auto backup_path = create_backup(input.file_path);
                if (backup_path) {
                    output.backup_path = *backup_path;
                }
            }
            
            // Write the file
            auto open_mode = std::ios::out;
            if (input.binary_mode) {
                open_mode |= std::ios::binary;
            }
            if (input.mode == WriteMode::Append) {
                open_mode |= std::ios::app;
            } else {
                open_mode |= std::ios::trunc;
            }
            
            std::ofstream file(input.file_path, open_mode);
            if (!file) {
                return ToolResult::error(
                    std::format("Failed to open file for writing: {}", input.file_path.string())
                );
            }
            
            file << input.content;
            file.close();
            
            if (!file.good()) {
                return ToolResult::error(
                    std::format("Failed to write to file: {}", input.file_path.string())
                );
            }
            
            // Set permissions if specified
            if (input.permissions) {
                fs::permissions(input.file_path, *input.permissions);
            }
            
            output.bytes_written = input.content.size();
            
            return format_result(output);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Write error: {}", e.what()));
        }
    }
    
    /// Create backup of file
    std::optional<fs::path> create_backup(const fs::path& path) {
        try {
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
            
            auto backup_path = std::format("{}.bak.{}", path.string(), millis);
            
            fs::copy_file(path, backup_path, fs::copy_options::overwrite_existing);
            
            // Store in history
            auto& history = backup_history_[path.string()];
            history.push_back(backup_path);
            
            // Keep only last 5 backups
            while (history.size() > 5) {
                auto old_backup = history.front();
                history.erase(history.begin());
                try {
                    fs::remove(old_backup);
                } catch (...) {}
            }
            
            return backup_path;
        } catch (...) {
            return std::nullopt;
        }
    }
    
    /// Format output result
    ToolResult format_result(const FileWriteOutput& output) {
        std::string result;
        
        if (output.created_new) {
            result = std::format("Created new file: {} ({} bytes)", 
                output.written_path.string(), output.bytes_written);
        } else {
            result = std::format("Overwrote file: {} ({} bytes)", 
                output.written_path.string(), output.bytes_written);
        }
        
        if (output.parent_dirs_created) {
            result += "\n(Parent directories created)";
        }
        
        if (output.backup_path) {
            result += std::format("\nBackup created at: {}", output.backup_path->string());
        }
        
        return ToolResult::success(result);
    }
};

} // namespace cc::tools::file_write

// Export main tool class
export namespace cc::tools {
    using cc::tools::file_write::FileWriteTool;

    /// Factory: create FileWriteTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_file_write_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            FileWriteTool tool_;
            cc::core::ToolDefinition def_ = FileWriteTool::definition();

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
