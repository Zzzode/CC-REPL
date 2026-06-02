/// @file plugin.cppm
/// @brief Plugin system - concept, definition, capabilities, and manager.
/// Plugins extend CLI functionality with tools, commands, and hooks.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <concepts>
#include <format>
#include <ranges>
#include <chrono>

#include <uv.h>

export module cc.plugins.plugin;

import cc.types.types;

export namespace cc::plugins {

using cc::core::Result;
using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================
// Plugin Metadata
// ============================================================

/// Static plugin metadata loaded from manifest
struct PluginDefinition {
    std::string name;           // Unique plugin identifier
    std::string version;        // SemVer version string
    std::string author;         // Author name or org
    std::string description;    // Human-readable description
    std::string entry_point;    // Path to plugin executable/script
    std::optional<std::string> homepage;    // URL for docs
    std::optional<std::string> license;     // SPDX identifier
    std::vector<std::string> dependencies;  // Required plugin dependencies

    /// Serialize definition to JSON
    [[nodiscard]] std::string to_json() const {
        return std::format(
            R"({{"name":"{}","version":"{}","author":"{}","description":"{}"}})",
            name, version, author, description);
    }
};

// ============================================================
// Plugin Capabilities
// ============================================================

/// Describes what a plugin can provide to the system
struct PluginCapabilities {
    std::vector<std::string> tools;     // Tool names this plugin provides
    std::vector<std::string> commands;  // Slash commands it registers
    std::vector<std::string> hooks;     // Lifecycle hooks it subscribes to

    /// Check if plugin provides any capabilities
    [[nodiscard]] bool is_empty() const noexcept {
        return tools.empty() && commands.empty() && hooks.empty();
    }

    /// Total number of capabilities
    [[nodiscard]] std::size_t total() const noexcept {
        return tools.size() + commands.size() + hooks.size();
    }
};

// ============================================================
// Plugin Lifecycle State
// ============================================================

/// Plugin process lifecycle states
enum class PluginState : std::uint8_t {
    Unloaded,       // Not yet loaded
    Loaded,         // Manifest parsed, not started
    Initializing,   // Init message sent, awaiting response
    Running,        // Fully operational
    Stopping,       // Stop requested, waiting for cleanup
    Stopped,        // Gracefully terminated
    Error,          // Crashed or unresponsive
};

// ============================================================
// Plugin Concept
// ============================================================

/// Concept defining what constitutes a valid plugin type
template <typename T>
concept Plugin = requires(T plugin) {
    { plugin.definition() } -> std::same_as<const PluginDefinition&>;
    { plugin.capabilities() } -> std::same_as<const PluginCapabilities&>;
    { plugin.state() } -> std::same_as<PluginState>;
    { plugin.init() } -> std::same_as<VoidResult>;
    { plugin.start() } -> std::same_as<VoidResult>;
    { plugin.stop() } -> std::same_as<VoidResult>;
};

// ============================================================
// Plugin Instance (runtime representation)
// ============================================================

/// Runtime instance of a loaded plugin with its process handle
struct PluginInstance {
    PluginDefinition definition;
    PluginCapabilities capabilities;
    PluginState state = PluginState::Unloaded;

    // Process management
    uv_process_t process{};         // libuv process handle
    uv_pipe_t stdin_pipe{};         // Pipe to plugin stdin
    uv_pipe_t stdout_pipe{};        // Pipe from plugin stdout
    std::string output_buffer;      // Accumulated stdout data
    std::uint32_t pid = 0;          // OS process ID

    // Communication state
    std::uint64_t last_response_at = 0;  // Last response timestamp
    std::uint32_t pending_requests = 0;  // Outstanding RPC calls

    /// Check if the plugin process is healthy
    [[nodiscard]] bool is_healthy() const noexcept {
        return state == PluginState::Running && pid > 0;
    }
};

// ============================================================
// Plugin Communication (JSON-RPC over stdio)
// ============================================================

/// JSON-RPC message sent to/from plugin process
struct PluginMessage {
    std::string id;          // Request ID for correlation
    std::string method;      // RPC method name
    std::string params_json; // JSON-encoded parameters
    bool is_response = false;
    std::optional<std::string> result_json;
    std::optional<std::string> error_message;
};

// ============================================================
// PluginManager - orchestrates all plugins
// ============================================================

/// Central manager for plugin discovery, lifecycle, and communication.
/// Plugins run as isolated child processes communicating via JSON-RPC on stdio.
class PluginManager {
    uv_loop_t* loop_;  // libuv event loop (externally owned)
    std::unordered_map<std::string, std::unique_ptr<PluginInstance>> plugins_;
    std::vector<std::string> search_paths_;
    std::uint32_t next_request_id_ = 1;

    // Callbacks
    std::function<void(const std::string&, PluginState)> on_state_change_;

public:
    explicit PluginManager(uv_loop_t* loop) : loop_(loop) {}

    ~PluginManager() { shutdown_all(); }

    // Non-copyable
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Add a directory to search for plugins
    void add_search_path(std::string path) {
        search_paths_.push_back(std::move(path));
    }

    /// Register a plugin from its definition (does not start it)
    [[nodiscard]] VoidResult register_plugin(PluginDefinition def) {
        if (plugins_.contains(def.name)) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' already registered", def.name)));
        }

        auto instance = std::make_unique<PluginInstance>();
        instance->definition = std::move(def);
        instance->state = PluginState::Loaded;
        plugins_[instance->definition.name] = std::move(instance);
        return {};
    }

    /// Initialize a registered plugin (spawn process, send init)
    [[nodiscard]] VoidResult init_plugin(std::string_view name) {
        auto* inst = find_instance(name);
        if (!inst) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' not found", name)));
        }
        if (inst->state != PluginState::Loaded) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' not in Loaded state", name)));
        }

        // Spawn plugin process
        auto spawn_result = spawn_plugin(*inst);
        if (!spawn_result) return spawn_result;

        inst->state = PluginState::Initializing;
        notify_state(inst->definition.name, PluginState::Initializing);

        // Send initialization message
        send_to_plugin(*inst, PluginMessage{
            .id = std::to_string(next_request_id_++),
            .method = "initialize",
            .params_json = inst->definition.to_json(),
        });

        return {};
    }

    /// Start a plugin (must be initialized first)
    [[nodiscard]] VoidResult start_plugin(std::string_view name) {
        auto* inst = find_instance(name);
        if (!inst) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' not found", name)));
        }

        inst->state = PluginState::Running;
        notify_state(inst->definition.name, PluginState::Running);
        return {};
    }

    /// Stop a specific plugin gracefully
    [[nodiscard]] VoidResult stop_plugin(std::string_view name) {
        auto* inst = find_instance(name);
        if (!inst) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' not found", name)));
        }

        inst->state = PluginState::Stopping;
        notify_state(inst->definition.name, PluginState::Stopping);

        // Send shutdown notification then kill process
        send_to_plugin(*inst, PluginMessage{
            .id = std::to_string(next_request_id_++),
            .method = "shutdown",
            .params_json = "{}",
        });

        // Force kill after timeout via libuv timer (simplified: immediate kill)
        if (inst->pid > 0) {
            uv_process_kill(&inst->process, SIGTERM);
        }
        inst->state = PluginState::Stopped;
        notify_state(inst->definition.name, PluginState::Stopped);
        return {};
    }

    /// Shutdown all running plugins
    void shutdown_all() {
        for (auto& [name, inst] : plugins_) {
            if (inst->is_healthy()) {
                stop_plugin(name);
            }
        }
    }

    /// Send an RPC call to a plugin
    [[nodiscard]] VoidResult call_plugin(std::string_view name,
                                          std::string method,
                                          std::string params_json) {
        auto* inst = find_instance(name);
        if (!inst || !inst->is_healthy()) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' not running", name)));
        }

        send_to_plugin(*inst, PluginMessage{
            .id = std::to_string(next_request_id_++),
            .method = std::move(method),
            .params_json = std::move(params_json),
        });
        return {};
    }

    /// Get the state of a specific plugin
    [[nodiscard]] PluginState get_state(std::string_view name) const {
        auto it = plugins_.find(std::string(name));
        if (it == plugins_.end()) return PluginState::Unloaded;
        return it->second->state;
    }

    /// List all registered plugin names
    [[nodiscard]] std::vector<std::string_view> plugin_names() const {
        std::vector<std::string_view> names;
        names.reserve(plugins_.size());
        for (const auto& [name, _] : plugins_) {
            names.emplace_back(name);
        }
        return names;
    }

    /// Callback registration for state changes
    void on_state_change(std::function<void(const std::string&, PluginState)> cb) {
        on_state_change_ = std::move(cb);
    }

    /// Number of registered plugins
    [[nodiscard]] std::size_t size() const noexcept { return plugins_.size(); }

private:
    /// Find a plugin instance by name
    [[nodiscard]] PluginInstance* find_instance(std::string_view name) {
        auto it = plugins_.find(std::string(name));
        return (it != plugins_.end()) ? it->second.get() : nullptr;
    }

    /// Spawn a child process for the plugin
    [[nodiscard]] VoidResult spawn_plugin(PluginInstance& inst) {
        uv_pipe_init(loop_, &inst.stdin_pipe, 0);
        uv_pipe_init(loop_, &inst.stdout_pipe, 0);

        uv_stdio_container_t stdio[3];
        stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
        stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&inst.stdin_pipe);
        stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
        stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&inst.stdout_pipe);
        stdio[2].flags = UV_IGNORE;

        uv_process_options_t opts{};
        const char* args[] = {inst.definition.entry_point.c_str(), nullptr};
        opts.file = inst.definition.entry_point.c_str();
        opts.args = const_cast<char**>(args);
        opts.stdio_count = 3;
        opts.stdio = stdio;
        opts.exit_cb = on_process_exit;
        inst.process.data = &inst;

        int r = uv_spawn(loop_, &inst.process, &opts);
        if (r != 0) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Failed to spawn plugin '{}': {}",
                    inst.definition.name, uv_strerror(r))));
        }

        inst.pid = inst.process.pid;
        return {};
    }

    /// Send a JSON-RPC message to a plugin via its stdin pipe
    void send_to_plugin(PluginInstance& inst, const PluginMessage& msg) {
        // Format as JSON-RPC line-delimited message
        std::string json = std::format(
            R"({{"jsonrpc":"2.0","id":"{}","method":"{}","params":{}}})""\n",
            msg.id, msg.method, msg.params_json);

        auto* req = new uv_write_t;
        auto* buf_data = new std::string(std::move(json));
        uv_buf_t buf = uv_buf_init(buf_data->data(), static_cast<unsigned int>(buf_data->size()));
        req->data = buf_data;
        uv_write(req, reinterpret_cast<uv_stream_t*>(&inst.stdin_pipe),
                 &buf, 1, on_plugin_write_done);
        ++inst.pending_requests;
    }

    /// Notify listeners of plugin state changes
    void notify_state(const std::string& name, PluginState state) {
        if (on_state_change_) on_state_change_(name, state);
    }

    // --- Static callbacks ---
    static void on_process_exit(uv_process_t* proc, int64_t exit_status, int) {
        auto* inst = static_cast<PluginInstance*>(proc->data);
        inst->state = (exit_status == 0) ? PluginState::Stopped : PluginState::Error;
        inst->pid = 0;
    }

    static void on_plugin_write_done(uv_write_t* req, int) {
        delete static_cast<std::string*>(req->data);
        delete req;
    }
};

// ============================================================
// PluginRegistry - manages installed plugin metadata
// ============================================================

/// Registry for tracking installed plugins and their metadata.
/// Separate from PluginManager which handles runtime lifecycle.
class PluginRegistry {
    std::unordered_map<std::string, PluginDefinition> installed_;

public:
    /// Register a plugin definition
    void install(PluginDefinition def) {
        installed_[def.name] = std::move(def);
    }

    /// Unregister a plugin
    void uninstall(std::string_view name) {
        installed_.erase(std::string(name));
    }

    /// Check if a plugin is installed
    [[nodiscard]] bool is_installed(std::string_view name) const {
        return installed_.contains(std::string(name));
    }

    /// Get a plugin's definition
    [[nodiscard]] const PluginDefinition* get(std::string_view name) const {
        auto it = installed_.find(std::string(name));
        return (it != installed_.end()) ? &it->second : nullptr;
    }

    /// Get all installed plugin definitions
    [[nodiscard]] std::vector<const PluginDefinition*> all() const {
        std::vector<const PluginDefinition*> result;
        result.reserve(installed_.size());
        for (const auto& [_, def] : installed_) {
            result.push_back(&def);
        }
        return result;
    }

    /// Number of installed plugins
    [[nodiscard]] std::size_t size() const noexcept { return installed_.size(); }
};

} // namespace cc::plugins
