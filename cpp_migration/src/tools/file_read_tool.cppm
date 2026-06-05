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
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.tools.file_read;

import cc.utils.file;
import cc.utils.error;
import cc.services.image;
import cc.tools.tool;
import cc.tools.notebook;
import cc.utils.json;

export namespace cc::tools::file_read {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolOutputContent;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;
using cc::services::image::ImageService;
using cc::services::image::format_to_mime;

namespace fs = std::filesystem;

constexpr std::size_t kNotebookLargeOutputThreshold = 10000;

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

[[nodiscard]] std::string notebook_cell_id(const NotebookCell& cell, std::size_t index) {
    return cell.id.value_or(std::format("cell-{}", index));
}

[[nodiscard]] bool notebook_outputs_are_large(const NotebookCell& cell) {
    std::size_t size = 0;
    for (const auto& output : cell.outputs) {
        size += output.text.size();
        if (size > kNotebookLargeOutputThreshold) return true;
    }
    return false;
}

[[nodiscard]] std::string format_notebook_for_read(const Notebook& notebook, const fs::path& path) {
    const auto language = notebook.language.empty() ? std::string("python") : notebook.language;
    std::string result;
    result += std::format("Notebook: {}\n", path.string());
    result += std::format("Cells: {}\n\n", notebook.cells.size());

    for (std::size_t i = 0; i < notebook.cells.size(); ++i) {
        const auto& cell = notebook.cells[i];
        const auto id = notebook_cell_id(cell, i);
        std::string metadata;
        if (cell.cell_type != CellType::Code) {
            metadata += std::format("<cell_type>{}</cell_type>", cell_type_name(cell.cell_type));
        }
        if (cell.cell_type == CellType::Code && language != "python") {
            metadata += std::format("<language>{}</language>", language);
        }

        result += std::format("<cell id=\"{}\">{}{}</cell id=\"{}\">\n", id, metadata, cell.source, id);

        if (cell.cell_type == CellType::Code && !cell.outputs.empty()) {
            if (notebook_outputs_are_large(cell)) {
                result += std::format(
                    "Outputs are too large to include. Use Bash with: cat {} | jq '.cells[{}].outputs'\n",
                    path.string(), i);
            } else {
                for (const auto& output : cell.outputs) {
                    if (output.text.empty()) continue;
                    result += output.text;
                    if (!result.ends_with('\n')) result.push_back('\n');
                }
            }
        }

        if (i + 1 < notebook.cells.size()) {
            result.push_back('\n');
        }
    }

    return result;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> read_binary_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("Failed to open file: {}", path.string()));
    }
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    if (size < 0) {
        return std::unexpected(std::format("Failed to stat file: {}", path.string()));
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            return std::unexpected(std::format("Failed to read file: {}", path.string()));
        }
    }
    return bytes;
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
    
    [[nodiscard]] bool check_permission(const ToolInput&) const {
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
            .total_lines = lines.size(),
            .base64_image = std::nullopt,
            .image_type = std::nullopt,
            .original_size = std::nullopt,
        };
        
        return format_text_result(output);
    }
    
    /// Read image file
    Result<ToolResult> read_image(const FileReadInput& input) {
        auto bytes = read_binary_file(input.file_path);
        if (!bytes) {
            return ToolResult::error(bytes.error());
        }

        auto info = ImageService::get_info(input.file_path);
        if (!info) {
            return ToolResult::error(info.error().message);
        }
        auto media_type = std::string(format_to_mime(info->format));
        if (media_type == "application/octet-stream") {
            return ToolResult::error(std::format("Unsupported image format: {}", input.file_path.string()));
        }

        auto data = ImageService::to_base64(std::span<const std::uint8_t>(bytes->data(), bytes->size()));
        return ToolResult::success_multi({
            ToolOutputContent::text_output(std::format(
                "Image file read: {} ({})",
                input.file_path.string(),
                info->summary())),
            ToolOutputContent::image_output(std::move(media_type), std::move(data)),
        });
    }
    
    /// Read PDF file
    Result<ToolResult> read_pdf(const FileReadInput& input) {
        auto bytes = read_binary_file(input.file_path);
        if (!bytes) {
            return ToolResult::error(bytes.error());
        }

        auto data = ImageService::to_base64(std::span<const std::uint8_t>(bytes->data(), bytes->size()));
        const auto file_size = bytes->size();
        return ToolResult::success_multi({
            ToolOutputContent::text_output(std::format(
                "PDF file read: {} ({} bytes)",
                input.file_path.string(),
                file_size)),
            ToolOutputContent::document_output("application/pdf", std::move(data)),
        });
    }
    
    /// Read notebook file
    Result<ToolResult> read_notebook(const FileReadInput& input) {
        NotebookEditTool notebook_tool;
        auto notebook = notebook_tool.load_notebook(input.file_path);
        if (!notebook) {
            return ToolResult::error(std::string(format_error(notebook.error())));
        }
        return ToolResult::success(format_notebook_for_read(*notebook, input.file_path));
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
