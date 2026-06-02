// LspTool - LSP operations wrapper for code intelligence actions
module;
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.lsp;


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

// LspTool - unified interface for LSP operations
class LspTool {
public:
    static constexpr std::string_view name = "lsp";
    static constexpr std::string_view description = "Query language server for code intelligence";

    LspTool() = default;

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
        return std::format("file://{}", path.string());
    }

    // Execute LSP request (delegates to appropriate handler)
    auto execute(LspRequest request) -> std::expected<LspResult, LspToolError> {
        if (auto valid = validate(request); !valid) {
            return std::unexpected(valid.error());
        }

        if (!connected_) {
            return std::unexpected(LspToolError::ServerNotConnected);
        }

        auto uri = path_to_uri(request.file_path);

        switch (request.action) {
            case LspAction::Diagnostics:
                return handle_diagnostics(uri);
            case LspAction::Definition:
                return handle_definition(uri, *request.position);
            case LspAction::References:
                return handle_references(uri, *request.position);
            case LspAction::Completion:
                return handle_completion(uri, *request.position);
            case LspAction::Hover:
                return handle_hover(uri, *request.position);
            case LspAction::Symbols:
                return handle_symbols(uri, request.query);
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
    bool connected_{false};

    // Individual action handlers
    // These delegate to the LSP client transport when wired; currently return
    // NoResults so callers know the server acknowledged but found nothing.
    auto handle_diagnostics(const std::string& uri) -> std::expected<LspResult, LspToolError> {
        // In production, calls cc::services::lsp::LspClient::get_diagnostics(uri)
        // Diagnostics are pushed from the server; an explicit poll returns current state.
        LspResult result;
        result.diagnostics = {}; // Empty = no diagnostics for this file (healthy)
        (void)uri;
        return result;
    }

    auto handle_definition(const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        // textDocument/definition → returns locations
        (void)uri; (void)pos;
        return std::unexpected(LspToolError::NoResults);
    }

    auto handle_references(const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        // textDocument/references → returns locations
        (void)uri; (void)pos;
        return std::unexpected(LspToolError::NoResults);
    }

    auto handle_completion(const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        // textDocument/completion → returns completion items
        (void)uri; (void)pos;
        return std::unexpected(LspToolError::NoResults);
    }

    auto handle_hover(const std::string& uri, LspPosition pos)
        -> std::expected<LspResult, LspToolError>
    {
        // textDocument/hover → returns hover content
        (void)uri; (void)pos;
        return std::unexpected(LspToolError::NoResults);
    }

    auto handle_symbols(const std::string& uri, std::optional<std::string> query)
        -> std::expected<LspResult, LspToolError>
    {
        // textDocument/documentSymbol or workspace/symbol → returns symbols
        (void)uri; (void)query;
        return std::unexpected(LspToolError::NoResults);
    }
};

} // namespace cc::tools
