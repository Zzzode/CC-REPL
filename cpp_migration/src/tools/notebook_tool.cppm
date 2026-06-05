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

#include <yyjson.h>

export module cc.tools.notebook;

namespace cc::tools::notebook_detail {

class JsonDocHandle {
public:
    explicit JsonDocHandle(yyjson_doc* doc = nullptr) : doc_(doc) {}
    ~JsonDocHandle() {
        if (doc_) yyjson_doc_free(doc_);
    }

    JsonDocHandle(const JsonDocHandle&) = delete;
    JsonDocHandle& operator=(const JsonDocHandle&) = delete;

    JsonDocHandle(JsonDocHandle&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    auto operator=(JsonDocHandle&& other) noexcept -> JsonDocHandle& {
        if (this != &other) {
            if (doc_) yyjson_doc_free(doc_);
            doc_ = std::exchange(other.doc_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] auto get() const noexcept -> yyjson_doc* { return doc_; }

private:
    yyjson_doc* doc_;
};

class JsonMutDocHandle {
public:
    explicit JsonMutDocHandle(yyjson_mut_doc* doc = nullptr) : doc_(doc) {}
    ~JsonMutDocHandle() {
        if (doc_) yyjson_mut_doc_free(doc_);
    }

    JsonMutDocHandle(const JsonMutDocHandle&) = delete;
    JsonMutDocHandle& operator=(const JsonMutDocHandle&) = delete;

    JsonMutDocHandle(JsonMutDocHandle&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    auto operator=(JsonMutDocHandle&& other) noexcept -> JsonMutDocHandle& {
        if (this != &other) {
            if (doc_) yyjson_mut_doc_free(doc_);
            doc_ = std::exchange(other.doc_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] auto get() const noexcept -> yyjson_mut_doc* { return doc_; }

private:
    yyjson_mut_doc* doc_;
};

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::expected<std::string, std::string> {
    std::ifstream file(path);
    if (!file) return std::unexpected("cannot open file");
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

[[nodiscard]] auto source_to_string(yyjson_val* source) -> std::string {
    if (yyjson_is_str(source)) {
        const char* text = yyjson_get_str(source);
        return text ? std::string(text, yyjson_get_len(source)) : std::string{};
    }

    if (!yyjson_is_arr(source)) return {};

    std::string joined;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(source, &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
        if (!yyjson_is_str(item)) continue;
        const char* text = yyjson_get_str(item);
        if (text) joined.append(text, yyjson_get_len(item));
    }
    return joined;
}

void replace_obj_value(yyjson_mut_doc* doc, yyjson_mut_val* obj, std::string_view key, yyjson_mut_val* value) {
    yyjson_mut_obj_remove_keyn(obj, key.data(), key.size());
    auto* key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
    yyjson_mut_obj_add(obj, key_val, value);
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

        yyjson_read_err err{};
        notebook_detail::JsonDocHandle doc(yyjson_read_opts(
            const_cast<char*>(content->data()), content->size(), 0, nullptr, &err));
        if (!doc.get()) {
            return std::unexpected(NotebookError::ParseError);
        }

        auto* root = yyjson_doc_get_root(doc.get());
        if (!yyjson_is_obj(root)) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        auto* nbformat = yyjson_obj_get(root, "nbformat");
        auto* nbformat_minor = yyjson_obj_get(root, "nbformat_minor");
        auto* cells = yyjson_obj_get(root, "cells");
        if (!yyjson_is_num(nbformat) || !yyjson_is_arr(cells)) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        Notebook nb;
        nb.raw_json = std::move(*content);
        nb.nbformat = static_cast<int>(yyjson_get_sint(nbformat));
        if (yyjson_is_num(nbformat_minor)) {
            nb.nbformat_minor = static_cast<int>(yyjson_get_sint(nbformat_minor));
        }

        auto* metadata = yyjson_obj_get(root, "metadata");
        if (yyjson_is_obj(metadata)) {
            auto* kernelspec = yyjson_obj_get(metadata, "kernelspec");
            if (yyjson_is_obj(kernelspec)) {
                auto* name = yyjson_obj_get(kernelspec, "name");
                if (yyjson_is_str(name)) {
                    nb.kernel_name.assign(yyjson_get_str(name), yyjson_get_len(name));
                }
            }
            auto* language_info = yyjson_obj_get(metadata, "language_info");
            if (yyjson_is_obj(language_info)) {
                auto* name = yyjson_obj_get(language_info, "name");
                if (yyjson_is_str(name)) {
                    nb.language.assign(yyjson_get_str(name), yyjson_get_len(name));
                }
            }
        }

        yyjson_arr_iter iter;
        yyjson_arr_iter_init(cells, &iter);
        yyjson_val* cell_val = nullptr;
        while ((cell_val = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!yyjson_is_obj(cell_val)) {
                return std::unexpected(NotebookError::InvalidNotebook);
            }

            auto* type_val = yyjson_obj_get(cell_val, "cell_type");
            if (!yyjson_is_str(type_val)) {
                return std::unexpected(NotebookError::InvalidNotebook);
            }
            auto cell_type = parse_cell_type(std::string_view(yyjson_get_str(type_val), yyjson_get_len(type_val)));
            if (!cell_type) {
                return std::unexpected(NotebookError::InvalidCellType);
            }

            NotebookCell cell;
            cell.cell_type = *cell_type;

            auto* id_val = yyjson_obj_get(cell_val, "id");
            if (yyjson_is_str(id_val)) {
                cell.id = std::string(yyjson_get_str(id_val), yyjson_get_len(id_val));
            }

            cell.source = notebook_detail::source_to_string(yyjson_obj_get(cell_val, "source"));

            auto* execution_count = yyjson_obj_get(cell_val, "execution_count");
            if (yyjson_is_num(execution_count)) {
                cell.execution_count = static_cast<int>(yyjson_get_sint(execution_count));
            }

            auto* outputs = yyjson_obj_get(cell_val, "outputs");
            if (yyjson_is_arr(outputs)) {
                yyjson_arr_iter outputs_iter;
                yyjson_arr_iter_init(outputs, &outputs_iter);
                yyjson_val* output_val = nullptr;
                while ((output_val = yyjson_arr_iter_next(&outputs_iter)) != nullptr) {
                    if (!yyjson_is_obj(output_val)) continue;
                    CellOutput output;
                    auto* output_type = yyjson_obj_get(output_val, "output_type");
                    if (yyjson_is_str(output_type)) {
                        output.output_type.assign(yyjson_get_str(output_type), yyjson_get_len(output_type));
                    }
                    auto* name = yyjson_obj_get(output_val, "name");
                    if (yyjson_is_str(name)) {
                        output.name = std::string(yyjson_get_str(name), yyjson_get_len(name));
                    }
                    output.text = notebook_detail::source_to_string(yyjson_obj_get(output_val, "text"));
                    cell.outputs.push_back(std::move(output));
                }
            }

            std::size_t raw_len = 0;
            char* raw_cell = yyjson_val_write(cell_val, 0, &raw_len);
            if (raw_cell) {
                cell.raw_json.assign(raw_cell, raw_len);
                std::free(raw_cell);
            }

            nb.cells.push_back(std::move(cell));
        }

        return nb;
    }

    // Save notebook back to disk
    auto save_notebook(const std::filesystem::path& path, const Notebook& notebook) const
        -> std::expected<void, NotebookError>
    {
        yyjson_read_err err{};
        notebook_detail::JsonDocHandle doc(yyjson_read_opts(
            const_cast<char*>(notebook.raw_json.data()), notebook.raw_json.size(), 0, nullptr, &err));
        if (!doc.get()) {
            return std::unexpected(NotebookError::ParseError);
        }

        notebook_detail::JsonMutDocHandle mut_doc(yyjson_doc_mut_copy(doc.get(), nullptr));
        if (!mut_doc.get()) {
            return std::unexpected(NotebookError::ParseError);
        }

        auto* root = yyjson_mut_doc_get_root(mut_doc.get());
        if (!yyjson_mut_is_obj(root)) {
            return std::unexpected(NotebookError::InvalidNotebook);
        }

        auto* cells = yyjson_mut_arr(mut_doc.get());
        for (const auto& cell : notebook.cells) {
            yyjson_mut_val* cell_obj = nullptr;
            if (!cell.raw_json.empty()) {
                yyjson_read_err cell_err{};
                notebook_detail::JsonDocHandle cell_doc(yyjson_read_opts(
                    const_cast<char*>(cell.raw_json.data()), cell.raw_json.size(), 0, nullptr, &cell_err));
                if (cell_doc.get()) {
                    cell_obj = yyjson_val_mut_copy(mut_doc.get(), yyjson_doc_get_root(cell_doc.get()));
                }
            }
            if (!cell_obj) {
                cell_obj = yyjson_mut_obj(mut_doc.get());
            }

            if (cell.dirty || cell.raw_json.empty()) {
                notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "cell_type",
                    yyjson_mut_strncpy(mut_doc.get(), cell_type_name(cell.cell_type).data(), cell_type_name(cell.cell_type).size()));
                notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "source",
                    yyjson_mut_strncpy(mut_doc.get(), cell.source.data(), cell.source.size()));

                if (!yyjson_mut_obj_get(cell_obj, "metadata")) {
                    notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "metadata",
                        yyjson_mut_obj(mut_doc.get()));
                }
                if (cell.id && !yyjson_mut_obj_get(cell_obj, "id")) {
                    notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "id",
                        yyjson_mut_strncpy(mut_doc.get(), cell.id->data(), cell.id->size()));
                }
                if (cell.cell_type == CellType::Code && (cell.clear_code_outputs || cell.raw_json.empty())) {
                    notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "execution_count",
                        yyjson_mut_null(mut_doc.get()));
                    notebook_detail::replace_obj_value(mut_doc.get(), cell_obj, "outputs",
                        yyjson_mut_arr(mut_doc.get()));
                }
            }

            yyjson_mut_arr_append(cells, cell_obj);
        }

        notebook_detail::replace_obj_value(mut_doc.get(), root, "cells", cells);

        std::size_t len = 0;
        char* json = yyjson_mut_write(mut_doc.get(), YYJSON_WRITE_PRETTY, &len);
        if (!json) {
            return std::unexpected(NotebookError::IoError);
        }

        std::ofstream file(path);
        if (!file) {
            std::free(json);
            return std::unexpected(NotebookError::IoError);
        }
        file.write(json, static_cast<std::streamsize>(len));
        file << '\n';
        std::free(json);

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
