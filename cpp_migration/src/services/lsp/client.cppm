/// @file lsp/client.cppm
/// @brief LSP Client - Language Server Protocol client with JSON-RPC 2.0 transport
/// Implements LSP client capabilities including document sync, completion,
/// go-to-definition, hover, diagnostics, and more.
module;

#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.services.lsp.client;

import cc.utils.json;
import cc.types.types;

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

/// StdioTransport uses pipe()+fork()+execvp() for true bidirectional
/// communication required by LSP's Content-Length framing protocol.
/// popen() is NOT suitable because it does not guarantee bidirectional I/O.
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

        int stdin_pipe[2]{};
        int stdout_pipe[2]{};
        if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
            return std::unexpected(LspClientError::ConnectionFailed);
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
            return std::unexpected(LspClientError::ConnectionFailed);
        }

        if (pid_ == 0) {
            // Child process
            ::close(stdin_pipe[1]);   // Close write end of stdin pipe
            ::close(stdout_pipe[0]);  // Close read end of stdout pipe
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            ::close(stdin_pipe[0]);
            ::close(stdout_pipe[1]);

            for (const auto& [key, value] : env_) {
                ::setenv(key.c_str(), value.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command_.c_str()));
            for (auto& arg : args_) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            ::execvp(command_.c_str(), argv.data());
            ::_exit(127);  // exec failed — must _exit, not exit
        }

        // Parent process
        ::close(stdin_pipe[0]);   // Close read end of stdin pipe
        ::close(stdout_pipe[1]);  // Close write end of stdout pipe
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        ::fcntl(stdout_fd_, F_SETFL, O_NONBLOCK);

        connected_ = true;
        return {};
    }

    [[nodiscard]] LspResult<void> send(std::string_view message) override {
        if (!is_connected()) {
            return std::unexpected(LspClientError::NotConnected);
        }

        // LSP uses Content-Length header followed by JSON
        std::string frame = std::format("Content-Length: {}\r\n\r\n", message.size());
        frame.append(message);

        std::string_view remaining(frame);
        while (!remaining.empty()) {
            auto written = ::write(stdin_fd_, remaining.data(), remaining.size());
            if (written <= 0) {
                if (errno == EINTR) continue;
                return std::unexpected(LspClientError::TransportError);
            }
            remaining.remove_prefix(static_cast<size_t>(written));
        }
        return {};
    }

    [[nodiscard]] LspResult<std::string> receive() override {
        if (!is_connected()) {
            return std::unexpected(LspClientError::NotConnected);
        }

        // Try to extract a complete message from the buffer first
        if (auto msg = try_extract_message()) {
            return *msg;
        }

        // Poll for incoming data with a timeout
        constexpr auto poll_timeout = std::chrono::milliseconds{5000};
        auto deadline = std::chrono::steady_clock::now() + poll_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            if (auto msg = try_extract_message()) {
                return *msg;
            }

            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            struct ::pollfd pfd{.fd = stdout_fd_, .events = POLLIN, .revents = 0};
            auto ready = ::poll(&pfd, 1, static_cast<int>(std::max<int64_t>(1, remaining_ms.count())));
            if (ready < 0) {
                if (errno == EINTR) continue;
                return std::unexpected(LspClientError::TransportError);
            }
            if (ready == 0) {
                return std::unexpected(LspClientError::Timeout);
            }

            // Drain available data into buffer
            std::array<char, 4096> chunk{};
            while (true) {
                auto n = ::read(stdout_fd_, chunk.data(), chunk.size());
                if (n > 0) {
                    inbound_buffer_.append(chunk.data(), static_cast<size_t>(n));
                    continue;
                }
                if (n == 0) {
                    connected_ = false;
                    return std::unexpected(LspClientError::ServerClosed);
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return std::unexpected(LspClientError::TransportError);
            }
        }

        return std::unexpected(LspClientError::Timeout);
    }

    [[nodiscard]] bool is_connected() const override {
        return connected_ && stdin_fd_ >= 0;
    }

    void close() override {
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if (stdout_fd_ >= 0) {
            ::close(stdout_fd_);
            stdout_fd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == 0) {
                ::kill(pid_, SIGTERM);
                // Give it a moment, then escalate to SIGKILL
                if (::waitpid(pid_, &status, WNOHANG) == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    ::kill(pid_, SIGKILL);
                }
                ::waitpid(pid_, &status, 0);
            }
            pid_ = -1;
        }
        connected_ = false;
        inbound_buffer_.clear();
    }

private:
    /// Attempt to parse a complete LSP message from the inbound buffer.
    /// Returns the message body if a complete frame is available.
    [[nodiscard]] std::optional<std::string> try_extract_message() {
        auto header_end = inbound_buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) return std::nullopt;

        auto header = std::string_view(inbound_buffer_).substr(0, header_end);
        int64_t content_length = 0;

        // Parse Content-Length from header
        auto cl_pos = header.find("Content-Length:");
        if (cl_pos != std::string_view::npos) {
            cl_pos += std::string_view("Content-Length:").size();
            while (cl_pos < header.size() && header[cl_pos] == ' ') ++cl_pos;
            auto cl_end = cl_pos;
            while (cl_end < header.size() && std::isdigit(static_cast<unsigned char>(header[cl_end]))) {
                ++cl_end;
            }
            if (cl_end > cl_pos) {
                content_length = std::stoll(std::string(header.substr(cl_pos, cl_end - cl_pos)));
            }
        }

        if (content_length <= 0) return std::nullopt;

        auto content_start = header_end + 4;
        if (static_cast<int64_t>(inbound_buffer_.size()) < static_cast<int64_t>(content_start) + content_length) {
            return std::nullopt;  // Not enough data yet
        }

        auto message = inbound_buffer_.substr(content_start, static_cast<size_t>(content_length));
        inbound_buffer_.erase(0, content_start + static_cast<size_t>(content_length));
        return message;
    }

    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    bool connected_ = false;
    std::string inbound_buffer_;
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
        shutdown();
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
        
        auto response = send_request_sync("initialize", doc.to_string());
        if (!response) {
            state_ = LspClientState::Error;
            return std::unexpected(response.error());
        }
        
        // Parse initialize result
        auto init_result = parse_initialize_result(*response);
        if (!init_result) {
            state_ = LspClientState::Error;
            return std::unexpected(LspClientError::InvalidResponse);
        }
        
        server_capabilities_ = init_result->capabilities;
        
        // Send initialized notification
        send_notification("initialized", "{}");
        
        // Start receive thread
        running_ = true;
        receive_thread_ = std::thread(&LspClient::receive_loop, this);
        
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
        transport_send(serialized); // Fire and forget
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
                error = LspClientError::RequestFailed;
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
    
    // =====================================================================
    // Response Parsers
    // =====================================================================
    
    // --- Helper: parse a Range from a JSON object node ---
    [[nodiscard]] static std::optional<Range> parse_range(JsonVal node) {
        if (!node.is_obj()) return std::nullopt;
        auto start_node = node.get("start");
        auto end_node = node.get("end");
        if (!start_node.is_obj() || !end_node.is_obj()) return std::nullopt;
        Range r;
        r.start.line = start_node.get("line").is_num() ? start_node.get("line").as_int() : 0;
        r.start.character = start_node.get("character").is_num() ? start_node.get("character").as_int() : 0;
        r.end.line = end_node.get("line").is_num() ? end_node.get("line").as_int() : 0;
        r.end.character = end_node.get("character").is_num() ? end_node.get("character").as_int() : 0;
        return r;
    }

    [[nodiscard]] LspResult<InitializeResult> parse_initialize_result(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        InitializeResult result;
        auto root = doc->root();

        // Parse server info
        auto info_node = root.get("serverInfo");
        if (info_node.is_obj()) {
            auto name_node = info_node.get("name");
            if (name_node.is_str()) {
                result.server_info = std::string(name_node.as_str());
            }
        }

        // Parse capabilities
        auto caps_node = root.get("capabilities");
        if (caps_node.is_obj()) {
            auto& caps = result.capabilities;

            auto tds = caps_node.get("textDocumentSync");
            if (tds.is_num()) caps.text_document_sync = tds.as_int();
            else if (tds.is_obj()) caps.text_document_sync = std::string(to_string(tds));

            auto cp = caps_node.get("completionProvider");
            if (cp.is_obj()) caps.completion_provider = std::string(to_string(cp));

            auto hp = caps_node.get("hoverProvider");
            if (hp.is_bool()) caps.hover_provider = hp.as_bool();

            auto shp = caps_node.get("signatureHelpProvider");
            if (shp.is_obj()) caps.signature_help_provider = std::string(to_string(shp));

            auto dp = caps_node.get("definitionProvider");
            if (dp.is_bool()) caps.definition_provider = dp.as_bool();

            auto rp = caps_node.get("referencesProvider");
            if (rp.is_bool()) caps.references_provider = rp.as_bool();

            auto dsp = caps_node.get("documentSymbolProvider");
            if (dsp.is_bool()) caps.document_symbol_provider = dsp.as_bool();

            auto dfp = caps_node.get("documentFormattingProvider");
            if (dfp.is_bool()) caps.document_formatting_provider = dfp.as_bool();

            auto cap_node = caps_node.get("codeActionProvider");
            if (cap_node.is_bool()) caps.code_action_provider = cap_node.as_bool() ? "true" : "false";
            else if (cap_node.is_obj()) caps.code_action_provider = std::string(to_string(cap_node));

            auto rnp = caps_node.get("renameProvider");
            if (rnp.is_bool()) caps.rename_provider = rnp.as_bool() ? "true" : "false";
            else if (rnp.is_obj()) caps.rename_provider = std::string(to_string(rnp));
        }

        return result;
    }

    [[nodiscard]] LspResult<CompletionList> parse_completion_list(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        CompletionList result;

        auto root = doc->root();
        if (root.is_obj()) {
            auto is_incomplete_node = root.get("isIncomplete");
            if (is_incomplete_node.is_bool()) {
                result.is_incomplete = is_incomplete_node.as_bool();
            }

            auto items_node = root.get("items");
            if (items_node.is_arr()) {
                items_node.iter([&result](JsonVal item_node) {
                    CompletionItem item;
                    auto label_node = item_node.get("label");
                    if (label_node.is_str()) {
                        item.label = std::string(label_node.as_str());
                    }
                    auto kind_node = item_node.get("kind");
                    if (kind_node.is_num()) {
                        item.kind = static_cast<int32_t>(kind_node.as_int());
                    }
                    auto detail_node = item_node.get("detail");
                    if (detail_node.is_str()) {
                        item.detail = std::string(detail_node.as_str());
                    }
                    auto doc_node = item_node.get("documentation");
                    if (doc_node.is_str()) {
                        item.documentation = std::string(doc_node.as_str());
                    } else if (doc_node.is_obj()) {
                        // MarkupContent: {kind, value}
                        auto val = doc_node.get("value");
                        if (val.is_str()) item.documentation = std::string(val.as_str());
                    }
                    auto sort_node = item_node.get("sortText");
                    if (sort_node.is_str()) item.sort_text = std::string(sort_node.as_str());
                    auto filter_node = item_node.get("filterText");
                    if (filter_node.is_str()) item.filter_text = std::string(filter_node.as_str());
                    auto insert_node = item_node.get("insertText");
                    if (insert_node.is_str()) item.insert_text = std::string(insert_node.as_str());
                    result.items.push_back(std::move(item));
                });
            }
        }

        return result;
    }

    [[nodiscard]] LspResult<Hover> parse_hover(const std::string& json_str) {
        auto doc = parse(json_str);
        if (!doc) {
            return std::unexpected(LspClientError::ParseError);
        }

        Hover result;
        result.contents = std::string{""};

        auto root = doc->root();
        if (!root.is_obj()) return result;

        // Hover.contents can be:
        //   string | MarkedString | MarkedString[] | MarkupContent
        auto contents_node = root.get("contents");
        if (contents_node.is_str()) {
            result.contents = std::string(contents_node.as_str());
        } else if (contents_node.is_obj()) {
            // MarkupContent: { kind: "markdown"|"plaintext", value: "..." }
            auto val = contents_node.get("value");
            if (val.is_str()) {
                result.contents = std::string(val.as_str());
            }
        } else if (contents_node.is_arr()) {
            // MarkedString[]
            std::vector<std::string> parts;
            contents_node.iter([&parts](JsonVal el) {
                if (el.is_str()) {
                    parts.push_back(std::string(el.as_str()));
                } else if (el.is_obj()) {
                    auto v = el.get("value");
                    if (v.is_str()) parts.push_back(std::string(v.as_str()));
                }
            });
            result.contents = std::move(parts);
        }

        // Parse optional range
        auto range_node = root.get("range");
        if (auto r = parse_range(range_node)) {
            result.range = *r;
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

        // Response can be Location[], LocationLink[], or a single Location
        if (root.is_arr()) {
            root.iter([&result](JsonVal loc_node) {
                Location loc;
                auto uri_node = loc_node.get("uri");
                if (uri_node.is_str()) {
                    loc.uri = std::string(uri_node.as_str());
                }
                if (auto r = parse_range(loc_node.get("range"))) {
                    loc.range = *r;
                }
                result.push_back(std::move(loc));
            });
        } else if (root.is_obj()) {
            // Single Location or LocationLink
            auto uri_node = root.get("uri");
            if (uri_node.is_str()) {
                Location loc;
                loc.uri = std::string(uri_node.as_str());
                if (auto r = parse_range(root.get("range"))) {
                    loc.range = *r;
                }
                result.push_back(std::move(loc));
            } else {
                // LocationLink: { targetUri, targetRange, targetSelectionRange }
                auto target_uri = root.get("targetUri");
                if (target_uri.is_str()) {
                    Location loc;
                    loc.uri = std::string(target_uri.as_str());
                    if (auto r = parse_range(root.get("targetRange"))) {
                        loc.range = *r;
                    }
                    result.push_back(std::move(loc));
                }
            }
        }

        return result;
    }

    // Helper: recursively parse DocumentSymbol hierarchy
    [[nodiscard]] static DocumentSymbol parse_one_document_symbol(JsonVal node) {
        DocumentSymbol sym;
        auto name_node = node.get("name");
        if (name_node.is_str()) sym.name = std::string(name_node.as_str());
        auto detail_node = node.get("detail");
        if (detail_node.is_str()) sym.detail = std::string(detail_node.as_str());
        auto kind_node = node.get("kind");
        if (kind_node.is_num()) sym.kind = static_cast<int32_t>(kind_node.as_int());
        auto dep_node = node.get("deprecated");
        if (dep_node.is_bool()) sym.deprecated = dep_node.as_bool();
        if (auto r = parse_range(node.get("range"))) sym.range = *r;
        if (auto r = parse_range(node.get("selectionRange"))) sym.selection_range = *r;
        auto children_node = node.get("children");
        if (children_node.is_arr()) {
            std::vector<DocumentSymbol> children;
            children_node.iter([&children](JsonVal child) {
                children.push_back(parse_one_document_symbol(child));
            });
            sym.children = std::move(children);
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
            // Could be DocumentSymbol[] or SymbolInformation[]
            root.iter([&result](JsonVal sym_node) {
                result.push_back(parse_one_document_symbol(sym_node));
            });
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
            root.iter([&result](JsonVal action_node) {
                CodeAction action;
                auto title_node = action_node.get("title");
                if (title_node.is_str()) action.title = std::string(title_node.as_str());
                auto kind_node = action_node.get("kind");
                if (kind_node.is_str()) action.kind = std::string(kind_node.as_str());
                auto preferred_node = action_node.get("isPreferred");
                if (preferred_node.is_bool()) action.is_preferred = preferred_node.as_bool();
                auto disabled_node = action_node.get("disabled");
                if (disabled_node.is_obj()) action.disabled = true;
                auto edit_node = action_node.get("edit");
                if (edit_node.is_obj() || edit_node.is_str()) action.edit = std::string(to_string(edit_node));
                auto cmd_node = action_node.get("command");
                if (cmd_node.is_obj()) {
                    auto cmd_name = cmd_node.get("command");
                    if (cmd_name.is_str()) action.command = std::string(cmd_name.as_str());
                }
                auto data_node = action_node.get("data");
                if (data_node.valid() && !data_node.is_null()) action.data = std::string(to_string(data_node));

                // Parse diagnostics
                auto diag_node = action_node.get("diagnostics");
                if (diag_node.is_arr()) {
                    std::vector<Diagnostic> diags;
                    diag_node.iter([&diags](JsonVal d) {
                        Diagnostic diag;
                        if (auto r = parse_range(d.get("range"))) diag.range = *r;
                        auto sev = d.get("severity");
                        if (sev.is_num()) diag.severity = static_cast<int32_t>(sev.as_int());
                        auto msg = d.get("message");
                        if (msg.is_str()) diag.message = std::string(msg.as_str());
                        diags.push_back(std::move(diag));
                    });
                    action.diagnostics = std::move(diags);
                }

                result.push_back(std::move(action));
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

        if (root.is_arr()) {
            root.iter([&result](JsonVal edit_node) {
                TextEdit edit;
                if (auto r = parse_range(edit_node.get("range"))) {
                    edit.range = *r;
                }
                auto new_text_node = edit_node.get("newText");
                if (new_text_node.is_str()) {
                    edit.new_text = std::string(new_text_node.as_str());
                }
                result.push_back(std::move(edit));
            });
        }

        return result;
    }
    
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
