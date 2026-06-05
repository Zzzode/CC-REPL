// LSP Server Instance Module
module;
#include <any>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.services.lsp.LSPServerInstance;

import cc.utils.error;
import cc.utils.json;
import cc.services.lsp.types;

export namespace cc::services::lsp {

using cc::utils::Result;
using cc::services::lsp::ScopedLspServerConfig;

// Forward declarations
struct LSPServerInstance;

// LSP Server Instance
struct LSPServerInstance {
    std::string name;
    ScopedLspServerConfig config;
    bool is_running = false;
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int64_t next_request_id = 1;
    std::vector<std::string> outbound_messages;
    std::string inbound_buffer;
    std::unordered_map<std::string, std::string> diagnostics_by_uri;
    
    // Start the server
    Result<void> start();
    
    // Stop the server
    Result<void> stop();
    
    // Send a request
    template<typename T>
    Result<T> send_request(const std::string& method, const std::any& params);
    
    // Send a notification
    Result<void> send_notification(const std::string& method, const std::any& params);

    // Drain notifications that arrive outside request/response windows.
    Result<void> poll(std::chrono::milliseconds timeout);

    [[nodiscard]] std::string diagnostics_json_for_uri(std::string_view uri) const;

private:
    Result<void> send_message(std::string_view json);
    Result<std::string> read_message(std::chrono::milliseconds timeout);
    std::string build_initialize_params() const;
    static std::string uri_encode(std::string_view value);
    static std::string path_to_file_uri(const std::string& file_path);
    static std::string params_to_json(const std::any& params);
    static std::optional<int64_t> message_id(const std::string& json);
    void handle_notification(const std::string& json);
};

// Create an LSP server instance
Result<std::unique_ptr<LSPServerInstance>> create_lsp_server_instance(
    const std::string& name,
    const ScopedLspServerConfig& config) {
    
    auto instance = std::make_unique<LSPServerInstance>();
    instance->name = name;
    instance->config = config;
    return instance;
}

// Start the server
Result<void> LSPServerInstance::start() {
    if (is_running) {
        return {};
    }
    if (config.command.empty()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::invalid_argument,
            "LSP server command is empty"));
    }
    
    int stdin_pipe[2]{};
    int stdout_pipe[2]{};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            "Failed to create LSP process pipes"));
    }

    pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            std::string("Failed to fork LSP server: ") + std::strerror(errno)));
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        for (const auto& [key, value] : config.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(config.command.c_str()));
        for (auto& arg : config.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(config.command.c_str(), argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    stdin_fd = stdin_pipe[1];
    stdout_fd = stdout_pipe[0];
    fcntl(stdout_fd, F_SETFL, O_NONBLOCK);
    is_running = true;
    auto initialized = send_request<std::string>("initialize", build_initialize_params());
    if (!initialized) {
        auto error = initialized.error();
        (void)stop();
        return std::unexpected(error);
    }
    auto notified = send_notification("initialized", "{}");
    if (!notified) {
        auto error = notified.error();
        (void)stop();
        return std::unexpected(error);
    }
    return {};
}

// Stop the server
Result<void> LSPServerInstance::stop() {
    if (!is_running) {
        return {};
    }
    
    (void)send_notification("exit", std::string{"null"});
    if (stdin_fd >= 0) {
        close(stdin_fd);
        stdin_fd = -1;
    }
    if (stdout_fd >= 0) {
        close(stdout_fd);
        stdout_fd = -1;
    }
    if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        pid = -1;
    }
    is_running = false;
    return {};
}

// Send a request
template<typename T>
Result<T> LSPServerInstance::send_request(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    const auto id = next_request_id++;
    auto params_json = params_to_json(params);
    auto message = std::format(
        R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})",
        id,
        method,
        params_json.empty() ? "{}" : params_json);
    auto sent = send_message(message);
    if (!sent) return std::unexpected(sent.error());

    if constexpr (std::is_same_v<T, std::string>) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{30000};
        while (std::chrono::steady_clock::now() < deadline) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto response = read_message(std::max(std::chrono::milliseconds{1}, remaining));
            if (!response) return std::unexpected(response.error());

            auto parsed = cc::utils::json::parse(*response);
            if (!parsed) return std::unexpected(parsed.error());
            auto root = parsed->root();
            auto method_node = root.get("method");
            if (method_node.is_str()) {
                handle_notification(*response);
                continue;
            }

            auto response_id = message_id(*response);
            if (!response_id || *response_id != id) continue;

            auto error_node = root.get("error");
            if (error_node.valid() && !error_node.is_null()) {
                std::string message = "LSP request failed";
                auto error_message = error_node.get("message");
                if (error_message.is_str()) message = std::string(error_message.as_str());
                return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::network_error, std::move(message)));
            }

            auto result_node = root.get("result");
            return result_node.valid() ? result_node.to_string() : std::string{"null"};
        }
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::timeout,
            "Timed out waiting for matching LSP response"));
    }
    return T{};
}

// Send a notification
Result<void> LSPServerInstance::send_notification(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    auto params_json = params_to_json(params);
    auto message = params_json.empty()
        ? std::format(R"({{"jsonrpc":"2.0","method":"{}"}})", method)
        : std::format(R"({{"jsonrpc":"2.0","method":"{}","params":{}}})", method, params_json);
    return send_message(message);
}

Result<void> LSPServerInstance::send_message(std::string_view json) {
    if (stdin_fd < 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::unavailable,
            "LSP server stdin is closed"));
    }
    std::string frame = std::format("Content-Length: {}\r\n\r\n", json.size());
    frame.append(json);
    outbound_messages.push_back(std::string(json));
    std::string_view remaining(frame);
    while (!remaining.empty()) {
        auto written = write(stdin_fd, remaining.data(), remaining.size());
        if (written <= 0) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to write LSP message: ") + std::strerror(errno)));
        }
        remaining.remove_prefix(static_cast<size_t>(written));
    }
    return {};
}

Result<std::string> LSPServerInstance::read_message(std::chrono::milliseconds timeout) {
    if (stdout_fd < 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::unavailable,
            "LSP server stdout is closed"));
    }

    auto read_available = [&](std::string& buffer) -> Result<void> {
        std::array<char, 4096> chunk{};
        while (true) {
            auto n = read(stdout_fd, chunk.data(), chunk.size());
            if (n > 0) {
                buffer.append(chunk.data(), static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                return std::unexpected(cc::utils::Error(
                    cc::utils::ErrorCode::unavailable,
                    "LSP server closed stdout"));
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to read LSP response: ") + std::strerror(errno)));
        }
    };

    auto try_extract_message = [&]() -> std::optional<std::string> {
        auto header_end = inbound_buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) return std::nullopt;
        auto header = inbound_buffer.substr(0, header_end);
        auto length_pos = header.find("Content-Length:");
        if (length_pos == std::string::npos) return std::nullopt;
        length_pos += std::string_view("Content-Length:").size();
        while (length_pos < header.size() && header[length_pos] == ' ') ++length_pos;
        auto length_end = length_pos;
        while (length_end < header.size() && std::isdigit(static_cast<unsigned char>(header[length_end]))) {
            ++length_end;
        }
        if (length_end == length_pos) return std::nullopt;
        auto length = static_cast<size_t>(std::stoull(header.substr(length_pos, length_end - length_pos)));
        auto content_start = header_end + 4;
        if (inbound_buffer.size() < content_start + length) return std::nullopt;
        auto message = inbound_buffer.substr(content_start, length);
        inbound_buffer.erase(0, content_start + length);
        return message;
    };

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto message = try_extract_message()) return *message;

        pollfd fd{.fd = stdout_fd, .events = POLLIN, .revents = 0};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto poll_timeout = static_cast<int>(std::max<int64_t>(1, remaining.count()));
        auto ready = ::poll(&fd, 1, poll_timeout);
        if (ready < 0) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to poll LSP response: ") + std::strerror(errno)));
        }
        if (ready == 0) break;
        auto read_result = read_available(inbound_buffer);
        if (!read_result) return std::unexpected(read_result.error());
    }

    return std::unexpected(cc::utils::Error(
        cc::utils::ErrorCode::timeout,
        "Timed out waiting for LSP response"));
}

Result<void> LSPServerInstance::poll(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto message = read_message(std::min(std::chrono::milliseconds{50}, std::max(std::chrono::milliseconds{1}, remaining)));
        if (!message) {
            if (message.error().code() == cc::utils::ErrorCode::timeout) return {};
            return std::unexpected(message.error());
        }
        handle_notification(*message);
    }
    return {};
}

std::string LSPServerInstance::diagnostics_json_for_uri(std::string_view uri) const {
    auto it = diagnostics_by_uri.find(std::string(uri));
    return it == diagnostics_by_uri.end() ? "[]" : it->second;
}

std::string LSPServerInstance::uri_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        bool safe =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string LSPServerInstance::path_to_file_uri(const std::string& file_path) {
    auto absolute = std::filesystem::absolute(std::filesystem::path(file_path)).string();
    return "file://" + uri_encode(absolute);
}

std::string LSPServerInstance::build_initialize_params() const {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();

    auto workspace_path = config.workspace_folder.value_or(std::filesystem::current_path().string());
    auto workspace_uri = path_to_file_uri(workspace_path);
    auto workspace_name = std::filesystem::path(workspace_path).filename().string();
    if (workspace_name.empty()) {
        workspace_name = workspace_path;
    }

    root.add("processId", doc.number(static_cast<int64_t>(getpid())));
    root.add("rootPath", doc.string(workspace_path));
    root.add("rootUri", doc.string(workspace_uri));

    auto initialization_options = doc.raw_json(
        config.initialization_options_json.empty() ? "{}" : config.initialization_options_json);
    root.add(
        "initializationOptions",
        initialization_options.valid() ? initialization_options : doc.object());

    auto workspace_folders = doc.array();
    auto workspace_folder = doc.object();
    workspace_folder.add("uri", doc.string(workspace_uri));
    workspace_folder.add("name", doc.string(workspace_name));
    workspace_folders.append(workspace_folder);
    root.add("workspaceFolders", workspace_folders);

    auto capabilities = doc.object();

    auto workspace = doc.object();
    workspace.add("configuration", doc.boolean(false));
    workspace.add("workspaceFolders", doc.boolean(false));
    capabilities.add("workspace", workspace);

    auto text_document = doc.object();
    auto synchronization = doc.object();
    synchronization.add("dynamicRegistration", doc.boolean(false));
    synchronization.add("willSave", doc.boolean(false));
    synchronization.add("willSaveWaitUntil", doc.boolean(false));
    synchronization.add("didSave", doc.boolean(true));
    text_document.add("synchronization", synchronization);

    auto publish_diagnostics = doc.object();
    publish_diagnostics.add("relatedInformation", doc.boolean(true));
    auto tag_support = doc.object();
    auto tag_value_set = doc.array();
    tag_value_set.append(doc.number(static_cast<int64_t>(1)));
    tag_value_set.append(doc.number(static_cast<int64_t>(2)));
    tag_support.add("valueSet", tag_value_set);
    publish_diagnostics.add("tagSupport", tag_support);
    publish_diagnostics.add("versionSupport", doc.boolean(false));
    publish_diagnostics.add("codeDescriptionSupport", doc.boolean(true));
    publish_diagnostics.add("dataSupport", doc.boolean(false));
    text_document.add("publishDiagnostics", publish_diagnostics);

    auto hover = doc.object();
    hover.add("dynamicRegistration", doc.boolean(false));
    auto content_format = doc.array();
    content_format.append(doc.string("markdown"));
    content_format.append(doc.string("plaintext"));
    hover.add("contentFormat", content_format);
    text_document.add("hover", hover);

    auto definition = doc.object();
    definition.add("dynamicRegistration", doc.boolean(false));
    definition.add("linkSupport", doc.boolean(true));
    text_document.add("definition", definition);

    auto references = doc.object();
    references.add("dynamicRegistration", doc.boolean(false));
    text_document.add("references", references);

    auto document_symbol = doc.object();
    document_symbol.add("dynamicRegistration", doc.boolean(false));
    document_symbol.add("hierarchicalDocumentSymbolSupport", doc.boolean(true));
    text_document.add("documentSymbol", document_symbol);

    auto call_hierarchy = doc.object();
    call_hierarchy.add("dynamicRegistration", doc.boolean(false));
    text_document.add("callHierarchy", call_hierarchy);
    capabilities.add("textDocument", text_document);

    auto general = doc.object();
    auto position_encodings = doc.array();
    position_encodings.append(doc.string("utf-16"));
    general.add("positionEncodings", position_encodings);
    capabilities.add("general", general);

    root.add("capabilities", capabilities);
    doc.set_root(root);
    return doc.to_string();
}

std::string LSPServerInstance::params_to_json(const std::any& params) {
    if (!params.has_value()) return "{}";
    if (params.type() == typeid(std::string)) {
        return std::any_cast<std::string>(params);
    }
    if (params.type() == typeid(const char*)) {
        return std::string(std::any_cast<const char*>(params));
    }
    if (params.type() == typeid(std::string_view)) {
        return std::string(std::any_cast<std::string_view>(params));
    }
    return "{}";
}

std::optional<int64_t> LSPServerInstance::message_id(const std::string& json) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::nullopt;
    auto id_node = parsed->root().get("id");
    if (id_node.is_num()) return id_node.as_int();
    if (id_node.is_str()) {
        try {
            return std::stoll(std::string(id_node.as_str()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void LSPServerInstance::handle_notification(const std::string& json) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return;
    auto root = parsed->root();
    auto method_node = root.get("method");
    if (!method_node.is_str()) return;
    if (method_node.as_str() != "textDocument/publishDiagnostics") return;

    auto params = root.get("params");
    auto uri_node = params.get("uri");
    auto diagnostics_node = params.get("diagnostics");
    if (!uri_node.is_str() || !diagnostics_node.is_arr()) return;
    diagnostics_by_uri[std::string(uri_node.as_str())] = diagnostics_node.to_string();
}

} // namespace cc::services::lsp
