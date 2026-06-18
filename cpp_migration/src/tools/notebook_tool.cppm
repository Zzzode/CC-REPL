// NotebookEditTool - Jupyter .ipynb notebook editing operations
module;
#include <cstddef>
#include <cstdlib>
#include <chrono>
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

import cc.utils.json;

namespace cc::tools::notebook_detail {

namespace json = cc::utils::json;

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::expected<std::string, std::string> {
    std::ifstream file(path);
    if (!file) return std::unexpected("cannot open file");
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Flatten a notebook "source" field (either a string or an array of string
// lines) into a single string.
[[nodiscard]] auto source_to_string(json::JsonVal source) -> std::string {
    if (source.is_str()) {
        return std::string(source.as_str());
    }
    if (!source.is_arr()) return {};

    std::string joined;
    source.iter([&](json::JsonVal item) {
        if (item.is_str()) joined.append(std::string(item.as_str()));
    });
    return joined;
}

// Replace (or insert) `key`→`value` on a mutable object via the canonical API.
void replace_obj_value(json::JsonMutVal obj, std::string_view key, json::JsonMutVal value) {
    obj.remove(key);
    obj.add(key, value);
}

} // namespace cc::tools::notebook_detail


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

[[nodiscard]] inline auto parse_cell_type(std::string_view type) -> std::optional<CellType> {
    if (type == "code") return CellType::Code;
    if (type == "markdown") return CellType::Markdown;
    if (type == "raw") return CellType::Raw;
    return std::nullopt;
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
    std::optional<std::string> id;
    std::string source;
    std::vector<CellOutput> outputs;
    std::optional<int> execution_count;
    std::unordered_map<std::string, std::string> metadata;
    std::string raw_json;
    bool dirty{false};
    bool clear_code_outputs{false};
};

// Notebook structure (simplified .ipynb representation)
struct Notebook {
    std::vector<NotebookCell> cells;
    int nbformat{4};
    int nbformat_minor{5};
    std::string kernel_name;
    std::string language;
    std::string raw_json;
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

    // Load a notebook from disk.
    auto load_notebook(const std::filesystem::path& path) const
        -> std::expected<Notebook, NotebookError>
    {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(NotebookError::FileNotFound);
        }

        auto content = notebook_detail::read_file(path);
        if (!content) return std::unexpected(NotebookError::IoError);

        auto parsed = notebook_detail::json::parse(*content);
        if (!parsed) {
            return std::unexpected(NotebookError::ParseError);
        }

        auto root = parsed->root();
        if (!root.is_obj()) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        auto nbformat = root.get("nbformat");
        auto nbformat_minor = root.get("nbformat_minor");
        auto cells = root.get("cells");
        if (!nbformat.is_num() || !cells.is_arr()) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        Notebook nb;
        nb.raw_json = std::move(*content);
        nb.nbformat = static_cast<int>(nbformat.as_int());
        if (nbformat_minor.is_num()) {
            nb.nbformat_minor = static_cast<int>(nbformat_minor.as_int());
        }

        auto metadata = root.get("metadata");
        if (metadata.is_obj()) {
            auto kernelspec = metadata.get("kernelspec");
            if (kernelspec.is_obj()) {
                auto name = kernelspec.get("name");
                if (name.is_str()) {
                    nb.kernel_name = std::string(name.as_str());
                }
            }
            auto language_info = metadata.get("language_info");
            if (language_info.is_obj()) {
                auto name = language_info.get("name");
                if (name.is_str()) {
                    nb.language = std::string(name.as_str());
                }
            }
        }

        bool ok = true;
        cells.iter([&](notebook_detail::json::JsonVal cell_val) {
            if (!cell_val.is_obj()) { ok = false; return; }

            auto type_val = cell_val.get("cell_type");
            if (!type_val.is_str()) { ok = false; return; }
            auto cell_type = parse_cell_type(type_val.as_str());
            if (!cell_type) { ok = false; return; }

            NotebookCell cell;
            cell.cell_type = *cell_type;

            auto id_val = cell_val.get("id");
            if (id_val.is_str()) {
                cell.id = std::string(id_val.as_str());
            }

            cell.source = notebook_detail::source_to_string(cell_val.get("source"));

            auto execution_count = cell_val.get("execution_count");
            if (execution_count.is_num()) {
                cell.execution_count = static_cast<int>(execution_count.as_int());
            }

            auto outputs = cell_val.get("outputs");
            if (outputs.is_arr()) {
                outputs.iter([&cell](notebook_detail::json::JsonVal output_val) {
                    if (!output_val.is_obj()) return;
                    CellOutput output;
                    auto output_type = output_val.get("output_type");
                    if (output_type.is_str()) {
                        output.output_type = std::string(output_type.as_str());
                    }
                    auto name = output_val.get("name");
                    if (name.is_str()) {
                        output.name = std::string(name.as_str());
                    }
                    output.text = notebook_detail::source_to_string(output_val.get("text"));
                    cell.outputs.push_back(std::move(output));
                });
            }

            // Preserve the cell's original JSON for round-tripping untouched fields.
            cell.raw_json = notebook_detail::json::to_string(cell_val);

            nb.cells.push_back(std::move(cell));
        });
        if (!ok) return std::unexpected(NotebookError::InvalidNotebook);

        return nb;
    }

    // Save notebook back to disk
    auto save_notebook(const std::filesystem::path& path, const Notebook& notebook) const
        -> std::expected<void, NotebookError>
    {
        namespace json = notebook_detail::json;

        // Parse the round-tripped raw JSON, then copy it into a mutable document
        // so individual cells/fields can be edited in place.
        auto parsed = json::parse(notebook.raw_json);
        if (!parsed) {
            return std::unexpected(NotebookError::ParseError);
        }

        json::JsonMutDoc mut_doc;
        auto root = mut_doc.copy_val(parsed->root());
        mut_doc.set_root(root);
        if (!root.is_obj()) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        auto cells = mut_doc.array();
        for (const auto& cell : notebook.cells) {
            json::JsonMutVal cell_obj{nullptr, mut_doc.raw()};
            if (!cell.raw_json.empty()) {
                cell_obj = mut_doc.raw_json(cell.raw_json);
            }
            if (!cell_obj.valid()) {
                cell_obj = mut_doc.object();
            }

            if (cell.dirty || cell.raw_json.empty()) {
                notebook_detail::replace_obj_value(cell_obj, "cell_type",
                    mut_doc.string(cell_type_name(cell.cell_type)));
                notebook_detail::replace_obj_value(cell_obj, "source",
                    mut_doc.string(cell.source));

                if (!cell_obj.get("metadata").valid()) {
                    notebook_detail::replace_obj_value(cell_obj, "metadata", mut_doc.object());
                }
                if (cell.id && !cell_obj.get("id").valid()) {
                    notebook_detail::replace_obj_value(cell_obj, "id", mut_doc.string(*cell.id));
                }
                if (cell.cell_type == CellType::Code && (cell.clear_code_outputs || cell.raw_json.empty())) {
                    notebook_detail::replace_obj_value(cell_obj, "execution_count", mut_doc.null());
                    notebook_detail::replace_obj_value(cell_obj, "outputs", mut_doc.array());
                }
            }

            cells.append(cell_obj);
        }

        notebook_detail::replace_obj_value(root, "cells", cells);

        auto json_str = mut_doc.to_pretty_string();
        if (json_str.empty()) {
            return std::unexpected(NotebookError::IoError);
        }

        std::ofstream file(path);
        if (!file) {
            return std::unexpected(NotebookError::IoError);
        }
        file.write(json_str.data(), static_cast<std::streamsize>(json_str.size()));
        file << '\n';

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
                    .id = std::nullopt,
                    .source = *request.source,
                    .outputs = {},
                    .execution_count = std::nullopt,
                    .metadata = {},
                    .raw_json = {},
                    .dirty = true,
                    .clear_code_outputs = false,
                };
                if (notebook.nbformat > 4 || (notebook.nbformat == 4 && notebook.nbformat_minor >= 5)) {
                    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
                    new_cell.id = std::format("cell-{}", static_cast<unsigned long long>(now));
                }
                if (new_cell.cell_type == CellType::Code) {
                    new_cell.clear_code_outputs = true;
                }
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
                const bool was_code = cell.cell_type == CellType::Code;
                cell.source = *request.source;
                if (request.cell_type) cell.cell_type = *request.cell_type;
                cell.dirty = true;
                if (was_code) {
                    cell.outputs.clear();
                    cell.execution_count = std::nullopt;
                    cell.clear_code_outputs = true;
                }
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
