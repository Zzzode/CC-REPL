// LspTool - LSP operations wrapper for code intelligence actions
module;
#include <expected>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.lsp;

import cc.services.lsp.LSPServerManager;
import cc.utils.error;
import cc.utils.json;

export namespace cc::tools {

// LSP action types
enum class LspAction {
    Diagnostics,
    Definition,
    References,
    Completion,
    Hover,
    Symbols,
};

constexpr auto lsp_action_name(LspAction a) -> std::string_view {
    switch (a) {
        case LspAction::Diagnostics: return "diagnostics";
        case LspAction::Definition:  return "definition";
        case LspAction::References:  return "references";
        case LspAction::Completion:  return "completion";
        case LspAction::Hover:       return "hover";
        case LspAction::Symbols:     return "symbols";
        default:                     return "unknown";
    }
}

// Error types for LSP operations
enum class LspToolError {
    InvalidAction,
    FilePathEmpty,
    ServerNotConnected,
    RequestFailed,
    Timeout,
    ParseError,
    NoResults,
};

constexpr auto format_error(LspToolError err) -> std::string_view {
    switch (err) {
        case LspToolError::InvalidAction:      return "Invalid LSP action specified";
        case LspToolError::FilePathEmpty:      return "File path is empty";
        case LspToolError::ServerNotConnected: return "LSP server is not connected";
        case LspToolError::RequestFailed:      return "LSP request failed";
        case LspToolError::Timeout:            return "LSP request timed out";
        case LspToolError::ParseError:         return "Failed to parse LSP response";
        case LspToolError::NoResults:          return "No results returned";
        default:                               return "Unknown LSP tool error";
    }
}

// Position in a document (0-based)
struct LspPosition {
    int line{0};
    int character{0};
};

// Range in a document
struct LspRange {
    LspPosition start;
    LspPosition end;
};

// Location result (file + range)
struct LspLocation {
    std::string uri;
    LspRange range;
};

// Diagnostic entry
struct LspDiagnostic {
    LspRange range;
    std::string severity;  // "error", "warning", "info", "hint"
    std::string message;
    std::string source;
    std::string code;
};

// Completion item
struct LspCompletionItem {
    std::string label;
    std::string kind;       // "function", "variable", "class", etc.
    std::string detail;
    std::string insert_text;
};

// Symbol information
struct LspSymbol {
    std::string name;
    std::string kind;
    LspRange range;
    std::optional<std::string> container_name;
};

// Hover result
struct LspHoverResult {
    std::string contents;
    std::optional<LspRange> range;
};

// Unified LSP request input
struct LspRequest {
    LspAction action;
    std::filesystem::path file_path;
    std::optional<LspPosition> position;  // Required for definition, references, completion, hover
    std::optional<std::string> query;     // Optional filter for symbols
};

// Unified LSP result (variant of all possible response types)
struct LspResult {
    std::vector<LspDiagnostic> diagnostics;
    std::vector<LspLocation> locations;
    std::vector<LspCompletionItem> completions;
    std::vector<LspSymbol> symbols;
    std::optional<LspHoverResult> hover;

    [[nodiscard]] bool empty() const {
        return diagnostics.empty() && locations.empty()
            && completions.empty() && symbols.empty() && !hover;
    }
};

namespace detail {

[[nodiscard]] inline auto read_file_text(const std::filesystem::path& path)
    -> std::expected<std::string, LspToolError>
{
    std::ifstream input(path);
    if (!input) return std::unexpected(LspToolError::RequestFailed);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] inline auto number_field(cc::utils::json::JsonVal value, std::string_view key, int fallback = 0) -> int {
    if (!value.is_obj()) return fallback;
    auto child = value.get(key);
    return child.is_num() ? static_cast<int>(child.as_int()) : fallback;
}

[[nodiscard]] inline auto string_field(cc::utils::json::JsonVal value, std::string_view key) -> std::string {
    if (!value.is_obj()) return {};
    auto child = value.get(key);
    return child.is_str() ? std::string(child.as_str()) : std::string{};
}

[[nodiscard]] inline auto parse_position(cc::utils::json::JsonVal value) -> LspPosition {
    return LspPosition{
        .line = number_field(value, "line"),
        .character = number_field(value, "character"),
    };
}

[[nodiscard]] inline auto parse_range(cc::utils::json::JsonVal value) -> LspRange {
    if (!value.is_obj()) return {};
    return LspRange{
        .start = parse_position(value.get("start")),
        .end = parse_position(value.get("end")),
    };
}

[[nodiscard]] inline auto severity_name(int severity) -> std::string {
    switch (severity) {
        case 1: return "error";
        case 2: return "warning";
        case 3: return "info";
        case 4: return "hint";
        default: return "unknown";
    }
}

[[nodiscard]] inline auto symbol_kind_name(int kind) -> std::string {
    static constexpr std::array<std::string_view, 27> names{
        "unknown", "file", "module", "namespace", "package", "class", "method",
        "property", "field", "constructor", "enum", "interface", "function",
        "variable", "constant", "string", "number", "boolean", "array",
        "object", "key", "null", "enum_member", "struct", "event", "operator",
        "type_parameter",
    };
    if (kind >= 0 && static_cast<std::size_t>(kind) < names.size()) return std::string(names[static_cast<std::size_t>(kind)]);
    return std::to_string(kind);
}

[[nodiscard]] inline auto completion_kind_name(int kind) -> std::string {
    static constexpr std::array<std::string_view, 26> names{
        "unknown", "text", "method", "function", "constructor", "field",
        "variable", "class", "interface", "module", "property", "unit",
        "value", "enum", "keyword", "snippet", "color", "file", "reference",
        "folder", "enum_member", "constant", "struct", "event", "operator",
        "type_parameter",
    };
    if (kind >= 0 && static_cast<std::size_t>(kind) < names.size()) return std::string(names[static_cast<std::size_t>(kind)]);
    return std::to_string(kind);
}

[[nodiscard]] inline auto parse_location(cc::utils::json::JsonVal value) -> std::optional<LspLocation> {
    if (!value.is_obj()) return std::nullopt;
    auto uri = value.get("uri");
    auto range = value.get("range");
    if (!uri.is_str()) {
        uri = value.get("targetUri");
        range = value.get("targetSelectionRange");
        if (!range.is_obj()) range = value.get("targetRange");
    }
    if (!uri.is_str()) return std::nullopt;
    return LspLocation{
        .uri = std::string(uri.as_str()),
        .range = parse_range(range),
    };
}

inline void append_locations(cc::utils::json::JsonVal value, std::vector<LspLocation>& out) {
    if (auto location = parse_location(value)) {
        out.push_back(std::move(*location));
        return;
    }
    if (!value.is_arr()) return;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (auto location = parse_location(item)) out.push_back(std::move(*location));
    });
}

[[nodiscard]] inline auto contents_to_text(cc::utils::json::JsonVal value) -> std::string {
    if (value.is_str()) return std::string(value.as_str());
    if (value.is_arr()) {
        std::string out;
        value.iter([&](cc::utils::json::JsonVal item) {
            auto text = contents_to_text(item);
            if (text.empty()) return;
            if (!out.empty()) out += "\n";
            out += text;
        });
        return out;
    }
    if (value.is_obj()) {
        auto value_node = value.get("value");
        if (value_node.is_str()) return std::string(value_node.as_str());
        auto contents_node = value.get("contents");
        if (contents_node.valid()) return contents_to_text(contents_node);
    }
    return {};
}

inline void append_symbol(
    cc::utils::json::JsonVal value,
    std::vector<LspSymbol>& out,
    const std::optional<std::string>& query,
    std::optional<std::string> container = std::nullopt)
{
    if (!value.is_obj()) return;
    auto name = string_field(value, "name");
    auto kind = symbol_kind_name(number_field(value, "kind"));
    auto range_node = value.get("range");
    if (!range_node.is_obj()) {
        auto location = value.get("location");
        range_node = location.get("range");
    }

    if (!name.empty() && (!query || query->empty() || name.find(*query) != std::string::npos)) {
        out.push_back(LspSymbol{
            .name = name,
            .kind = kind,
            .range = parse_range(range_node),
            .container_name = container ? container : [&]() -> std::optional<std::string> {
                auto container_name = string_field(value, "containerName");
                return container_name.empty() ? std::nullopt : std::optional<std::string>{std::move(container_name)};
            }(),
        });
    }

    auto children = value.get("children");
    if (children.is_arr()) {
        children.iter([&](cc::utils::json::JsonVal child) {
            append_symbol(child, out, query, name.empty() ? container : std::optional<std::string>{name});
        });
    }
}

[[nodiscard]] inline auto parse_diagnostics(std::string_view json) -> std::expected<LspResult, LspToolError> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    if (!root.is_arr()) return result;
    root.iter([&](cc::utils::json::JsonVal item) {
        LspDiagnostic diagnostic;
        diagnostic.range = parse_range(item.get("range"));
        diagnostic.severity = severity_name(number_field(item, "severity", 3));
        diagnostic.message = string_field(item, "message");
        diagnostic.source = string_field(item, "source");
        auto code = item.get("code");
        if (code.is_str()) diagnostic.code = std::string(code.as_str());
        if (code.is_num()) diagnostic.code = std::to_string(code.as_int());
        result.diagnostics.push_back(std::move(diagnostic));
    });
    return result;
}

[[nodiscard]] inline auto parse_locations_result(std::string_view json) -> std::expected<LspResult, LspToolError> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    append_locations(parsed->root(), result.locations);
    return result;
}

[[nodiscard]] inline auto parse_completion_result(std::string_view json) -> std::expected<LspResult, LspToolError> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    auto items = root.is_arr() ? root : root.get("items");
    if (!items.is_arr()) return result;
    items.iter([&](cc::utils::json::JsonVal item) {
        LspCompletionItem completion;
        completion.label = string_field(item, "label");
        completion.kind = completion_kind_name(number_field(item, "kind"));
        completion.detail = string_field(item, "detail");
        completion.insert_text = string_field(item, "insertText");
        if (completion.insert_text.empty()) {
            completion.insert_text = string_field(item.get("textEdit"), "newText");
        }
        if (!completion.label.empty()) result.completions.push_back(std::move(completion));
    });
    return result;
}

[[nodiscard]] inline auto parse_hover_result(std::string_view json) -> std::expected<LspResult, LspToolError> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    if (!root.is_obj()) return result;
    auto contents = contents_to_text(root.get("contents"));
    if (contents.empty()) return result;
    LspHoverResult hover{.contents = std::move(contents)};
    auto range = root.get("range");
    if (range.is_obj()) hover.range = parse_range(range);
    result.hover = std::move(hover);
    return result;
}

[[nodiscard]] inline auto parse_symbols_result(std::string_view json, const std::optional<std::string>& query)
    -> std::expected<LspResult, LspToolError>
{
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    if (root.is_arr()) {
        root.iter([&](cc::utils::json::JsonVal item) {
            append_symbol(item, result.symbols, query);
        });
    } else {
        append_symbol(root, result.symbols, query);
    }
    return result;
}

[[nodiscard]] inline auto build_text_document_params(std::string_view uri) -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(uri));
    root.add("textDocument", text_document);
    doc.set_root(root);
    return doc.to_string();
}

[[nodiscard]] inline auto build_position_params(std::string_view uri, LspPosition position) -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(uri));
    root.add("textDocument", text_document);
    auto pos = doc.object();
    pos.add("line", doc.number(static_cast<int64_t>(position.line)));
    pos.add("character", doc.number(static_cast<int64_t>(position.character)));
    root.add("position", pos);
    doc.set_root(root);
    return doc.to_string();
}

[[nodiscard]] inline auto build_references_params(std::string_view uri, LspPosition position) -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(uri));
    root.add("textDocument", text_document);
    auto pos = doc.object();
    pos.add("line", doc.number(static_cast<int64_t>(position.line)));
    pos.add("character", doc.number(static_cast<int64_t>(position.character)));
    root.add("position", pos);
    auto context = doc.object();
    context.add("includeDeclaration", doc.boolean(true));
    root.add("context", context);
    doc.set_root(root);
    return doc.to_string();
}

} // namespace detail

// LspTool - unified interface for LSP operations
class LspTool {
public:
    static constexpr std::string_view name = "lsp";
    static constexpr std::string_view description = "Query language server for code intelligence";

    LspTool() : manager_(cc::services::lsp::create_lsp_server_manager()) {}

    explicit LspTool(std::unique_ptr<cc::services::lsp::LSPServerManager> manager)
        : manager_(std::move(manager)) {}

    // Validate request before execution
    auto validate(const LspRequest& request) const -> std::expected<void, LspToolError> {
        if (request.file_path.empty()) {
            return std::unexpected(LspToolError::FilePathEmpty);
        }

        // Position is required for certain actions
        bool needs_position = (request.action == LspAction::Definition
                            || request.action == LspAction::References
                            || request.action == LspAction::Completion
                            || request.action == LspAction::Hover);

        if (needs_position && !request.position) {
            return std::unexpected(LspToolError::InvalidAction);
        }
        return {};
    }

    // Convert file path to URI format
    static auto path_to_uri(const std::filesystem::path& path) -> std::string {
        return cc::services::lsp::LSPServerManager::file_uri_for_path(path.string());
    }

    // Execute LSP request (delegates to appropriate handler)
    auto execute(LspRequest request) -> std::expected<LspResult, LspToolError> {
        if (auto valid = validate(request); !valid) {
            return std::unexpected(valid.error());
        }

        if (!connected_ || !manager_) {
            return std::unexpected(LspToolError::ServerNotConnected);
        }

        request.file_path = std::filesystem::absolute(request.file_path);
        auto content = detail::read_file_text(request.file_path);
        if (!content) return std::unexpected(content.error());

        auto opened = manager_->open_file(request.file_path.string(), *content);
        if (!opened) return std::unexpected(map_error(opened.error()));

        auto uri = path_to_uri(request.file_path);

        switch (request.action) {
            case LspAction::Diagnostics:
                return handle_diagnostics(request.file_path);
            case LspAction::Definition:
                return handle_definition(request.file_path, uri, *request.position);
            case LspAction::References:
                return handle_references(request.file_path, uri, *request.position);
            case LspAction::Completion:
                return handle_completion(request.file_path, uri, *request.position);
            case LspAction::Hover:
                return handle_hover(request.file_path, uri, *request.position);
            case LspAction::Symbols:
                return handle_symbols(request.file_path, uri, request.query);
        }
        return std::unexpected(LspToolError::InvalidAction);
    }

    // Connection management
    void set_connected(bool connected) { connected_ = connected; }
    [[nodiscard]] bool is_connected() const { return connected_; }

    // Generate JSON schema for LLM tool invocation
    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["diagnostics", "definition", "references", "completion", "hover", "symbols"] }},
      "file_path": {{ "type": "string", "description": "Absolute path to the file" }},
      "line": {{ "type": "integer", "description": "Line number (0-based)" }},
      "character": {{ "type": "integer", "description": "Character offset (0-based)" }},
      "query": {{ "type": "string", "description": "Filter query for symbols" }}
    }},
    "required": ["action", "file_path"]
  }}
}})json", name, description);
    }

private:
    bool connected_{true};
    std::unique_ptr<cc::services::lsp::LSPServerManager> manager_;

    [[nodiscard]] static auto map_error(const cc::utils::Error& error) -> LspToolError {
        using cc::utils::ErrorCode;
        switch (error.code()) {
            case ErrorCode::timeout: return LspToolError::Timeout;
            case ErrorCode::parse_error: return LspToolError::ParseError;
            case ErrorCode::not_found:
            case ErrorCode::unavailable: return LspToolError::ServerNotConnected;
            default: return LspToolError::RequestFailed;
        }
    }

    auto send_request(
        const std::filesystem::path& file_path,
        std::string_view method,
        std::string params) -> std::expected<std::string, LspToolError>
    {
        auto response = manager_->send_request<std::string>(file_path.string(), std::string(method), std::move(params));
        if (!response) return std::unexpected(map_error(response.error()));
        return *response;
    }

    auto handle_diagnostics(const std::filesystem::path& file_path) -> std::expected<LspResult, LspToolError> {
        auto polled = manager_->poll_file(file_path.string(), std::chrono::milliseconds{250});
        if (!polled) return std::unexpected(map_error(polled.error()));
        auto diagnostics = manager_->diagnostics_json_for_file(file_path.string());
        if (!diagnostics) return std::unexpected(map_error(diagnostics.error()));
        return detail::parse_diagnostics(*diagnostics);
    }

    auto handle_definition(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/definition", detail::build_position_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_locations_result(*response);
    }

    auto handle_references(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/references", detail::build_references_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_locations_result(*response);
    }

    auto handle_completion(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/completion", detail::build_position_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_completion_result(*response);
    }

    auto handle_hover(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/hover", detail::build_position_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_hover_result(*response);
    }

    auto handle_symbols(const std::filesystem::path& file_path, const std::string& uri, std::optional<std::string> query)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/documentSymbol", detail::build_text_document_params(uri));
        if (!response) return std::unexpected(response.error());
        return detail::parse_symbols_result(*response, query);
    }
};

} // namespace cc::tools
