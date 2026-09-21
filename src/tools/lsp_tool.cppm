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

// LSP action types. The action set mirrors the TS LSP tool operation union
// (src/tools/LSPTool/schemas.ts:180-190): goToDefinition, findReferences,
// hover, documentSymbol, workspaceSymbol, goToImplementation,
// prepareCallHierarchy, incomingCalls, outgoingCalls. The C++ port keeps two
// additional helpers (Diagnostics, Completion) that are not part of the TS
// operation union but are exercised by runtime_registry.cppm; they are left in
// place so existing call sites keep working.
enum class LspAction {
    Diagnostics,
    Definition,
    References,
    Completion,
    Hover,
    Symbols,
    Implementation,        // TS: goToImplementation  -> textDocument/implementation
    WorkspaceSymbol,       // TS: workspaceSymbol     -> workspace/symbol
    PrepareCallHierarchy,  // TS: prepareCallHierarchy -> textDocument/prepareCallHierarchy
    IncomingCalls,         // TS: incomingCalls        -> callHierarchy/incomingCalls (two-step)
    OutgoingCalls,         // TS: outgoingCalls        -> callHierarchy/outgoingCalls (two-step)
};

constexpr auto lsp_action_name(LspAction a) -> std::string_view {
    switch (a) {
        case LspAction::Diagnostics:            return "diagnostics";
        case LspAction::Definition:             return "definition";
        case LspAction::References:             return "references";
        case LspAction::Completion:             return "completion";
        case LspAction::Hover:                  return "hover";
        case LspAction::Symbols:                return "symbols";
        case LspAction::Implementation:         return "implementation";
        case LspAction::WorkspaceSymbol:        return "workspaceSymbol";
        case LspAction::PrepareCallHierarchy:   return "prepareCallHierarchy";
        case LspAction::IncomingCalls:          return "incomingCalls";
        case LspAction::OutgoingCalls:          return "outgoingCalls";
        default:                                return "unknown";
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
    LspPosition start{};
    LspPosition end{};
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

// CallHierarchyItem (LSP 3.16). Used by prepareCallHierarchy and as the
// from/to peer of incoming/outgoing call edges.
struct LspCallItem {
    std::string name;
    std::string kind = {};        // human-readable SymbolKind name
    std::string uri = {};
    std::string detail = {};
    std::optional<std::string> tags = std::nullopt; // raw tag array string when present
    LspRange range{};
    LspRange selection_range{};
    std::optional<std::string> data_json = std::nullopt; // opaque server payload
};

// CallHierarchyIncomingCall / OutgoingCall edge. For incoming, `peer` is the
// `from` item and `ranges` are `fromRanges`; for outgoing, `peer` is the `to`
// item and `ranges` are `toRanges` (see vscode-languageserver-types).
struct LspCallEdge {
    LspCallItem peer;
    std::vector<LspRange> ranges;
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
    std::optional<LspPosition> position = std::nullopt; // Required for definition, references, completion, hover
    std::optional<std::string> query = std::nullopt;    // Optional filter for symbols
};

// Unified LSP result (variant of all possible response types)
struct LspResult {
    std::vector<LspDiagnostic> diagnostics;
    std::vector<LspLocation> locations;
    std::vector<LspCompletionItem> completions;
    std::vector<LspSymbol> symbols;
    std::optional<LspHoverResult> hover;
    std::vector<LspCallItem> call_items;   // prepareCallHierarchy result
    std::vector<LspCallEdge> call_edges;   // incomingCalls / outgoingCalls result

    [[nodiscard]] bool empty() const {
        return diagnostics.empty() && locations.empty()
            && completions.empty() && symbols.empty() && !hover
            && call_items.empty() && call_edges.empty();
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
    LspHoverResult hover{
        .contents = std::move(contents),
        .range = std::nullopt
    };
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

// Parse a single CallHierarchyItem (LSP 3.16 shape).
[[nodiscard]] inline auto parse_call_item(cc::utils::json::JsonVal value) -> std::optional<LspCallItem> {
    if (!value.is_obj()) return std::nullopt;
    auto uri = value.get("uri");
    if (!uri.is_str()) return std::nullopt;
    LspCallItem item;
    item.name = string_field(value, "name");
    item.kind = symbol_kind_name(number_field(value, "kind"));
    item.uri = std::string(uri.as_str());
    item.detail = string_field(value, "detail");
    item.range = parse_range(value.get("range"));
    item.selection_range = parse_range(value.get("selectionRange"));
    auto tags = value.get("tags");
    if (tags.is_arr()) item.tags = tags.to_string();
    auto data = value.get("data");
    if (data.valid()) item.data_json = data.to_string();
    return item;
}

// Parse the CallHierarchyItem[] result of textDocument/prepareCallHierarchy.
[[nodiscard]] inline auto parse_call_items_result(std::string_view json) -> std::expected<LspResult, LspToolError> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    if (!root.is_arr()) {
        // A single object or null/missing -> degrade gracefully.
        if (auto single = parse_call_item(root)) result.call_items.push_back(std::move(*single));
        return result;
    }
    root.iter([&](cc::utils::json::JsonVal item) {
        if (auto parsed_item = parse_call_item(item)) result.call_items.push_back(std::move(*parsed_item));
    });
    return result;
}

// Parse callHierarchy/incomingCalls (CallHierarchyIncomingCall[]) and
// callHierarchy/outgoingCalls (CallHierarchyOutgoingCall[]). The two shapes
// differ only in the peer key (`from` vs `to`) and the ranges key
// (`fromRanges` vs `toRanges`); both are handled here.
[[nodiscard]] inline auto parse_call_edges_result(std::string_view json, bool incoming)
    -> std::expected<LspResult, LspToolError>
{
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::unexpected(LspToolError::ParseError);
    LspResult result;
    auto root = parsed->root();
    auto peer_key = incoming ? "from" : "to";
    auto ranges_key = incoming ? "fromRanges" : "toRanges";
    if (!root.is_arr()) {
        if (root.is_obj()) {
            LspCallEdge edge;
            if (auto peer = parse_call_item(root.get(peer_key))) edge.peer = std::move(*peer);
            auto ranges = root.get(ranges_key);
            ranges.iter([&](cc::utils::json::JsonVal r) {
                edge.ranges.push_back(parse_range(r));
            });
            result.call_edges.push_back(std::move(edge));
        }
        return result;
    }
    root.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        LspCallEdge edge;
        if (auto peer = parse_call_item(item.get(peer_key))) edge.peer = std::move(*peer);
        auto ranges = item.get(ranges_key);
        ranges.iter([&](cc::utils::json::JsonVal r) {
            edge.ranges.push_back(parse_range(r));
        });
        result.call_edges.push_back(std::move(edge));
    });
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

// workspace/symbol uses an empty query to return all symbols (mirrors TS
// LSPTool.ts:471-477: `params: { query: '' }`).
[[nodiscard]] inline auto build_workspace_symbol_params() -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("query", doc.string(""));
    doc.set_root(root);
    return doc.to_string();
}

// callHierarchy/incomingCalls and callHierarchy/outgoingCalls take a single
// CallHierarchyItem under the `item` key (TS LSPTool.ts:324-326). We re-emit
// the previously-returned item JSON verbatim so server-supplied fields like
// `data` survive the round-trip.
[[nodiscard]] inline auto build_call_hierarchy_request_params(const std::string& item_json) -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto item = doc.raw_json(item_json.empty() ? "null" : item_json);
    root.add("item", item.valid() ? item : doc.null());
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

        // Position is required for position-based actions. Mirrors TS schemas.ts
        // where every operation except workspaceSymbol requires line/character
        // (workspaceSymbol still carries them in the schema but ignores them).
        bool needs_position = (request.action == LspAction::Definition
                            || request.action == LspAction::References
                            || request.action == LspAction::Completion
                            || request.action == LspAction::Hover
                            || request.action == LspAction::Implementation
                            || request.action == LspAction::PrepareCallHierarchy
                            || request.action == LspAction::IncomingCalls
                            || request.action == LspAction::OutgoingCalls);

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
            case LspAction::Implementation:
                return handle_implementation(request.file_path, uri, *request.position);
            case LspAction::WorkspaceSymbol:
                return handle_workspace_symbol(request.file_path, request.query);
            case LspAction::PrepareCallHierarchy:
                return handle_prepare_call_hierarchy(request.file_path, uri, *request.position);
            case LspAction::IncomingCalls:
                return handle_call_direction(request.file_path, uri, *request.position, /*incoming=*/true);
            case LspAction::OutgoingCalls:
                return handle_call_direction(request.file_path, uri, *request.position, /*incoming=*/false);
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
      "action": {{ "type": "string", "enum": ["diagnostics", "definition", "references", "completion", "hover", "symbols", "implementation", "workspaceSymbol", "prepareCallHierarchy", "incomingCalls", "outgoingCalls"] }},
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

    // goToImplementation reuses the Location/LocationLink parser, mirroring TS
    // LSPTool.ts:755-792 (which reuses the goToDefinition formatter).
    auto handle_implementation(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/implementation", detail::build_position_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_locations_result(*response);
    }

    // workspace/symbol with an empty query returns all symbols. TS LSPTool.ts
    // does not require a file path for the request itself, but the schema
    // keeps filePath mandatory, so we honour it here for parity.
    auto handle_workspace_symbol(const std::filesystem::path& file_path, std::optional<std::string> query)
        -> std::expected<LspResult, LspToolError>
    {
        // Manager picks the server by file extension; file_path is still used
        // to route the request even though the params omit it.
        auto response = send_request(file_path, "workspace/symbol", detail::build_workspace_symbol_params());
        if (!response) return std::unexpected(response.error());
        return detail::parse_symbols_result(*response, query);
    }

    auto handle_prepare_call_hierarchy(const std::filesystem::path& file_path, const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        auto response = send_request(file_path, "textDocument/prepareCallHierarchy", detail::build_position_params(uri, pos));
        if (!response) return std::unexpected(response.error());
        return detail::parse_call_items_result(*response);
    }

    // incoming/outgoing calls are a two-step flow (TS LSPTool.ts:299-334):
    // prepareCallHierarchy first, then callHierarchy/incomingCalls or
    // /outgoingCalls with `{ item: callItems[0] }`.
    auto handle_call_direction(
        const std::filesystem::path& file_path,
        const std::string& uri,
        LspPosition pos,
        bool incoming) -> std::expected<LspResult, LspToolError>
    {
        auto prepared = send_request(file_path, "textDocument/prepareCallHierarchy", detail::build_position_params(uri, pos));
        if (!prepared) return std::unexpected(prepared.error());
        auto items_result = detail::parse_call_items_result(*prepared);
        if (!items_result) return std::unexpected(items_result.error());
        if (items_result->call_items.empty()) {
            return LspResult{};  // TS: "No call hierarchy item found at this position"
        }

        // Reuse the server-supplied item JSON so opaque `data` survives. We
        // rebuild a minimal item JSON from the first parsed item.
        const auto& first = items_result->call_items.front();
        std::string item_json = build_call_item_json(first);

        auto method = incoming ? "callHierarchy/incomingCalls" : "callHierarchy/outgoingCalls";
        auto response = send_request(file_path, method, detail::build_call_hierarchy_request_params(item_json));
        if (!response) return std::unexpected(response.error());
        return detail::parse_call_edges_result(*response, incoming);
    }

    // Render a LspCallItem back to JSON for the { item: ... } request payload.
    [[nodiscard]] static auto build_call_item_json(const LspCallItem& item) -> std::string {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        root.add("name", doc.string(item.name));
        root.add("kind", doc.number(static_cast<int64_t>(parse_symbol_kind_value(item.kind))));
        root.add("uri", doc.string(item.uri));
        if (!item.detail.empty()) root.add("detail", doc.string(item.detail));
        root.add("range", build_range_json(doc, item.range));
        root.add("selectionRange", build_range_json(doc, item.selection_range));
        if (item.data_json) {
            auto raw = doc.raw_json(*item.data_json);
            if (raw.valid()) root.add("data", raw);
        }
        doc.set_root(root);
        return doc.to_string();
    }

    [[nodiscard]] static auto build_range_json(cc::utils::json::JsonMutDoc& doc, const LspRange& range)
        -> cc::utils::json::JsonMutVal
    {
        auto obj = doc.object();
        auto start = doc.object();
        start.add("line", doc.number(static_cast<int64_t>(range.start.line)));
        start.add("character", doc.number(static_cast<int64_t>(range.start.character)));
        obj.add("start", start);
        auto end = doc.object();
        end.add("line", doc.number(static_cast<int64_t>(range.end.line)));
        end.add("character", doc.number(static_cast<int64_t>(range.end.character)));
        obj.add("end", end);
        return obj;
    }

    // Inverse of detail::symbol_kind_name for round-tripping CallHierarchyItem.kind.
    [[nodiscard]] static auto parse_symbol_kind_value(std::string_view name) -> int {
        static constexpr std::array<std::string_view, 27> names{
            "unknown", "file", "module", "namespace", "package", "class", "method",
            "property", "field", "constructor", "enum", "interface", "function",
            "variable", "constant", "string", "number", "boolean", "array",
            "object", "key", "null", "enum_member", "struct", "event", "operator",
            "type_parameter",
        };
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) return static_cast<int>(i);
        }
        return 0;  // unknown
    }
};

} // namespace cc::tools
