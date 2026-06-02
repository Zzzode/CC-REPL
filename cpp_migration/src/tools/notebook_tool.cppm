// NotebookEditTool - Jupyter .ipynb notebook editing operations
module;
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.notebook;


export namespace cc::tools {

// Cell types in a Jupyter notebook
enum class CellType {
    Code,
    Markdown,
    Raw,
};

constexpr auto cell_type_name(CellType t) -> std::string_view {
    switch (t) {
        case CellType::Code:     return "code";
        case CellType::Markdown: return "markdown";
        case CellType::Raw:      return "raw";
        default:                 return "unknown";
    }
}

// Cell operations
enum class CellOperation {
    Insert,
    Delete,
    Update,
    Move,
};

constexpr auto cell_op_name(CellOperation op) -> std::string_view {
    switch (op) {
        case CellOperation::Insert: return "insert";
        case CellOperation::Delete: return "delete";
        case CellOperation::Update: return "update";
        case CellOperation::Move:   return "move";
        default:                    return "unknown";
    }
}

// Error types for notebook operations
enum class NotebookError {
    PathEmpty,
    FileNotFound,
    InvalidNotebook,
    CellIndexOutOfRange,
    InvalidCellType,
    InvalidOperation,
    ParseError,
    IoError,
    ContentTooLarge,
};

constexpr auto format_error(NotebookError err) -> std::string_view {
    switch (err) {
        case NotebookError::PathEmpty:           return "Notebook path is empty";
        case NotebookError::FileNotFound:        return "Notebook file not found";
        case NotebookError::InvalidNotebook:     return "Invalid notebook format";
        case NotebookError::CellIndexOutOfRange: return "Cell index is out of range";
        case NotebookError::InvalidCellType:     return "Invalid cell type";
        case NotebookError::InvalidOperation:    return "Invalid cell operation";
        case NotebookError::ParseError:          return "Failed to parse notebook JSON";
        case NotebookError::IoError:             return "I/O error reading/writing notebook";
        case NotebookError::ContentTooLarge:     return "Cell content exceeds size limit";
        default:                                 return "Unknown notebook error";
    }
}

// Cell output representation
struct CellOutput {
    std::string output_type;  // "stream", "execute_result", "display_data", "error"
    std::string text;
    std::optional<std::string> name;  // "stdout", "stderr" for stream outputs
};

// Notebook cell structure
struct NotebookCell {
    CellType cell_type{CellType::Code};
    std::string source;
    std::vector<CellOutput> outputs;
    std::optional<int> execution_count;
    std::unordered_map<std::string, std::string> metadata;
};

// Notebook structure (simplified .ipynb representation)
struct Notebook {
    std::vector<NotebookCell> cells;
    int nbformat{4};
    int nbformat_minor{5};
    std::string kernel_name;
    std::string language;
};

// Edit request for cell operations
struct NotebookEditRequest {
    std::filesystem::path notebook_path;
    CellOperation operation;
    size_t cell_index{0};                         // Target cell index
    std::optional<size_t> target_index;           // Destination for move operation
    std::optional<CellType> cell_type;            // For insert/update
    std::optional<std::string> source;            // Cell content for insert/update
};

// Edit result
struct NotebookEditResult {
    size_t total_cells{0};
    size_t affected_index{0};
    CellOperation operation_performed;
    std::string message;
};

// NotebookEditTool - edits Jupyter notebook cells
class NotebookEditTool {
public:
    static constexpr std::string_view name = "notebook_edit";
    static constexpr std::string_view description = "Edit Jupyter notebook cells (insert, delete, update, move)";
    static constexpr size_t kMaxCellContent = 1024 * 1024; // 1MB per cell

    NotebookEditTool() = default;

    // Validate an edit request
    auto validate(const NotebookEditRequest& request, const Notebook& notebook) const
        -> std::expected<void, NotebookError>
    {
        if (request.notebook_path.empty()) {
            return std::unexpected(NotebookError::PathEmpty);
        }

        size_t cell_count = notebook.cells.size();

        switch (request.operation) {
            case CellOperation::Insert:
                if (request.cell_index > cell_count) {
                    return std::unexpected(NotebookError::CellIndexOutOfRange);
                }
                if (!request.source) {
                    return std::unexpected(NotebookError::InvalidOperation);
                }
                if (request.source->size() > kMaxCellContent) {
                    return std::unexpected(NotebookError::ContentTooLarge);
                }
                break;

            case CellOperation::Delete:
                if (request.cell_index >= cell_count) {
                    return std::unexpected(NotebookError::CellIndexOutOfRange);
                }
                break;

            case CellOperation::Update:
                if (request.cell_index >= cell_count) {
                    return std::unexpected(NotebookError::CellIndexOutOfRange);
                }
                if (!request.source) {
                    return std::unexpected(NotebookError::InvalidOperation);
                }
                break;

            case CellOperation::Move:
                if (request.cell_index >= cell_count) {
                    return std::unexpected(NotebookError::CellIndexOutOfRange);
                }
                if (!request.target_index || *request.target_index >= cell_count) {
                    return std::unexpected(NotebookError::CellIndexOutOfRange);
                }
                break;
        }
        return {};
    }

    // Load a notebook from disk (simplified JSON parsing)
    auto load_notebook(const std::filesystem::path& path) const
        -> std::expected<Notebook, NotebookError>
    {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(NotebookError::FileNotFound);
        }

        std::ifstream file(path);
        if (!file) return std::unexpected(NotebookError::IoError);

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        // Simplified: in production use yyjson for proper parsing
        Notebook nb;
        if (content.find("\"nbformat\"") == std::string::npos) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }
        return nb;
    }

    // Save notebook back to disk
    auto save_notebook(const std::filesystem::path& path, const Notebook& notebook) const
        -> std::expected<void, NotebookError>
    {
        std::ofstream file(path);
        if (!file) return std::unexpected(NotebookError::IoError);

        // Simplified JSON serialization
        file << "{\n  \"nbformat\": " << notebook.nbformat
             << ",\n  \"nbformat_minor\": " << notebook.nbformat_minor
             << ",\n  \"cells\": [\n";

        for (size_t i = 0; i < notebook.cells.size(); ++i) {
            const auto& cell = notebook.cells[i];
            if (i > 0) file << ",\n";
            file << "    {\"cell_type\": \"" << cell_type_name(cell.cell_type)
                 << "\", \"source\": \"" << cell.source << "\"}";
        }
        file << "\n  ]\n}\n";

        if (!file.good()) return std::unexpected(NotebookError::IoError);
        return {};
    }

    // Execute a cell edit operation
    auto execute(NotebookEditRequest request) -> std::expected<NotebookEditResult, NotebookError> {
        auto nb_result = load_notebook(request.notebook_path);
        if (!nb_result) return std::unexpected(nb_result.error());
        auto& notebook = *nb_result;

        if (auto valid = validate(request, notebook); !valid) {
            return std::unexpected(valid.error());
        }

        NotebookEditResult result;
        result.operation_performed = request.operation;
        result.affected_index = request.cell_index;

        switch (request.operation) {
            case CellOperation::Insert: {
                NotebookCell new_cell{
                    .cell_type = request.cell_type.value_or(CellType::Code),
                    .source = *request.source,
                };
                notebook.cells.insert(notebook.cells.begin() +
                    static_cast<ptrdiff_t>(request.cell_index), std::move(new_cell));
                result.message = std::format("Inserted cell at index {}", request.cell_index);
                break;
            }
            case CellOperation::Delete: {
                notebook.cells.erase(notebook.cells.begin() +
                    static_cast<ptrdiff_t>(request.cell_index));
                result.message = std::format("Deleted cell at index {}", request.cell_index);
                break;
            }
            case CellOperation::Update: {
                auto& cell = notebook.cells[request.cell_index];
                cell.source = *request.source;
                if (request.cell_type) cell.cell_type = *request.cell_type;
                // Clear outputs on code cell update
                if (cell.cell_type == CellType::Code) cell.outputs.clear();
                result.message = std::format("Updated cell at index {}", request.cell_index);
                break;
            }
            case CellOperation::Move: {
                auto cell = std::move(notebook.cells[request.cell_index]);
                notebook.cells.erase(notebook.cells.begin() +
                    static_cast<ptrdiff_t>(request.cell_index));
                notebook.cells.insert(notebook.cells.begin() +
                    static_cast<ptrdiff_t>(*request.target_index), std::move(cell));
                result.message = std::format("Moved cell from {} to {}",
                    request.cell_index, *request.target_index);
                break;
            }
        }

        // Save modified notebook
        if (auto save = save_notebook(request.notebook_path, notebook); !save) {
            return std::unexpected(save.error());
        }

        result.total_cells = notebook.cells.size();
        return result;
    }

    // Generate JSON schema for LLM tool invocation
    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "notebook_path": {{ "type": "string", "description": "Path to the .ipynb file" }},
      "operation": {{ "type": "string", "enum": ["insert", "delete", "update", "move"] }},
      "cell_index": {{ "type": "integer", "description": "Target cell index (0-based)" }},
      "target_index": {{ "type": "integer", "description": "Destination index for move" }},
      "cell_type": {{ "type": "string", "enum": ["code", "markdown", "raw"] }},
      "source": {{ "type": "string", "description": "Cell content" }}
    }},
    "required": ["notebook_path", "operation", "cell_index"]
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
