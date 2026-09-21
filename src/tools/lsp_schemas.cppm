/// @file lsp_schemas.cppm
/// @brief LSP method schema definitions for code intelligence operations.
/// Defines supported LSP methods with their parameters and result types.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>
#include <array>

export module cc.tools.lsp_schemas;

export namespace cc::tools::lsp_schemas {

/// Schema definition for a single LSP method
struct LSPSchema {
    std::string method;
    std::string description;
    std::vector<std::string> required_params;
    std::optional<std::string> result_type;
};

/// All supported LSP method schemas
[[nodiscard]] inline std::vector<LSPSchema> get_supported_schemas() {
    return {
        // Document synchronization
        {
            .method = "textDocument/didOpen",
            .description = "Notify server that a document was opened",
            .required_params = {"textDocument.uri", "textDocument.languageId", "textDocument.text"},
            .result_type = std::nullopt,
        },
        {
            .method = "textDocument/didChange",
            .description = "Notify server of document content changes",
            .required_params = {"textDocument.uri", "contentChanges"},
            .result_type = std::nullopt,
        },
        {
            .method = "textDocument/didClose",
            .description = "Notify server that a document was closed",
            .required_params = {"textDocument.uri"},
            .result_type = std::nullopt,
        },

        // Language features
        {
            .method = "textDocument/completion",
            .description = "Request code completions at a position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "CompletionList",
        },
        {
            .method = "textDocument/hover",
            .description = "Request hover information at a position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Hover",
        },
        {
            .method = "textDocument/definition",
            .description = "Go to definition of symbol at position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Location | Location[]",
        },
        {
            .method = "textDocument/references",
            .description = "Find all references to symbol at position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Location[]",
        },
        {
            .method = "textDocument/documentSymbol",
            .description = "List all symbols in a document",
            .required_params = {"textDocument.uri"},
            .result_type = "DocumentSymbol[] | SymbolInformation[]",
        },
        {
            .method = "textDocument/codeAction",
            .description = "Request code actions (quick fixes, refactoring) for a range",
            .required_params = {"textDocument.uri", "range", "context.diagnostics"},
            .result_type = "CodeAction[]",
        },
        {
            .method = "textDocument/formatting",
            .description = "Format entire document",
            .required_params = {"textDocument.uri", "options.tabSize", "options.insertSpaces"},
            .result_type = "TextEdit[]",
        },
        {
            .method = "textDocument/rangeFormatting",
            .description = "Format a range within a document",
            .required_params = {"textDocument.uri", "range", "options.tabSize"},
            .result_type = "TextEdit[]",
        },
        {
            .method = "textDocument/rename",
            .description = "Rename a symbol across the workspace",
            .required_params = {"textDocument.uri", "position.line", "position.character", "newName"},
            .result_type = "WorkspaceEdit",
        },
        {
            .method = "textDocument/signatureHelp",
            .description = "Request signature help at a position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "SignatureHelp",
        },
        {
            .method = "textDocument/declaration",
            .description = "Go to declaration of symbol at position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Location | Location[]",
        },
        {
            .method = "textDocument/typeDefinition",
            .description = "Go to type definition of symbol at position",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Location | Location[]",
        },
        {
            .method = "textDocument/implementation",
            .description = "Find implementations of an interface/abstract method",
            .required_params = {"textDocument.uri", "position.line", "position.character"},
            .result_type = "Location[]",
        },

        // Diagnostics
        {
            .method = "textDocument/publishDiagnostics",
            .description = "Server notification of diagnostics for a document",
            .required_params = {"uri", "diagnostics"},
            .result_type = std::nullopt,
        },

        // Workspace
        {
            .method = "workspace/symbol",
            .description = "Search for symbols across the workspace",
            .required_params = {"query"},
            .result_type = "SymbolInformation[]",
        },
        {
            .method = "workspace/executeCommand",
            .description = "Execute a workspace command",
            .required_params = {"command"},
            .result_type = "any",
        },

        // Lifecycle
        {
            .method = "initialize",
            .description = "Initialize the LSP server with capabilities",
            .required_params = {"rootUri", "capabilities"},
            .result_type = "InitializeResult",
        },
        {
            .method = "shutdown",
            .description = "Request server shutdown",
            .required_params = {},
            .result_type = std::nullopt,
        },
    };
}

/// Find schema for a specific LSP method
[[nodiscard]] inline std::optional<LSPSchema> find_schema(std::string_view method) {
    auto schemas = get_supported_schemas();
    for (const auto& schema : schemas) {
        if (schema.method == method) {
            return schema;
        }
    }
    return std::nullopt;
}

/// Check if a method is supported
[[nodiscard]] inline bool is_supported_method(std::string_view method) {
    return find_schema(method).has_value();
}

/// Get all supported method names
[[nodiscard]] inline std::vector<std::string> get_all_methods() {
    auto schemas = get_supported_schemas();
    std::vector<std::string> methods;
    methods.reserve(schemas.size());
    for (const auto& schema : schemas) {
        methods.push_back(schema.method);
    }
    return methods;
}

/// Get methods by category prefix (e.g., "textDocument/", "workspace/")
[[nodiscard]] inline std::vector<LSPSchema> get_methods_by_prefix(std::string_view prefix) {
    auto schemas = get_supported_schemas();
    std::vector<LSPSchema> filtered;
    for (auto& schema : schemas) {
        if (schema.method.starts_with(prefix)) {
            filtered.push_back(std::move(schema));
        }
    }
    return filtered;
}

} // namespace cc::tools::lsp_schemas
