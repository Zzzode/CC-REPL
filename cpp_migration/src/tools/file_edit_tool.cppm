// FileEditTool - Edits files using string replacement or patch
module;

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <regex>

export module cc.tools.file_edit;

import cc.utils.file;
import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.sed_edit_parser;
import cc.tools.sed_validation;

export namespace cc::tools::file_edit {

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
// FileEditTool Configuration and Types
// =========================================================================

/// Edit mode: replace old_string with new_string
struct StringReplaceEdit {
    std::string old_string;
    std::string new_string;
};

/// Single edit operation
struct EditOperation {
    enum class Type { Replace, ReplaceAll, Insert, Delete };
    
    Type type = Type::Replace;
    std::string old_string;
    std::string new_string;
};

/// FileEditTool input parameters
struct FileEditInput {
    fs::path file_path;
    std::vector<EditOperation> edits;
    bool validate_before_write = true;
    bool create_backup = true;
    bool dry_run = false;

    /// Parse from JSON using yyjson for proper escape handling
    static std::expected<FileEditInput, std::string> from_json(std::string_view json) {
        using namespace cc::utils::json;
        auto doc = parse(json);
        if (!doc) {
            return std::unexpected("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return std::unexpected("Expected JSON object");
        }

        FileEditInput input;

        // Extract file_path (required)
        auto path_node = root.get("file_path");
        if (!path_node.is_str()) {
            return std::unexpected("Missing 'file_path' field");
        }
        input.file_path = std::string(path_node.as_str());

        // Extract old_string and new_string (simple replace mode)
        auto old_node = root.get("old_string");
        auto new_node = root.get("new_string");

        if (old_node.is_str() && new_node.is_str()) {
            auto replace_all_node = root.get("replace_all");
            auto type = (replace_all_node.is_bool() && replace_all_node.as_bool())
                ? EditOperation::Type::ReplaceAll
                : EditOperation::Type::Replace;

            input.edits.push_back(EditOperation{
                .type = type,
                .old_string = std::string(old_node.as_str()),
                .new_string = std::string(new_node.as_str())
            });
        }

        // Extract options
        auto validate_node = root.get("validate_before_write");
        if (validate_node.is_bool()) {
            input.validate_before_write = validate_node.as_bool();
        }

        auto backup_node = root.get("create_backup");
        if (backup_node.is_bool()) {
            input.create_backup = backup_node.as_bool();
        }

        auto dry_run_node = root.get("dry_run");
        if (dry_run_node.is_bool()) {
            input.dry_run = dry_run_node.as_bool();
        }

        if (input.file_path.empty()) {
            return std::unexpected("Missing 'file_path' field");
        }

        return input;
    }
};

/// FileEditTool output result
struct FileEditOutput {
    fs::path edited_path;
    std::vector<EditOperation> applied_edits;
    std::uint64_t num_edits = 0;
    std::uint64_t num_replacements = 0;
    bool content_changed = false;
    std::optional<fs::path> backup_path;
    bool dry_run = false;
};

// =========================================================================
// String Replacement Utilities
// =========================================================================

/// Count occurrences of a substring
std::size_t count_occurrences(std::string_view text, std::string_view search) {
    if (search.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(search, pos)) != std::string_view::npos) {
        ++count;
        pos += search.length();
    }
    return count;
}

/// Replace all occurrences (returns new string)
std::string replace_all(std::string_view text, std::string_view search, std::string_view replace) {
    if (search.empty()) return std::string(text);
    
    std::string result;
    result.reserve(text.length());
    
    std::size_t pos = 0;
    std::size_t prev = 0;
    
    while ((pos = text.find(search, prev)) != std::string_view::npos) {
        result.append(text, prev, pos - prev);
        result.append(replace);
        prev = pos + search.length();
    }
    
    result.append(text, prev);
    return result;
}

/// Replace first occurrence only
std::string replace_first(std::string_view text, std::string_view search, std::string_view replace) {
    if (search.empty()) return std::string(text);
    
    auto pos = text.find(search);
    if (pos == std::string_view::npos) {
        return std::string(text);
    }
    
    std::string result;
    result.reserve(text.length() - search.length() + replace.length());
    result.append(text, 0, pos);
    result.append(replace);
    result.append(text, pos + search.length());
    return result;
}

// =========================================================================
// Sed integration hooks
// =========================================================================

/// Try to extract a FileEditInput from a raw sed -i command string.
/// Enables the FileEdit rendering pipeline to handle BashTool-style
/// in-place edits (e.g. `sed -i 's/foo/bar/g' file.txt`) using the same
/// UI / permission surface as explicit Edit tool calls.
///
/// TODO(migration): integrate into the BashTool → EditTool bridge so that
/// sed in-place edits produce file-edit-style diff previews before
/// executing.  Currently returns std::nullopt when the command does not
/// parse as a simple substitution, preserving existing behaviour.
[[nodiscard]] inline std::optional<FileEditInput>
try_parse_sed_in_place(std::string_view sed_command) {
    using cc::tools::sed_edit_parser::parse_sed_edit_command;
    using cc::tools::sed_edit_parser::SedOp;

    auto parsed = parse_sed_edit_command(sed_command);
    if (!parsed.has_value()) return std::nullopt;

    if (parsed->commands.empty()) return std::nullopt;
    if (parsed->commands[0].op != SedOp::Substitute) return std::nullopt;

    FileEditInput input;
    input.file_path = parsed->file_path;
    // NOTE: FileEditInput currently models literal replacements; a full
    // integration would add a SedSubstitute edit kind so that regex
    // patterns (with backreferences) can round-trip accurately.  For now
    // we leave the edit vector empty — the caller can fall back to
    // executing the sed command via BashTool.
    // TODO(migration): add SedSubstitute EditOperation::Type and populate
    // input.edits with parsed->pattern / parsed->replacement.
    (void)parsed;
    return input;
}

// =========================================================================
// FileEditTool Implementation
// =========================================================================

/// FileEditTool - Edits files with safety checks
class FileEditTool {
public:
    static constexpr std::string_view kName = "Edit";
    static constexpr std::string_view kDescription = 
        "Edit a file using string replacement. Use with care!";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "file_path",
                        .type = "string",
                        .description = "Absolute path to the file to edit",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "old_string",
                        .type = "string",
                        .description = "String to replace in the file",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "new_string",
                        .type = "string",
                        .description = "String to use as replacement",
                        .required = true
                    }
                }
            },
            .permission = ToolPermission::Write,
            .category = "filesystem"
        };
    }
    
    FileEditTool() = default;
    
    /// Set allowed directories for file editing
    void set_allowed_directories(std::vector<std::string> dirs) {
        allowed_directories_ = std::move(dirs);
    }
    
    /// Check if edit is allowed based on path permissions
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        auto parsed = FileEditInput::from_json(input.json());
        if (!parsed) return false;
        return is_path_allowed(parsed->file_path);
    }
    
    /// Check if a file path is within allowed directories
    [[nodiscard]] bool is_path_allowed(const fs::path& path) const {
        if (allowed_directories_.empty()) return true;  // No restriction
        
        std::string abs_path;
        try {
            abs_path = fs::absolute(path).string();
        } catch (...) {
            abs_path = path.string();
        }
        
        for (const auto& dir : allowed_directories_) {
            if (abs_path.starts_with(dir)) return true;
        }
        return false;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_input = FileEditInput::from_json(input.json());
        if (!parsed_input) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        return execute_internal(*parsed_input);
    }
    
private:
    std::vector<std::string> allowed_directories_;
    std::unordered_map<std::string, std::vector<fs::path>> backup_history_;
    
    /// Internal execution
    Result<ToolResult> execute_internal(const FileEditInput& input) {
        try {
            // Validate path
            if (input.file_path.empty()) {
                return ToolResult::error("Invalid empty file path");
            }
            
            // Check path permissions
            if (!is_path_allowed(input.file_path)) {
                return ToolResult::error(std::format(
                    "Path '{}' is outside allowed directories", input.file_path.string()));
            }
            
            // Check if file exists
            if (!fs::exists(input.file_path)) {
                return ToolResult::error(
                    std::format("File not found: {}", input.file_path.string())
                );
            }
            
            // Read original content
            auto read_result = cc::utils::file::read_file(input.file_path);
            if (!read_result) {
                return ToolResult::error(read_result.error());
            }
            std::string original_content = *read_result;
            
            FileEditOutput output;
            output.edited_path = input.file_path;
            output.dry_run = input.dry_run;
            
            // Apply edits
            std::string current_content = original_content;
            
            for (const auto& edit : input.edits) {
                auto occurrences = count_occurrences(current_content, edit.old_string);
                
                if (occurrences == 0) {
                    return ToolResult::error(std::format(
                        "String not found in file: '{}'",
                        edit.old_string
                    ));
                }
                
                if (occurrences > 1) {
                    return ToolResult::error(std::format(
                        "Multiple occurrences ({}) of '{}' in file - be more specific",
                        occurrences,
                        edit.old_string
                    ));
                }
                
                // Apply replacement
                switch (edit.type) {
                    case EditOperation::Type::Replace:
                        current_content = replace_first(current_content, edit.old_string, edit.new_string);
                        output.num_replacements += 1;
                        break;
                    case EditOperation::Type::ReplaceAll:
                        current_content = replace_all(current_content, edit.old_string, edit.new_string);
                        output.num_replacements += occurrences;
                        break;
                    default:
                        break;
                }
                
                output.applied_edits.push_back(edit);
            }
            
            output.num_edits = output.applied_edits.size();
            output.content_changed = (current_content != original_content);
            
            // If dry run, return without writing
            if (input.dry_run) {
                return format_result(output, current_content, true);
            }
            
            // Validate edit if requested
            if (input.validate_before_write) {
                auto validation = validate_edit(original_content, current_content);
                if (validation.has_value()) {
                    return ToolResult::error(*validation);
                }
            }
            
            // Create backup if needed
            if (input.create_backup && output.content_changed) {
                auto backup_path = create_backup(input.file_path);
                if (backup_path) {
                    output.backup_path = *backup_path;
                }
            }
            
            // Write modified content
            auto write_result = cc::utils::file::write_file(input.file_path, current_content);
            if (!write_result) {
                return ToolResult::error(write_result.error());
            }
            
            return format_result(output, current_content, false);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Edit error: {}", e.what()));
        }
    }
    
    /// Validate the edit for safety
    std::optional<std::string> validate_edit(
        std::string_view original, 
        std::string_view modified
    ) {
        // Basic validation - can be extended
        if (modified.length() > original.length() * 10) {
            return "Edit results in more than 10x file size increase - aborting";
        }
        return std::nullopt;
    }
    
    /// Create backup of file
    std::optional<fs::path> create_backup(const fs::path& path) {
        try {
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
            
            auto backup_path = std::format("{}.edit.bak.{}", path.string(), millis);
            
            fs::copy_file(path, backup_path, fs::copy_options::overwrite_existing);
            
            auto& history = backup_history_[path.string()];
            history.push_back(backup_path);
            
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
    ToolResult format_result(
        const FileEditOutput& output,
        std::string_view new_content,
        bool dry_run
    ) {
        std::string result;
        
        if (dry_run) {
            result = "[Dry Run - Would edit file]\n";
        } else {
            result = "[Edited file]\n";
        }
        
        result += std::format("File: {}\n", output.edited_path.string());
        result += std::format("Edits applied: {}\n", output.num_edits);
        result += std::format("Replacements made: {}\n", output.num_replacements);
        
        if (output.backup_path) {
            result += std::format("Backup created at: {}\n", output.backup_path->string());
        }
        
        if (output.content_changed) {
            result += "\nContent changed successfully";
        } else {
            result += "\nNo changes made";
        }
        
        return ToolResult::success(result);
    }
};

} // namespace cc::tools::file_edit

// Export main tool class
export namespace cc::tools {
    using cc::tools::file_edit::FileEditTool;

    /// Factory: create FileEditTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_file_edit_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            FileEditTool tool_;
            cc::core::ToolDefinition def_ = FileEditTool::definition();

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

    /// Convenience: parse a `sed -i …` command into a FileEditInput so the
    /// caller can render an edit-style preview before executing it.
    /// Returns std::nullopt when the command is not a simple sed in-place
    /// substitution (caller should fall back to BashTool execution).
    [[nodiscard]] inline auto try_parse_sed_in_place(std::string_view cmd) {
        return cc::tools::file_edit::try_parse_sed_in_place(cmd);
    }
}
