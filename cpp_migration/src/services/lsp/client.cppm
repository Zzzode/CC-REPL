/// @file lsp/client.cppm
/// @brief LSP Client - Language Server Protocol client with JSON-RPC 2.0 transport
/// Implements LSP client capabilities including document sync, completion,
/// go-to-definition, hover, diagnostics, and more.
module;

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <expected>
#include <functional>
#include <thread>
#include <atomic>
#include <sstream>
#include <optional>
#include <variant>
#include <format>
#include <filesystem>
#include <future>

export module cc.services.lsp.client;

import cc.utils.json;
import cc.types.types;
import cc.utils.bash_execution;

export namespace cc::services::lsp {

using namespace cc::utils::json;
using namespace cc::core;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// =========================================================================
// LSP Error Code
// =========================================================================

enum class LspErrorCode : int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    ServerNotInitialized = -32002,
    UnknownErrorCode = -32001,
    RequestFailed = -32803,
    ServerCancelled = -32802,
    ContentModified = -32801,
    RequestCancelled = -32800,
};

// =========================================================================
// LSP Types
// =========================================================================

struct Position {
    int64_t line = 0;
    int64_t character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct TextDocumentIdentifier {
    std::string uri;
};

struct VersionedTextDocumentIdentifier {
    std::string uri;
    int64_t version = 0;
};

struct TextDocumentItem {
    std::string uri;
    std::string language_id;
    int64_t version = 0;
    std::string text;
};

struct TextEdit {
    Range range;
    std::string new_text;
};

struct TextDocumentContentChangeEvent {
    std::optional<Range> range;
    std::optional<int64_t> range_length;
    std::string text;
};

struct Location {
    std::string uri;
    Range range;
};

struct LocationLink {
    std::optional<std::string> origin_selection_range;
    std::string target_uri;
    Range target_range;
    Range target_selection_range;
};

struct Diagnostic {
    Range range;
    int32_t severity = 1; // 1: Error, 2: Warning, 3: Info, 4: Hint
    std::optional<int64_t> code;
    std::optional<std::string> code_description;
    std::optional<std::string> source;
    std::string message;
    std::optional<std::vector<std::string>> tags;
    std::optional<std::vector<Location>> related_information;
    std::optional<std::string> data;
};

struct CompletionItem {
    std::string label;
    std::optional<int32_t> kind;
    std::vector<std::string> tags;
    std::optional<std::string> detail;
    std::optional<std::string> documentation;
    std::optional<bool> deprecated;
    std::optional<bool> preselect;
    std::optional<std::string> sort_text;
    std::optional<std::string> filter_text;
    std::optional<std::string> insert_text;
    std::optional<int32_t> insert_text_format;
    std::optional<TextEdit> text_edit;
    std::optional<std::vector<TextEdit>> additional_text_edits;
    std::optional<std::string> commit_characters;
    std::optional<std::string> command;
    std::optional<std::string> data;
};

struct CompletionList {
    bool is_incomplete = false;
    std::optional<std::vector<std::string>> item_defaults;
    std::vector<CompletionItem> items;
};

struct Hover {
    std::variant<std::string, std::vector<std::string>> contents;
    std::optional<Range> range;
};

struct SignatureHelp {
    std::vector<std::string> signatures;
    std::optional<int64_t> active_signature;
    std::optional<int64_t> active_parameter;
};

struct DocumentSymbol {
    std::string name;
    std::optional<std::string> detail;
    int32_t kind = 0;
    std::vector<int32_t> tags;
    std::optional<bool> deprecated;
    Range range;
    Range selection_range;
    std::optional<std::vector<DocumentSymbol>> children;
};

struct CodeActionContext {
    std::vector<Diagnostic> diagnostics;
    std::optional<std::vector<std::string>> only;
    std::optional<std::string> trigger_kind;
};

struct CodeAction {
    std::string title;
    std::optional<std::string> kind;
    std::optional<std::vector<Diagnostic>> diagnostics;
    std::optional<bool> is_preferred;
    std::optional<bool> disabled;
    std::optional<std::string> edit;
    std::optional<std::string> command;
    std::optional<std::string> data;
};

// =========================================================================
// Server Capabilities
// =========================================================================

struct TextDocumentSyncClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<bool> will_save;
    std::optional<bool> will_save_wait_until;
    std::optional<bool> did_save;
};

struct CompletionClientCapabilities {
    std::optional<bool> dynamic_registration;
    struct CompletionItemCapabilities {
        std::optional<bool> snippet_support;
        std::optional<bool> commit_characters_support;
        std::optional<std::vector<std::string>> documentation_format;
        std::optional<bool> deprecated_support;
        std::optional<bool> preselect_support;
        std::optional<std::vector<std::string>> tag_support;
        std::optional<bool> insert_replace_support;
        std::optional<bool> resolve_support;
        std::optional<bool> insert_text_mode_support;
        std::optional<bool> label_details_support;
    };
    std::optional<CompletionItemCapabilities> completion_item;
    struct CompletionItemKindCapabilities {
        std::optional<std::vector<int32_t>> value_set;
    };
    std::optional<CompletionItemKindCapabilities> completion_item_kind;
    std::optional<bool> context_support;
};

struct HoverClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<std::vector<std::string>> content_format;
};

struct SignatureHelpClientCapabilities {
    std::optional<bool> dynamic_registration;
    struct SignatureInformationCapabilities {
        std::optional<std::vector<std::string>> documentation_format;
        struct ParameterInformationCapabilities {
            std::optional<bool> label_offset_support;
        };
        std::optional<ParameterInformationCapabilities> parameter_information;
        std::optional<bool> active_parameter_support;
    };
    std::optional<SignatureInformationCapabilities> signature_information;
    std::optional<bool> context_support;
};

struct DeclarationClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<bool> link_support;
};

struct DefinitionClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<bool> link_support;
};

struct TypeDefinitionClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<bool> link_support;
};

struct ImplementationClientCapabilities {
    std::optional<bool> dynamic_registration;
    std::optional<bool> link_support;
};

struct ReferencesClientCapabilities {
    std::optional<bool> dynamic_registration;
};

struct DocumentHighlightClientCapabilities {
    std::optional<bool> dynamic_registration;
};

struct DocumentSymbolClientCapabilities {
    std::optional<bool> dynamic_registration;
    struct SymbolKindCapabilities {
        std::optional<std::vector<int32_t>> value_set;
    };
    std::optional<SymbolKindCapabilities> symbol_kind;
    std::optional<bool> hierarchical_document_symbol_support;
    std::optional<bool> tag_support;
    std::optional<bool> label_support;
};

struct WorkspaceSymbolClientCapabilities {
    std::optional<bool> dynamic_registration;
    struct SymbolKindCapabilities {
        std::optional<std::vector<int32_t>> value_set;
    };
    std::optional<SymbolKindCapabilities> symbol_kind;
    std::optional<bool> tag_support;
    std::optional<bool> resolve_support;
};

struct CodeActionClientCapabilities {
    std::optional<bool> dynamic_registration;
    struct CodeActionLiteralSupport {
        struct CodeActionKindCapabilities {
            std::vector<std::string> value_set;
        };
        CodeActionKindCapabilities code_action_kind;
    };
    std::optional<CodeActionLiteralSupport> code_action_literal_support;
    std::optional<bool> is_preferred_support;
    std::optional<bool> disabled_support;
    std::optional<bool> data_support;
    std::optional<bool> resolve_support;
    std::optional<bool> honors_change_annotations;
};

struct ClientCapabilities {
    struct WorkspaceClientCapabilities {
        std::optional<bool> apply_edit;
        std::optional<std::string> workspace_edit;
        std::optional<bool> did_change_configuration;
        std::optional<bool> did_change_watched_files;
        std::optional<bool> symbol;
        std::optional<bool> execute_command;
        std::optional<bool> workspace_folders;
        std::optional<bool> configuration;
        std::optional<bool> semantic_tokens;
        std::optional<bool> code_lens;
        std::optional<bool> file_operations;
        std::optional<bool> inline_value;
        std::optional<bool> inlay_hint;
        std::optional<bool> diagnostics;
    };
    std::optional<WorkspaceClientCapabilities> workspace;

    struct TextDocumentClientCapabilities {
        std::optional<TextDocumentSyncClientCapabilities> synchronization;
        std::optional<CompletionClientCapabilities> completion;
        std::optional<HoverClientCapabilities> hover;
        std::optional<SignatureHelpClientCapabilities> signature_help;
        std::optional<DeclarationClientCapabilities> declaration;
        std::optional<DefinitionClientCapabilities> definition;
        std::optional<TypeDefinitionClientCapabilities> type_definition;
        std::optional<ImplementationClientCapabilities> implementation;
        std::optional<ReferencesClientCapabilities> references;
        std::optional<DocumentHighlightClientCapabilities> document_highlight;
        std::optional<DocumentSymbolClientCapabilities> document_symbol;
        std::optional<CodeActionClientCapabilities> code_action;
        std::optional<bool> code_lens;
        std::optional<bool> document_link;
        std::optional<bool> color_provider;
        std::optional<bool> formatting;
        std::optional<bool> range_formatting;
        std::optional<bool> on_type_formatting;
        std::optional<bool> rename;
        std::optional<bool> publish_diagnostics;
        std::optional<bool> folding_range;
        std::optional<bool> selection_range;
        std::optional<bool> semantic_tokens;
        std::optional<bool> moniker;
        std::optional<bool> linked_editing_range;
        std::optional<bool> call_hierarchy;
        std::optional<bool> type_hierarchy;
        std::optional<bool> document_link_dynamic_registration;
        std::optional<bool> inline_value;
        std::optional<bool> inlay_hint;
        std::optional<bool> inline_completion;
        std::optional<bool> diagnostics;
    };
    std::optional<TextDocumentClientCapabilities> text_document;

    struct WindowClientCapabilities {
        std::optional<bool> show_message;
        std::optional<bool> show_message_request;
        std::optional<bool> show_document;
        std::optional<bool> work_done_progress;
    };
    std::optional<WindowClientCapabilities> window;

    struct GeneralClientCapabilities {
        std::optional<std::vector<std::string>> regular_expressions;
        std::optional<std::string> markdown;
        std::optional<bool> position_encodings;
        std::optional<bool> stale_request_support;
    };
    std::optional<GeneralClientCapabilities> general;

    std::optional<bool> notebook_document;
};

struct ServerCapabilities {
    std::optional<std::variant<int64_t, std::string>> text_document_sync;
    std::optional<std::string> completion_provider;
    std::optional<bool> hover_provider;
    std::optional<std::string> signature_help_provider;
    std::optional<bool> declaration_provider;
    std::optional<bool> definition_provider;
    std::optional<bool> type_definition_provider;
    std::optional<bool> implementation_provider;
    std::optional<bool> references_provider;
    std::optional<bool> document_highlight_provider;
    std::optional<bool> document_symbol_provider;
    std::optional<std::string> code_action_provider;
    std::optional<std::string> code_lens_provider;
    std::optional<std::string> document_link_provider;
    std::optional<bool> color_provider;
    std::optional<bool> document_formatting_provider;
    std::optional<bool> document_range_formatting_provider;
    std::optional<std::string> document_on_type_formatting_provider;
    std::optional<std::string> rename_provider;
    std::optional<std::string> folding_range_provider;
    std::optional<bool> execute_command_provider;
    std::optional<bool> selection_range_provider;
    std::optional<std::string> semantic_tokens_provider;
    std::optional<bool> moniker_provider;
    std::optional<std::string> linked_editing_range_provider;
    std::optional<bool> call_hierarchy_provider;
    std::optional<bool> type_hierarchy_provider;
    std::optional<std::string> inline_value_provider;
    std::optional<std::string> inlay_hint_provider;
    std::optional<std::string> inline_completion_provider;
    std::optional<std::string> diagnostic_provider;
    std::optional<std::string> workspace;
    std::optional<std::string> experimental;
};

// =========================================================================
// LSP Result Types
// =========================================================================

struct InitializeResult {
    ServerCapabilities capabilities;
    std::optional<std::string> server_info;
};

// =========================================================================
// LSP Client Error
// =========================================================================

enum class LspClientError {
    NotConnected,
    AlreadyConnected,
    ConnectionFailed,
    ServerCrashed,
    Timeout,
    InvalidResponse,
    ParseError,
    NotInitialized,
    RequestFailed,
    TransportError,
    ServerClosed,
};

// Result type for LSP operations
template<typename T>
using LspResult = std::expected<T, LspClientError>;

// Callback types
using RequestCallback = std::function<void(const std::string& response, std::optional<LspClientError> error)>;
using NotificationCallback = std::function<void(const std::string& method, const std::string& params_json)>;

// =========================================================================
// Transport Interface
// =========================================================================

class ILspTransport {
public:
    virtual ~ILspTransport() = default;
    
    [[nodiscard]] virtual LspResult<void> start() = 0;
    [[nodiscard]] virtual LspResult<void> send(std::string_view message) = 0;
    [[nodiscard]] virtual LspResult<std::string> receive() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;
    virtual void close() = 0;
};

// =========================================================================
// Stdio Transport
// =========================================================================

class StdioTransport : public ILspTransport {
public:
    StdioTransport(std::string command, std::vector<std::string> args,
                   std::map<std::string, std::string> env = {})
        : command_(std::move(command))
        , args_(std::move(args))
        , env_(std::move(env)) {}
    
    ~StdioTransport() override {
        close();
    }
    
    [[nodiscard]] LspResult<void> start() override {
        if (is_connected()) {
            return std::unexpected(LspClientError::AlreadyConnected);
        }
        
        // Build command line
        std::string full_cmd = command_;
        for (const auto& arg : args_) {
            full_cmd += " " + arg;
        }

        // Separate the language server's stderr from the duplex stream. The
        // duplex pipe is a single socketpair-backed FILE*, so without an
        // explicit redirect the server's stderr (diagnostics/logs) is merged
        // into the stream and corrupts JSON-RPC framing. Redirect to a log
        // file when CC_LSP_LOG is set, otherwise discard.
        if (const char* lsp_log = std::getenv("CC_LSP_LOG")) {
            full_cmd += " 2>>";
            full_cmd += lsp_log;
        } else {
            full_cmd += " 2>/dev/null";
        }

        // Set environment variables
        for (const auto& [key, value] : env_) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        // Open bidirectional pipe
        pipe_handle_ = cc::utils::bash::popen_spawn_duplex(full_cmd);
        if (!pipe_handle_) {
            return std::unexpected(LspClientError::ConnectionFailed);
        }
        
        connected_ = true;
        return {};
    }
    
    [[nodiscard]] LspResult<void> send(std::string_view message) override {
        if (!is_connected()) {
            return std::unexpected(LspClientError::NotConnected);
        }
        
        // LSP uses Content-Length header followed by JSON
        auto content_length = std::format("Content-Length: {}\r\n\r\n", message.size());
        if (fputs(content_length.c_str(), pipe_handle_) == EOF) {
            return std::unexpected(LspClientError::TransportError);
        }
        if (fwrite(message.data(), 1, message.size(), pipe_handle_) != message.size()) {
            return std::unexpected(LspClientError::TransportError);
        }
        fflush(pipe_handle_);
        return {};
    }
    
    [[nodiscard]] LspResult<std::string> receive() override {
        if (!is_connected()) {
            return std::unexpected(LspClientError::NotConnected);
        }
        
        // Read Content-Length header. LSP allows any header casing/whitespace
        // and other headers (e.g. Content-Type) may appear; parse defensively.
        char line[512];
        int64_t content_length = 0;
        bool saw_header = false;
        while (fgets(line, sizeof(line), pipe_handle_) != nullptr) {
            std::string_view line_str(line);
            // Strip trailing CR/LF.
            while (!line_str.empty() && (line_str.back() == '\r' || line_str.back() == '\n')) {
                line_str.remove_suffix(1);
            }
            if (line_str.empty()) {
                if (saw_header) break;      // blank line ends the header block
                continue;                   // tolerate leading blank lines
            }
            saw_header = true;
            // Case-insensitive "Content-Length:" prefix.
            constexpr std::string_view kKey = "content-length:";
            if (line_str.size() >= kKey.size()) {
                bool match = true;
                for (std::size_t i = 0; i < kKey.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(line_str[i])) != kKey[i]) { match = false; break; }
                }
                if (match) {
                    auto rest = line_str.substr(kKey.size());
                    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
                    int64_t parsed = 0;
                    bool ok = !rest.empty();
                    for (char c : rest) {
                        if (c < '0' || c > '9') { ok = false; break; }
                        parsed = parsed * 10 + (c - '0');
                    }
                    if (ok) content_length = parsed;
                }
            }
        }

        if (content_length <= 0) {
            if (feof(pipe_handle_)) {
                connected_ = false;
                return std::unexpected(LspClientError::ServerClosed);
            }
            return std::unexpected(LspClientError::ParseError);
        }

        // Read content, tolerating short reads by looping until the full
        // Content-Length has been consumed (stdio may return fewer bytes).
        std::string content;
        content.reserve(static_cast<std::size_t>(content_length));
        std::size_t remaining = static_cast<std::size_t>(content_length);
        std::array<char, 4096> chunk{};
        while (remaining > 0) {
            std::size_t want = std::min(remaining, chunk.size());
            std::size_t got = fread(chunk.data(), 1, want, pipe_handle_);
            if (got == 0) {
                if (feof(pipe_handle_)) {
                    connected_ = false;
                    return std::unexpected(LspClientError::ServerClosed);
                }
                return std::unexpected(LspClientError::TransportError);
            }
            content.append(chunk.data(), got);
            remaining -= got;
        }

        return content;
    }
    
    [[nodiscard]] bool is_connected() const override {
        return connected_ && pipe_handle_ != nullptr;
    }
    
    void close() override {
        if (pipe_handle_) {
            cc::utils::bash::pclose_spawn(pipe_handle_);
            pipe_handle_ = nullptr;
        }
        connected_ = false;
    }
    
private:
    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    FILE* pipe_handle_ = nullptr;
    bool connected_ = false;
};

// =========================================================================
// Pending Request
// =========================================================================

struct PendingRequest {
    int64_t id;
    std::chrono::steady_clock::time_point sent_at;
    std::chrono::milliseconds timeout;
    RequestCallback callback;
    bool completed = false;
};

// =========================================================================
// LSP Client State
// =========================================================================

enum class LspClientState {
    NotStarted,
    Starting,
    Initializing,
    Ready,
    ShuttingDown,
    Stopped,
    Error,
};

// =========================================================================
// LSP Client
// =========================================================================

class LspClient {
public:
    struct Config {
        std::string name;
        std::chrono::milliseconds request_timeout{30000};
        std::chrono::milliseconds init_timeout{60000};
        std::string root_uri;
        ClientCapabilities client_capabilities;
        std::string trace = "off"; // "off", "messages", "verbose"
    };
    
    explicit LspClient(Config config)
        : config_(std::move(config))
        , state_(LspClientState::NotStarted)
        , next_request_id_(1)
        , running_(false) {}
    
    ~LspClient() {
        (void)shutdown();
    }
    
    // Connect to server using stdio transport
    [[nodiscard]] LspResult<void> connect_stdio(
        std::string command, std::vector<std::string> args,
        std::map<std::string, std::string> env = {}) {
        
        if (state_ != LspClientState::NotStarted && state_ != LspClientState::Stopped) {
            return std::unexpected(LspClientError::AlreadyConnected);
        }
        
        state_ = LspClientState::Starting;
        transport_ = std::make_unique<StdioTransport>(std::move(command), std::move(args), std::move(env));
        
        auto result = transport_->start();
        if (!result) {
            state_ = LspClientState::Error;
            return result;
        }
        
        auto init_result = initialize();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        return {};
    }
    
    // Set notification callback
    void set_notification_callback(NotificationCallback callback) {
        notification_callback_ = std::move(callback);
    }
    
    // Set diagnostics callback
    void set_diagnostics_callback(
        std::function<void(const std::string& uri, const std::vector<Diagnostic>& diagnostics)> callback) {
        diagnostics_callback_ = std::move(callback);
    }
    
    // =====================================================================
    // LSP Methods
    // =====================================================================
    
    // Initialize the connection
    [[nodiscard]] LspResult<InitializeResult> initialize() {
        state_ = LspClientState::Initializing;
        
        // Build initialize params
        JsonMutDoc doc;
        auto root = doc.object();
        
        if (!config_.root_uri.empty()) {
            root.add("rootUri", doc.string(config_.root_uri));
            // Remove "file://" prefix for rootPath
            auto root_path_str = config_.root_uri.size() > 7 ? config_.root_uri.substr(7) : config_.root_uri;
            root.add("rootPath", doc.string(root_path_str));
        } else {
            root.add("rootUri", doc.null());
            root.add("rootPath", doc.null());
        }
        
        // Client capabilities
        auto capabilities = doc.object();
        
        // Workspace capabilities
        auto workspace = doc.object();
        workspace.add("applyEdit", doc.boolean(true));
        auto workspace_edit = doc.object();
        workspace_edit.add("documentChanges", doc.boolean(true));
        workspace.add("workspaceEdit", workspace_edit);
        workspace.add("didChangeConfiguration", doc.object());
        workspace.add("didChangeWatchedFiles", doc.object());
        workspace.add("symbol", doc.object());
        workspace.add("executeCommand", doc.object());
        capabilities.add("workspace", workspace);
        
        // Text document capabilities
        auto text_document = doc.object();
        
        auto sync = doc.object();
        sync.add("dynamicRegistration", doc.boolean(false));
        sync.add("willSave", doc.boolean(false));
        sync.add("willSaveWaitUntil", doc.boolean(false));
        sync.add("didSave", doc.boolean(true));
        text_document.add("synchronization", sync);
        
        auto completion = doc.object();
        completion.add("dynamicRegistration", doc.boolean(false));
        auto completion_item = doc.object();
        completion_item.add("snippetSupport", doc.boolean(false));
        completion_item.add("commitCharactersSupport", doc.boolean(true));
        auto doc_format = doc.array();
        doc_format.append(doc.string("markdown"));
        doc_format.append(doc.string("plaintext"));
        completion_item.add("documentationFormat", doc_format);
        completion.add("completionItem", completion_item);
        text_document.add("completion", completion);
        
        auto hover = doc.object();
        hover.add("dynamicRegistration", doc.boolean(false));
        auto hover_format = doc.array();
        hover_format.append(doc.string("markdown"));
        hover_format.append(doc.string("plaintext"));
        hover.add("contentFormat", hover_format);
        text_document.add("hover", hover);
        
        text_document.add("signatureHelp", doc.object());
        text_document.add("declaration", doc.object());
        text_document.add("definition", doc.object());
        text_document.add("typeDefinition", doc.object());
        text_document.add("implementation", doc.object());
        text_document.add("references", doc.object());
        text_document.add("documentHighlight", doc.object());
        text_document.add("documentSymbol", doc.object());
        text_document.add("codeAction", doc.object());
        text_document.add("codeLens", doc.object());
        text_document.add("documentLink", doc.object());
        text_document.add("documentFormatting", doc.object());
        text_document.add("documentRangeFormatting", doc.object());
        text_document.add("rename", doc.object());
        text_document.add("foldingRange", doc.object());
        text_document.add("publishDiagnostics", doc.object());
        
        capabilities.add("textDocument", text_document);
        
        // Window capabilities
        auto window = doc.object();
        window.add("workDoneProgress", doc.boolean(false));
        capabilities.add("window", window);
        
        root.add("capabilities", capabilities);
        
        // Trace
        root.add("trace", doc.string(config_.trace));
        
        // Workspace folders
        root.add("workspaceFolders", doc.null());
        
        doc.set_root(root);

        // Start the receive thread BEFORE issuing the initialize request: the
        // synchronous request waits on a promise that is only fulfilled by the
        // receive loop, so it would otherwise always time out.
        running_ = true;
        receive_thread_ = std::thread(&LspClient::receive_loop, this);

        auto response = send_request_sync("initialize", doc.to_string());
        if (!response) {
            running_ = false;
            if (receive_thread_.joinable()) receive_thread_.join();
            state_ = LspClientState::Error;
            return std::unexpected(response.error());
        }

        // Parse initialize result
        auto init_result = parse_initialize_result(*response);
        if (!init_result) {
            running_ = false;
            if (receive_thread_.joinable()) receive_thread_.join();
            state_ = LspClientState::Error;
            return std::unexpected(LspClientError::InvalidResponse);
        }

        server_capabilities_ = init_result->capabilities;

        // Send initialized notification
        send_notification("initialized", "{}");

        state_ = LspClientState::Ready;
        return *init_result;
    }
    
    // Shutdown the server
    [[nodiscard]] LspResult<void> shutdown() {
        if (state_ == LspClientState::Ready) {
            state_ = LspClientState::ShuttingDown;
            auto result = send_request_sync("shutdown", "null");
            if (result) {
                send_notification("exit", "null");
            }
        }
        
        running_ = false;
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        if (transport_) {
            transport_->close();
        }
        
        state_ = LspClientState::Stopped;
        return {};
    }
    
    // =====================================================================
    // Text Document Methods
    // =====================================================================
    
    // Open a text document
    void did_open(const TextDocumentItem& document) {
        {
            std::lock_guard<std::mutex> lock(documents_mutex_);
            open_documents_[document.uri] = document;
        }
        
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        text_document.add("languageId", doc.string(document.language_id));
        text_document.add("version", doc.number(document.version));
        text_document.add("text", doc.string(document.text));
        root.add("textDocument", text_document);
        
        doc.set_root(root);
        send_notification("textDocument/didOpen", doc.to_string());
    }
    
    // Change a text document
    void did_change(const VersionedTextDocumentIdentifier& document,
                    const std::vector<TextDocumentContentChangeEvent>& changes) {
        {
            std::lock_guard<std::mutex> lock(documents_mutex_);
            auto it = open_documents_.find(document.uri);
            if (it != open_documents_.end()) {
                it->second.version = document.version;
                for (const auto& change : changes) {
                    if (!change.range) {
                        it->second.text = change.text;
                    }
                }
            }
        }
        
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        text_document.add("version", doc.number(document.version));
        root.add("textDocument", text_document);
        
        auto content_changes = doc.array();
        for (const auto& change : changes) {
            auto change_obj = doc.object();
            if (change.range) {
                auto range = doc.object();
                auto start = doc.object();
                start.add("line", doc.number(change.range->start.line));
                start.add("character", doc.number(change.range->start.character));
                auto end = doc.object();
                end.add("line", doc.number(change.range->end.line));
                end.add("character", doc.number(change.range->end.character));
                range.add("start", start);
                range.add("end", end);
                change_obj.add("range", range);
                if (change.range_length) {
                    change_obj.add("rangeLength", doc.number(*change.range_length));
                }
            }
            change_obj.add("text", doc.string(change.text));
            content_changes.append(change_obj);
        }
        root.add("contentChanges", content_changes);
        
        doc.set_root(root);
        send_notification("textDocument/didChange", doc.to_string());
    }
    
    // Close a text document
    void did_close(const TextDocumentIdentifier& document) {
        {
            std::lock_guard<std::mutex> lock(documents_mutex_);
            open_documents_.erase(document.uri);
        }
        
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        doc.set_root(root);
        send_notification("textDocument/didClose", doc.to_string());
    }
    
    // Save a text document
    void did_save(const TextDocumentIdentifier& document, std::optional<std::string> text = std::nullopt) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        if (text) {
            root.add("text", doc.string(*text));
        }
        
        doc.set_root(root);
        send_notification("textDocument/didSave", doc.to_string());
    }
    
    // =====================================================================
    // Language Features
    // =====================================================================
    
    // Get completions at a position
    [[nodiscard]] LspResult<CompletionList> completion(const TextDocumentIdentifier& document, const Position& position) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto pos = doc.object();
        pos.add("line", doc.number(position.line));
        pos.add("character", doc.number(position.character));
        root.add("position", pos);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/completion", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_completion_list(*response);
    }
    
    // Get hover information at a position
    [[nodiscard]] LspResult<Hover> hover(const TextDocumentIdentifier& document, const Position& position) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto pos = doc.object();
        pos.add("line", doc.number(position.line));
        pos.add("character", doc.number(position.character));
        root.add("position", pos);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/hover", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_hover(*response);
    }
    
    // Go to definition
    [[nodiscard]] LspResult<std::vector<Location>> definition(const TextDocumentIdentifier& document, const Position& position) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto pos = doc.object();
        pos.add("line", doc.number(position.line));
        pos.add("character", doc.number(position.character));
        root.add("position", pos);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/definition", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_locations(*response);
    }
    
    // Go to references
    [[nodiscard]] LspResult<std::vector<Location>> references(const TextDocumentIdentifier& document, const Position& position, bool include_declaration = true) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto pos = doc.object();
        pos.add("line", doc.number(position.line));
        pos.add("character", doc.number(position.character));
        root.add("position", pos);
        
        auto context = doc.object();
        context.add("includeDeclaration", doc.boolean(include_declaration));
        root.add("context", context);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/references", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_locations(*response);
    }
    
    // Get document symbols
    [[nodiscard]] LspResult<std::vector<DocumentSymbol>> document_symbol(const TextDocumentIdentifier& document) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/documentSymbol", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_document_symbols(*response);
    }
    
    // Get code actions
    [[nodiscard]] LspResult<std::vector<CodeAction>> code_action(const TextDocumentIdentifier& document, const Range& range, const CodeActionContext& context) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto range_obj = doc.object();
        auto start = doc.object();
        start.add("line", doc.number(range.start.line));
        start.add("character", doc.number(range.start.character));
        auto end = doc.object();
        end.add("line", doc.number(range.end.line));
        end.add("character", doc.number(range.end.character));
        range_obj.add("start", start);
        range_obj.add("end", end);
        root.add("range", range_obj);
        
        auto ctx = doc.object();
        auto diagnostics_arr = doc.array();
        for (const auto& diag : context.diagnostics) {
            auto diag_obj = doc.object();
            auto diag_range = doc.object();
            auto diag_start = doc.object();
            diag_start.add("line", doc.number(diag.range.start.line));
            diag_start.add("character", doc.number(diag.range.start.character));
            auto diag_end = doc.object();
            diag_end.add("line", doc.number(diag.range.end.line));
            diag_end.add("character", doc.number(diag.range.end.character));
            diag_range.add("start", diag_start);
            diag_range.add("end", diag_end);
            diag_obj.add("range", diag_range);
            diag_obj.add("severity", doc.number(static_cast<int64_t>(diag.severity)));
            diag_obj.add("message", doc.string(diag.message));
            diagnostics_arr.append(diag_obj);
        }
        ctx.add("diagnostics", diagnostics_arr);
        root.add("context", ctx);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/codeAction", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_code_actions(*response);
    }
    
    // Format document
    [[nodiscard]] LspResult<std::vector<TextEdit>> formatting(const TextDocumentIdentifier& document, int64_t tab_size = 4, bool insert_spaces = true) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto options = doc.object();
        options.add("tabSize", doc.number(tab_size));
        options.add("insertSpaces", doc.boolean(insert_spaces));
        root.add("options", options);
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/formatting", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return parse_text_edits(*response);
    }
    
    // Rename symbol
    [[nodiscard]] LspResult<std::string> rename(const TextDocumentIdentifier& document, const Position& position, const std::string& new_name) {
        JsonMutDoc doc;
        auto root = doc.object();
        
        auto text_document = doc.object();
        text_document.add("uri", doc.string(document.uri));
        root.add("textDocument", text_document);
        
        auto pos = doc.object();
        pos.add("line", doc.number(position.line));
        pos.add("character", doc.number(position.character));
        root.add("position", pos);
        
        root.add("newName", doc.string(new_name));
        
        doc.set_root(root);
        
        auto response = send_request_sync("textDocument/rename", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        return *response;
    }
    
    // =====================================================================
    // State Accessors
    // =====================================================================
    
    [[nodiscard]] LspClientState state() const { return state_; }
    [[nodiscard]] bool is_ready() const { return state_ == LspClientState::Ready; }
    [[nodiscard]] const ServerCapabilities& server_capabilities() const { return server_capabilities_; }
    
    [[nodiscard]] std::optional<TextDocumentItem> get_open_document(const std::string& uri) const {
        std::lock_guard<std::mutex> lock(documents_mutex_);
        auto it = open_documents_.find(uri);
        if (it != open_documents_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    [[nodiscard]] std::vector<std::string> get_open_document_uris() const {
        std::lock_guard<std::mutex> lock(documents_mutex_);
        std::vector<std::string> uris;
        uris.reserve(open_documents_.size());
        for (const auto& [uri, _] : open_documents_) {
            uris.push_back(uri);
        }
        return uris;
    }
    
private:
    // =====================================================================
    // JSON-RPC Helpers
    // =====================================================================
    
    // Send a JSON-RPC request and wait for response (synchronous)
    [[nodiscard]] LspResult<std::string> send_request_sync(
        std::string_view method, std::string params_json) {
        
        std::promise<LspResult<std::string>> prom;
        auto future = prom.get_future();
        
        auto callback = [&prom](const std::string& response, std::optional<LspClientError> error) {
            if (error) {
                prom.set_value(std::unexpected(*error));
            } else {
                prom.set_value(response);
            }
        };
        
        send_request_async(method, std::move(params_json), std::move(callback));
        
        auto status = future.wait_for(config_.request_timeout);
        if (status == std::future_status::timeout) {
            return std::unexpected(LspClientError::Timeout);
        }
        
        return future.get();
    }
    
    // Send a JSON-RPC request asynchronously
    void send_request_async(
        std::string_view method,
        std::string params_json,
        RequestCallback callback) {
        
        int64_t id = next_request_id_++;
        
        // Build JSON-RPC message as string (params already serialized)
        std::string serialized = std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})",
            id, method, params_json.empty() ? "{}" : params_json);
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[id] = PendingRequest{
                .id = id,
                .sent_at = std::chrono::steady_clock::now(),
                .timeout = config_.request_timeout,
                .callback = std::move(callback)
            };
        }
        
        auto result = transport_send(serialized);
        if (!result) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                if (it->second.callback) {
                    it->second.callback("", result.error());
                }
                pending_requests_.erase(it);
            }
        }
    }
    
    // Send a JSON-RPC notification (no response expected)
    void send_notification(std::string_view method, std::string params_json) {
        // Build JSON-RPC notification as string (params already serialized)
        std::string serialized;
        if (params_json.empty()) {
            serialized = std::format(R"({{"jsonrpc":"2.0","method":"{}"}})", method);
        } else {
            serialized = std::format(
                R"({{"jsonrpc":"2.0","method":"{}","params":{}}})", method, params_json);
        }
        (void)transport_send(serialized); // Fire and forget
    }
    
    // Transport layer abstraction
    [[nodiscard]] LspResult<void> transport_send(std::string_view message) {
        if (!transport_ || !transport_->is_connected()) {
            return std::unexpected(LspClientError::NotConnected);
        }
        return transport_->send(message);
    }
    
    // Receive loop (runs in background thread)
    void receive_loop() {
        while (running_) {
            if (!transport_ || !transport_->is_connected()) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            
            auto result = transport_->receive();
            if (!result) {
                if (result.error() == LspClientError::ServerClosed) {
                    state_ = LspClientState::Stopped;
                    running_ = false;
                } else if (result.error() == LspClientError::Timeout) {
                    // Check for timed-out pending requests
                    check_pending_timeouts();
                }
                continue;
            }
            
            handle_incoming_message(*result);
        }
    }
    
    // Handle incoming message
    void handle_incoming_message(const std::string& message) {
        // Parse message
        auto doc = parse(message);
        if (!doc) {
            return;
        }
        
        auto root = doc->root();
        if (!root.is_obj()) {
            return;
        }
        
        // Check if it's a response (has id)
        auto id_node = root.get("id");
        if (id_node.valid() && !id_node.is_null()) {
            // Handle response
            int64_t id = 0;
            if (id_node.is_num()) {
                id = static_cast<int64_t>(id_node.as_int());
            } else if (id_node.is_str()) {
                try {
                    id = std::stoll(std::string(id_node.as_str()));
                } catch (...) {
                    return;
                }
            } else {
                return;
            }
            
            // Check for error
            auto error_node = root.get("error");
            std::optional<LspClientError> error = std::nullopt;
            if (error_node.valid() && !error_node.is_null()) {
                // Map the server's JSON-RPC error code to a meaningful
                // LspClientError instead of collapsing every failure to
                // RequestFailed.
                auto code_node = error_node.get("code");
                auto code = code_node.is_num()
                    ? static_cast<LspErrorCode>(code_node.as_int())
                    : LspErrorCode::UnknownErrorCode;
                switch (code) {
                    case LspErrorCode::ParseError:           error = LspClientError::ParseError; break;
                    case LspErrorCode::InvalidRequest:
                    case LspErrorCode::InvalidParams:        error = LspClientError::InvalidResponse; break;
                    case LspErrorCode::MethodNotFound:       error = LspClientError::RequestFailed; break;
                    case LspErrorCode::ServerNotInitialized: error = LspClientError::NotConnected; break;
                    case LspErrorCode::RequestCancelled:
                    case LspErrorCode::ServerCancelled:      error = LspClientError::Timeout; break;
                    case LspErrorCode::ContentModified:      error = LspClientError::RequestFailed; break;
                    default:                                 error = LspClientError::RequestFailed; break;
                }
            }
            
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end() && !it->second.completed) {
                it->second.completed = true;
                if (it->second.callback) {
                    if (error) {
                        it->second.callback("", *error);
                    } else {
                        auto result_node = root.get("result");
                        if (result_node.valid()) {
                            auto result_str = to_string(result_node);
                            it->second.callback(result_str.empty() ? "{}" : result_str, std::nullopt);
                        } else {
                            it->second.callback("", std::nullopt);
                        }
                    }
                }
                pending_requests_.erase(it);
            }
        } else {
            // Check if it's a notification (method without id)
            auto method_node = root.get("method");
            if (method_node.is_str()) {
                std::string method = std::string(method_node.as_str());
                
                // Handle specific notifications
                if (method == "textDocument/publishDiagnostics") {
                    handle_publish_diagnostics(root);
                }
                
                // Extract params
                std::string params_json = "{}";
                auto params_node = root.get("params");
                if (params_node.valid() && !params_node.is_null()) {
                    auto serialized = to_string(params_node);
                    if (!serialized.empty()) params_json = std::move(serialized);
                }
                
                if (notification_callback_) {
                    notification_callback_(method, params_json);
                }
            }
        }
    }
    
    // Handle publishDiagnostics notification
    void handle_publish_diagnostics(const JsonVal& root) {
        auto params = root.get("params");
        if (!params.valid()) return;
        
        auto uri_node = params.get("uri");
        auto diagnostics_node = params.get("diagnostics");
        
        if (!uri_node.is_str() || !diagnostics_node.is_arr()) return;
        
        std::string uri = std::string(uri_node.as_str());
        std::vector<Diagnostic> diagnostics;
        
        diagnostics_node.iter([&diagnostics](JsonVal diag_node) {
            Diagnostic diag;
            
            // Parse range
            auto range_node = diag_node.get("range");
            if (range_node.is_obj()) {
                auto start_node = range_node.get("start");
                auto end_node = range_node.get("end");
                if (start_node.is_obj() && end_node.is_obj()) {
                    diag.range.start.line = static_cast<int64_t>(start_node.get("line").as_int());
                    diag.range.start.character = static_cast<int64_t>(start_node.get("character").as_int());
                    diag.range.end.line = static_cast<int64_t>(end_node.get("line").as_int());
                    diag.range.end.character = static_cast<int64_t>(end_node.get("character").as_int());
                }
            }
            
            // Parse severity
            auto severity_node = diag_node.get("severity");
            if (severity_node.is_num()) {
                diag.severity = static_cast<int32_t>(severity_node.as_int());
            }
            
            // Parse message
            auto message_node = diag_node.get("message");
            if (message_node.is_str()) {
                diag.message = std::string(message_node.as_str());
            }
            
            diagnostics.push_back(std::move(diag));
        });
        
        // Update internal diagnostics store
        {
            std::lock_guard<std::mutex> lock(documents_mutex_);
            diagnostics_[uri] = diagnostics;
        }
        
        // Notify callback
        if (diagnostics_callback_) {
            diagnostics_callback_(uri, diagnostics);
        }
    }
    
    // Check for timed-out pending requests
    void check_pending_timeouts() {
        auto now = std::chrono::steady_clock::now();
        
        std::vector<int64_t> timed_out;
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (const auto& [id, req] : pending_requests_) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.sent_at);
                if (elapsed > req.timeout) {
                    timed_out.push_back(id);
                }
            }
            
            for (const auto& id : timed_out) {
                auto it = pending_requests_.find(id);
                if (it != pending_requests_.end() && !it->second.completed) {
                    it->second.completed = true;
                    if (it->second.callback) {
                        it->second.callback("", LspClientError::Timeout);
                    }
                    pending_requests_.erase(it);
                }
            }
        }
    }
    
public:
    // =====================================================================
    // Response Parsers (pure functions; public for unit testing with fixtures)
    // =====================================================================

    // --- Low-level JSON helpers (operate on a cc::utils::json::JsonVal) ---

    [[nodiscard]] static Position parse_position(JsonVal node) {
        Position p;
        if (!node.is_obj()) return p;
        if (auto v = node.get("line"); v && v.is_num()) p.line = v.as_int();
        if (auto v = node.get("character"); v && v.is_num()) p.character = v.as_int();
        return p;
    }

    [[nodiscard]] static Range parse_range(JsonVal node) {
        Range r;
        if (!node.is_obj()) return r;
        r.start = parse_position(node.get("start"));
        r.end = parse_position(node.get("end"));
        return r;
    }

    // Extract a human-readable string from a MarkupContent / string / array
    // "contents" value (per Hover).
    [[nodiscard]] static std::string extract_markup_string(JsonVal node) {
        if (node.is_str()) return std::string(node.as_str());
        if (node.is_obj()) {
            if (auto v = node.get("value"); v && v.is_str()) return std::string(v.as_str());
        }
        if (node.is_arr()) {
            std::string joined;
            node.iter([&](JsonVal part) {
                if (part.is_str()) {
                    if (!joined.empty()) joined.push_back('\n');
                    joined += std::string(part.as_str());
                } else if (part.is_obj()) {
                    if (auto v = part.get("value"); v && v.is_str()) {
                        if (!joined.empty()) joined.push_back('\n');
                        joined += std::string(v.as_str());
                    }
                }
            });
            return joined;
        }
        return {};
    }

    [[nodiscard]] LspResult<InitializeResult> parse_initialize_result(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        InitializeResult result;

        // Parse capabilities
        auto caps_node = doc->root().get("capabilities");
        if (caps_node.is_obj()) {
            auto& c = result.capabilities;
            // Each provider may be a bool or an options object; treat presence +
            // truthiness as "supported".
            auto flag = [](JsonVal v) -> std::optional<bool> {
                if (!v.valid()) return std::nullopt;
                if (v.is_bool()) return v.as_bool();
                if (v.is_obj()) return true; // options object => enabled
                return std::nullopt;
            };
            if (auto tds = caps_node.get("textDocumentSync"); tds.valid()) {
                if (tds.is_num()) c.text_document_sync = tds.as_int();
                else if (tds.is_obj()) {
                    if (auto ch = tds.get("change"); ch && ch.is_num()) c.text_document_sync = ch.as_int();
                }
            }
            if (flag(caps_node.get("completionProvider"))) c.completion_provider = "enabled";
            c.hover_provider = flag(caps_node.get("hoverProvider"));
            c.declaration_provider = flag(caps_node.get("declarationProvider"));
            c.definition_provider = flag(caps_node.get("definitionProvider"));
            c.type_definition_provider = flag(caps_node.get("typeDefinitionProvider"));
            c.implementation_provider = flag(caps_node.get("implementationProvider"));
            c.references_provider = flag(caps_node.get("referencesProvider"));
            c.document_highlight_provider = flag(caps_node.get("documentHighlightProvider"));
            c.document_symbol_provider = flag(caps_node.get("documentSymbolProvider"));
            if (flag(caps_node.get("codeActionProvider"))) c.code_action_provider = "enabled";
            if (flag(caps_node.get("codeLensProvider"))) c.code_lens_provider = "enabled";
            if (flag(caps_node.get("documentLinkProvider"))) c.document_link_provider = "enabled";
            c.color_provider = flag(caps_node.get("colorProvider"));
            c.document_formatting_provider = flag(caps_node.get("documentFormattingProvider"));
            c.document_range_formatting_provider = flag(caps_node.get("documentRangeFormattingProvider"));
            if (flag(caps_node.get("renameProvider"))) c.rename_provider = "enabled";
            if (flag(caps_node.get("foldingRangeProvider"))) c.folding_range_provider = "enabled";
            c.execute_command_provider = flag(caps_node.get("executeCommandProvider"));
            c.selection_range_provider = flag(caps_node.get("selectionRangeProvider"));
            if (flag(caps_node.get("semanticTokensProvider"))) c.semantic_tokens_provider = "enabled";
            if (flag(caps_node.get("signatureHelpProvider"))) c.signature_help_provider = "enabled";
            if (flag(caps_node.get("callHierarchyProvider"))) c.call_hierarchy_provider = true;
            if (flag(caps_node.get("typeHierarchyProvider"))) c.type_hierarchy_provider = true;
            if (flag(caps_node.get("inlayHintProvider"))) c.inlay_hint_provider = "enabled";
        }

        // Parse serverInfo (name/version)
        auto info = doc->root().get("serverInfo");
        if (info.is_obj()) {
            std::string name;
            if (auto n = info.get("name"); n && n.is_str()) name = std::string(n.as_str());
            if (auto v = info.get("version"); v && v.is_str()) name += " " + std::string(v.as_str());
            if (!name.empty()) result.server_info = name;
        }

        return result;
    }

    [[nodiscard]] LspResult<CompletionList> parse_completion_list(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        CompletionList result;

        // The response may be a CompletionList object or a bare array of items.
        auto root = doc->root();
        JsonVal items_node = root.get("items");
        if (!items_node.is_arr()) {
            // Bare array form.
            if (root.is_arr()) items_node = root;
        } else if (root.is_obj()) {
            if (auto v = root.get("isIncomplete"); v && v.is_bool()) result.is_incomplete = v.as_bool();
        }

        if (items_node.is_arr()) {
            items_node.iter([&result](JsonVal item_node) {
                CompletionItem item;
                if (auto v = item_node.get("label"); v && v.is_str()) item.label = std::string(v.as_str());
                if (auto v = item_node.get("kind"); v && v.is_num()) item.kind = static_cast<int32_t>(v.as_int());
                if (auto v = item_node.get("detail"); v && v.is_str()) item.detail = std::string(v.as_str());
                if (auto v = item_node.get("documentation"); v.valid()) item.documentation = extract_markup_string(v);
                if (auto v = item_node.get("sortText"); v && v.is_str()) item.sort_text = std::string(v.as_str());
                if (auto v = item_node.get("filterText"); v && v.is_str()) item.filter_text = std::string(v.as_str());
                if (auto v = item_node.get("insertText"); v && v.is_str()) item.insert_text = std::string(v.as_str());
                if (auto v = item_node.get("insertTextFormat"); v && v.is_num()) item.insert_text_format = static_cast<int32_t>(v.as_int());
                if (auto v = item_node.get("deprecated"); v && v.is_bool()) item.deprecated = v.as_bool();
                if (auto v = item_node.get("preselect"); v && v.is_bool()) item.preselect = v.as_bool();
                if (auto te = item_node.get("textEdit"); te.is_obj()) {
                    TextEdit edit;
                    edit.range = parse_range(te.get("range"));
                    if (auto nt = te.get("newText"); nt && nt.is_str()) edit.new_text = std::string(nt.as_str());
                    item.text_edit = std::move(edit);
                }
                result.items.push_back(std::move(item));
            });
        }

        return result;
    }

    [[nodiscard]] LspResult<Hover> parse_hover(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        Hover result;
        auto root = doc->root();
        if (root.is_obj()) {
            result.contents = extract_markup_string(root.get("contents"));
            if (auto r = root.get("range"); r.is_obj()) result.range = parse_range(r);
        } else {
            result.contents = std::string{};
        }
        return result;
    }

    [[nodiscard]] LspResult<std::vector<Location>> parse_locations(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        std::vector<Location> result;
        auto root = doc->root();
        // A single Location (object) or an array of Locations are both valid.
        if (root.is_obj()) {
            Location loc;
            if (auto v = root.get("uri"); v && v.is_str()) loc.uri = std::string(v.as_str());
            loc.range = parse_range(root.get("range"));
            result.push_back(std::move(loc));
        } else if (root.is_arr()) {
            root.iter([&result](JsonVal loc_node) {
                Location loc;
                if (auto v = loc_node.get("uri"); v && v.is_str()) loc.uri = std::string(v.as_str());
                loc.range = parse_range(loc_node.get("range"));
                result.push_back(std::move(loc));
            });
        }

        return result;
    }

    // Parse one DocumentSymbol (or SymbolInformation) and its children recursively.
    [[nodiscard]] static DocumentSymbol parse_one_symbol(JsonVal node) {
        DocumentSymbol sym;
        if (!node.is_obj()) return sym;
        if (auto v = node.get("name"); v && v.is_str()) sym.name = std::string(v.as_str());
        if (auto v = node.get("detail"); v && v.is_str()) sym.detail = std::string(v.as_str());
        if (auto v = node.get("kind"); v && v.is_num()) sym.kind = static_cast<int32_t>(v.as_int());
        sym.range = parse_range(node.get("range"));
        sym.selection_range = parse_range(node.get("selectionRange"));
        // SymbolInformation (flat form) has no selectionRange; fall back to range.
        if (auto sr = node.get("selectionRange"); !sr.is_obj()) sym.selection_range = sym.range;
        if (auto children = node.get("children"); children.is_arr()) {
            std::vector<DocumentSymbol> kids;
            children.iter([&kids](JsonVal c) { kids.push_back(parse_one_symbol(c)); });
            sym.children = std::move(kids);
        }
        return sym;
    }

    [[nodiscard]] LspResult<std::vector<DocumentSymbol>> parse_document_symbols(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }
        std::vector<DocumentSymbol> result;
        auto root = doc->root();
        if (root.is_arr()) {
            root.iter([&result](JsonVal n) { result.push_back(parse_one_symbol(n)); });
        }
        return result;
    }

    [[nodiscard]] LspResult<std::vector<CodeAction>> parse_code_actions(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }
        std::vector<CodeAction> result;
        auto root = doc->root();
        if (root.is_arr()) {
            root.iter([&result](JsonVal n) {
                if (!n.is_obj()) return;
                CodeAction a;
                if (auto v = n.get("title"); v && v.is_str()) a.title = std::string(v.as_str());
                if (auto v = n.get("kind"); v && v.is_str()) a.kind = std::string(v.as_str());
                if (auto v = n.get("isPreferred"); v && v.is_bool()) a.is_preferred = v.as_bool();
                if (auto v = n.get("disabled"); v.is_obj()) {
                    if (auto r = v.get("reason"); r && r.is_str()) a.disabled = true;
                }
                result.push_back(std::move(a));
            });
        }
        return result;
    }

    [[nodiscard]] LspResult<std::vector<TextEdit>> parse_text_edits(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }
        std::vector<TextEdit> result;
        auto root = doc->root();
        // May be a bare array, or a WorkspaceEdit with "changes"/"documentChanges".
        JsonVal edits_node = root;
        if (root.is_obj()) {
            JsonVal changes = root.get("changes");
            if (changes.is_obj()) {
                // { uri: [TextEdit, ...], ... } — flatten across URIs.
                changes.iter_obj([&result](auto /*uri*/, JsonVal arr) {
                    if (arr.is_arr()) {
                        arr.iter([&result](JsonVal e) {
                            TextEdit te;
                            te.range = parse_range(e.get("range"));
                            if (auto nt = e.get("newText"); nt && nt.is_str()) te.new_text = std::string(nt.as_str());
                            result.push_back(std::move(te));
                        });
                    }
                });
                return result;
            }
        }
        if (edits_node.is_arr()) {
            edits_node.iter([&result](JsonVal e) {
                if (!e.is_obj()) return;
                TextEdit te;
                te.range = parse_range(e.get("range"));
                if (auto nt = e.get("newText"); nt && nt.is_str()) te.new_text = std::string(nt.as_str());
                result.push_back(std::move(te));
            });
        }
        return result;
    }

private:
    // =====================================================================
    // Member Variables
    // =====================================================================
    
    Config config_;
    LspClientState state_;
    int64_t next_request_id_;
    ServerCapabilities server_capabilities_;
    
    std::unique_ptr<ILspTransport> transport_;
    
    std::mutex pending_mutex_;
    std::map<int64_t, PendingRequest> pending_requests_;
    
    mutable std::mutex documents_mutex_;
    std::unordered_map<std::string, TextDocumentItem> open_documents_;
    std::unordered_map<std::string, std::vector<Diagnostic>> diagnostics_;
    
    std::atomic<bool> running_;
    std::thread receive_thread_;
    
    NotificationCallback notification_callback_;
    std::function<void(const std::string& uri, const std::vector<Diagnostic>& diagnostics)> diagnostics_callback_;
};

} // namespace cc::services::lsp
