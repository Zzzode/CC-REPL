// FileReadTool - Reads file content with range support and safety checks
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
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

export module cc.tools.file_read;

import cc.utils.file;
import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::file_read {

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
// FileReadTool Configuration and Types
// =========================================================================

/// File encoding detection result
enum class FileEncoding {
    UTF8,
    ASCII,
    Latin1,
    Binary,
    Unknown
};

/// Image formats supported for binary detection
constexpr std::array<std::string_view, 5> kImageExtensions = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp"
};

/// Blocked device paths that we should never read
constexpr std::array<std::string_view, 12> kBlockedPaths = {
    "/dev/zero", "/dev/random", "/dev/urandom", "/dev/full",
    "/dev/stdin", "/dev/tty", "/dev/console",
    "/dev/stdout", "/dev/stderr",
    "/dev/fd/0", "/dev/fd/1", "/dev/fd/2"
};

/// FileReadTool input parameters
struct FileReadInput {
    fs::path file_path;
    std::optional<std::uint64_t> offset;  // 1-based line number to start
    std::optional<std::uint64_t> limit;   // Number of lines to read
    std::optional<std::string> pages;     // Page range for PDF files

    /// Parse from JSON using yyjson for proper escape handling
    static std::expected<FileReadInput, std::string> from_json(std::string_view json) {
        using namespace cc::utils::json;
        auto doc = parse(json);
        if (!doc) {
            return std::unexpected("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return std::unexpected("Expected JSON object");
        }

        FileReadInput input;

        // Extract file_path (required)
        auto path_node = root.get("file_path");
        if (!path_node.is_str()) {
            return std::unexpected("Missing 'file_path' field");
        }
        input.file_path = std::string(path_node.as_str());

        // Extract offset (optional)
        auto offset_node = root.get("offset");
        if (offset_node.is_num()) {
            input.offset = static_cast<std::uint64_t>(offset_node.as_int());
        }

        // Extract limit (optional)
        auto limit_node = root.get("limit");
        if (limit_node.is_num()) {
            input.limit = static_cast<std::uint64_t>(limit_node.as_int());
        }

        // Extract pages (optional, for PDF)
        auto pages_node = root.get("pages");
        if (pages_node.is_str()) {
            input.pages = std::string(pages_node.as_str());
        }

        if (input.file_path.empty()) {
            return std::unexpected("Missing 'file_path' field");
        }

        return input;
    }
};

/// FileReadTool output result
struct FileReadOutput {
    enum class OutputType { Text, Image, Notebook, PDF, FileUnchanged };
    
    OutputType type = OutputType::Text;
    std::string content;
    std::string file_path;
    std::uint64_t num_lines = 0;
    std::uint64_t start_line = 1;
    std::uint64_t total_lines = 0;
    std::optional<std::string> base64_image;
    std::optional<std::string> image_type;
    std::optional<std::uint64_t> original_size;
};

// =========================================================================
// File Detection and Validation
// =========================================================================

/// Check if path is blocked
[[nodiscard]] bool is_blocked_path(const fs::path& path) noexcept {
    auto path_str = path.string();
    for (auto blocked : kBlockedPaths) {
        if (path_str == blocked) {
            return true;
        }
    }
    // Check for /proc/*/fd/* patterns
    if (path_str.starts_with("/proc/") && path_str.find("/fd/") != std::string::npos) {
        return true;
    }
    return false;
}

/// Detect if file is binary by checking for null bytes
[[nodiscard]] FileEncoding detect_encoding(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return FileEncoding::Unknown;
    }
    
    std::array<char, 8192> buffer;
    file.read(buffer.data(), buffer.size());
    auto bytes_read = file.gcount();
    
    if (bytes_read <= 0) {
        return FileEncoding::ASCII; // Empty file is ASCII
    }
    
    bool has_null = false;
    bool has_high_byte = false;
    for (std::streamsize i = 0; i < bytes_read; ++i) {
        auto byte = static_cast<unsigned char>(buffer[i]);
        if (byte == 0) {
            has_null = true;
            break;
        }
        if (byte > 127) {
            has_high_byte = true;
        }
    }
    
    if (has_null) {
        return FileEncoding::Binary;
    }
    if (!has_high_byte) {
        return FileEncoding::ASCII;
    }
    return FileEncoding::UTF8;
}

/// Check if file is an image by extension
[[nodiscard]] bool is_image_file(const fs::path& path) noexcept {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (auto img_ext : kImageExtensions) {
        if (ext == img_ext) {
            return true;
        }
    }
    return false;
}

/// Check if file is a PDF
[[nodiscard]] bool is_pdf_file(const fs::path& path) noexcept {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".pdf";
}

/// Check if file is a Jupyter notebook
[[nodiscard]] bool is_notebook_file(const fs::path& path) noexcept {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".ipynb";
}

// =========================================================================
// FileReadTool Implementation
// =========================================================================

/// FileReadTool - Reads files with safety checks
class FileReadTool {
public:
    static constexpr std::string_view kName = "Read";
    static constexpr std::string_view kDescription = 
        "Read the contents of a file. Supports line ranges for large files.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "file_path",
                        .type = "string",
                        .description = "Absolute path to the file to read",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "offset",
                        .type = "number",
                        .description = "Line number to start reading from (1-based)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "limit",
                        .type = "number",
                        .description = "Maximum number of lines to read",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "pages",
                        .type = "string",
                        .description = "Page range for PDF files (e.g., \"1-5\")",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::ReadOnly,
            .category = "filesystem"
        };
    }
    
    FileReadTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        // Always allow - permission checks would be implemented in production
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_input = FileReadInput::from_json(input.json());
        if (!parsed_input) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        return execute_internal(*parsed_input);
    }
    
private:
    std::unordered_map<std::string, std::pair<std::string, std::chrono::system_clock::time_point>> read_cache_;
    
    /// Internal execution
    Result<ToolResult> execute_internal(const FileReadInput& input) {
        try {
            // Validate path first
            if (is_blocked_path(input.file_path)) {
                return ToolResult::error(
                    std::format("Cannot read blocked path: {}", input.file_path.string())
                );
            }
            
            // Check if file exists
            if (!fs::exists(input.file_path)) {
                return ToolResult::error(
                    std::format("File not found: {}", input.file_path.string())
                );
            }
            
            // Check file type
            if (is_image_file(input.file_path)) {
                return read_image(input);
            }
            
            if (is_pdf_file(input.file_path)) {
                return read_pdf(input);
            }
            
            if (is_notebook_file(input.file_path)) {
                return read_notebook(input);
            }
            
            // Default: read as text
            return read_text(input);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Read error: {}", e.what()));
        }
    }
    
    /// Read file as text
    Result<ToolResult> read_text(const FileReadInput& input) {
        // Check if binary file
        auto encoding = detect_encoding(input.file_path);
        if (encoding == FileEncoding::Binary) {
            return ToolResult::error("Cannot read binary file as text");
        }
        
        std::ifstream file(input.file_path);
        if (!file) {
            return ToolResult::error(std::format("Failed to open file: {}", input.file_path.string()));
        }
        
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        
        // Apply offset and limit
        std::uint64_t start_idx = 0;
        std::uint64_t count = lines.size();
        
        if (input.offset) {
            start_idx = std::min(*input.offset - 1, static_cast<std::uint64_t>(lines.size()));
        }
        if (input.limit) {
            count = std::min(*input.limit, static_cast<std::uint64_t>(lines.size()) - start_idx);
        }
        
        // Build output with line numbers
        std::string result;
        std::uint64_t max_line = start_idx + count;
        std::size_t line_num_width = std::format("{}", max_line).size();
        
        for (std::uint64_t i = 0; i < count; ++i) {
            auto line_num = start_idx + i + 1;
            result += std::format("{:>{}}  {}\n", line_num, line_num_width, lines[start_idx + i]);
        }
        
        FileReadOutput output{
            .type = FileReadOutput::OutputType::Text,
            .content = result,
            .file_path = input.file_path.string(),
            .num_lines = count,
            .start_line = start_idx + 1,
            .total_lines = lines.size()
        };
        
        return format_text_result(output);
    }
    
    /// Read image file
    Result<ToolResult> read_image(const FileReadInput& input) {
        // Image handling would require more complex implementation
        // For now, return basic info
        auto file_size = fs::file_size(input.file_path);
        return ToolResult::success(std::format(
            "[Image file: {} ({} bytes)]",
            input.file_path.string(),
            file_size
        ));
    }
    
    /// Read PDF file
    Result<ToolResult> read_pdf(const FileReadInput& input) {
        auto file_size = fs::file_size(input.file_path);
        return ToolResult::success(std::format(
            "[PDF file: {} ({} bytes)]",
            input.file_path.string(),
            file_size
        ));
    }
    
    /// Read notebook file
    Result<ToolResult> read_notebook(const FileReadInput& input) {
        auto read_result = cc::utils::file::read_file(input.file_path);
        if (!read_result) {
            return ToolResult::error(read_result.error());
        }
        return ToolResult::success(*read_result);
    }
    
    /// Format text result
    ToolResult format_text_result(const FileReadOutput& output) {
        std::string result;
        
        if (!output.content.empty()) {
            result = output.content;
        } else if (output.total_lines == 0) {
            result = "[Empty file]";
        } else {
            result = std::format(
                "[File has {} lines, starting at line {}]",
                output.total_lines,
                output.start_line
            );
        }
        
        return ToolResult::success(result);
    }
};

} // namespace cc::tools::file_read

// Export main tool class
export namespace cc::tools {
    using cc::tools::file_read::FileReadTool;

    /// Factory: create FileReadTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_file_read_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            FileReadTool tool_;
            cc::core::ToolDefinition def_ = FileReadTool::definition();

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
