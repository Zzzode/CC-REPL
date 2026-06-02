// C++23 Swarm Backends Module
// Merges: detection.ts, InProcessBackend.ts, ITermBackend.ts, TmuxBackend.ts,
//         PaneBackendExecutor.ts, registry.ts, types.ts, teammateModeSnapshot.ts, it2Setup.ts
module;

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.swarm_backends;

export namespace cc::utils::swarm_backends {

// ============================================================================
// Types & Enums (from types.ts)
// ============================================================================

/// Types of backends available for teammate execution.
enum class BackendType {
    Tmux,       ///< Uses tmux for pane management (works in tmux or standalone)
    ITerm2,     ///< Uses iTerm2 native split panes via the it2 CLI
    InProcess,  ///< Runs teammate in same process with isolated context
};

/// Convert BackendType to string
[[nodiscard]] inline std::string_view backend_type_name(BackendType type) {
    switch (type) {
        case BackendType::Tmux: return "tmux";
        case BackendType::ITerm2: return "iterm2";
        case BackendType::InProcess: return "in-process";
    }
    return "unknown";
}

/// Subset of BackendType for pane-based backends only
enum class PaneBackendType {
    Tmux,
    ITerm2,
};

/// Agent color names for pane borders and titles
enum class AgentColor {
    Red, Blue, Green, Yellow, Purple, Orange, Pink, Cyan,
};

/// Convert AgentColor to display string
[[nodiscard]] inline std::string_view agent_color_name(AgentColor color) {
    switch (color) {
        case AgentColor::Red: return "red";
        case AgentColor::Blue: return "blue";
        case AgentColor::Green: return "green";
        case AgentColor::Yellow: return "yellow";
        case AgentColor::Purple: return "purple";
        case AgentColor::Orange: return "orange";
        case AgentColor::Pink: return "pink";
        case AgentColor::Cyan: return "cyan";
    }
    return "unknown";
}

/// Opaque pane identifier.
/// For tmux: pane ID (e.g., "%1"). For iTerm2: session UUID.
using PaneId = std::string;

/// Result of creating a new teammate pane
struct CreatePaneResult {
    PaneId pane_id;
    bool is_first_teammate = false;
};

/// Identity fields for a teammate
struct TeammateIdentity {
    std::string name;                       ///< Agent name (e.g., "researcher")
    std::string team_name;                  ///< Team name this teammate belongs to
    std::optional<AgentColor> color;        ///< Assigned color for UI differentiation
    bool plan_mode_required = false;        ///< Whether plan mode approval required
};

/// Configuration for spawning a teammate (any execution mode)
struct TeammateSpawnConfig {
    // Identity
    std::string name;
    std::string team_name;
    std::optional<AgentColor> color;
    bool plan_mode_required = false;

    // Execution config
    std::string prompt;                     ///< Initial prompt to send
    std::string cwd;                        ///< Working directory
    std::optional<std::string> model;       ///< Model to use
    std::optional<std::string> system_prompt;
    std::string system_prompt_mode = "default"; ///< "default", "replace", "append"
    std::optional<std::string> worktree_path;
    std::string parent_session_id;

    // Permissions
    std::vector<std::string> permissions;
    bool allow_permission_prompts = false;
};

/// Result from spawning a teammate
struct TeammateSpawnResult {
    bool success = false;
    std::string agent_id;                   ///< Unique ID: agentName@teamName
    std::optional<std::string> error;
    std::optional<std::string> task_id;     ///< Task ID in app state (in-process)
    std::optional<PaneId> pane_id;          ///< Pane ID (pane-based)
};

/// Message to send to a teammate
struct TeammateMessage {
    std::string text;
    std::string from;
    std::optional<std::string> color;
    std::optional<std::string> timestamp;   ///< ISO string
    std::optional<std::string> summary;     ///< 5-10 word preview for UI
};

/// Result from backend detection
struct BackendDetectionResult {
    BackendType backend_type;
    bool is_native = false;         ///< Running inside the backend's environment
    bool needs_it2_setup = false;   ///< iTerm2 detected but it2 not installed
};

/// Teammate mode as captured at session startup
enum class TeammateMode {
    Auto,       ///< Detect based on environment
    Tmux,       ///< Force pane-based via tmux
    InProcess,  ///< Force in-process execution
};

/// Type guard: is this a pane-based backend type?
[[nodiscard]] inline bool is_pane_backend(BackendType type) {
    return type == BackendType::Tmux || type == BackendType::ITerm2;
}

// ============================================================================
// PaneBackend — Abstract interface for pane management (from types.ts)
// ============================================================================

/// Abstract interface for pane management backends.
/// Abstracts operations for creating and managing terminal panes
/// for teammate visualization in swarm mode.
class PaneBackend {
public:
    virtual ~PaneBackend() = default;

    /// The backend type identifier
    [[nodiscard]] virtual BackendType type() const = 0;

    /// Human-readable display name
    [[nodiscard]] virtual std::string_view display_name() const = 0;

    /// Whether this backend supports hiding and showing panes
    [[nodiscard]] virtual bool supports_hide_show() const = 0;

    /// Check if this backend is available on the system
    [[nodiscard]] virtual bool is_available() const = 0;

    /// Check if we're running inside this backend's environment
    [[nodiscard]] virtual bool is_running_inside() const = 0;

    /// Create a new pane for a teammate in the swarm view
    [[nodiscard]] virtual CreatePaneResult create_teammate_pane(
        std::string_view name, AgentColor color) = 0;

    /// Send a command to a specific pane
    virtual void send_command_to_pane(
        const PaneId& pane_id, std::string_view command,
        bool use_external_session = false) = 0;

    /// Set the border color for a pane
    virtual void set_pane_border_color(
        const PaneId& pane_id, AgentColor color,
        bool use_external_session = false) = 0;

    /// Set the title for a pane
    virtual void set_pane_title(
        const PaneId& pane_id, std::string_view name, AgentColor color,
        bool use_external_session = false) = 0;

    /// Enable pane border status display (shows titles in borders)
    virtual void enable_pane_border_status(
        std::optional<std::string_view> window_target = std::nullopt,
        bool use_external_session = false) = 0;

    /// Rebalance panes to achieve desired layout
    virtual void rebalance_panes(
        std::string_view window_target, bool has_leader) = 0;

    /// Kill/close a specific pane
    [[nodiscard]] virtual bool kill_pane(
        const PaneId& pane_id, bool use_external_session = false) = 0;

    /// Hide a pane by breaking it out into a hidden window
    [[nodiscard]] virtual bool hide_pane(
        const PaneId& pane_id, bool use_external_session = false) = 0;

    /// Show a previously hidden pane
    [[nodiscard]] virtual bool show_pane(
        const PaneId& pane_id, std::string_view target_window_or_pane,
        bool use_external_session = false) = 0;
};

// ============================================================================
// TeammateExecutor — Abstract interface for teammate lifecycle (from types.ts)
// ============================================================================

/// Common interface for teammate execution backends.
/// Abstracts differences between pane-based and in-process execution.
class TeammateExecutor {
public:
    virtual ~TeammateExecutor() = default;

    /// Backend type identifier
    [[nodiscard]] virtual BackendType type() const = 0;

    /// Check if this executor is available on the system
    [[nodiscard]] virtual bool is_available() const = 0;

    /// Spawn a new teammate with the given configuration
    [[nodiscard]] virtual TeammateSpawnResult spawn(
        const TeammateSpawnConfig& config) = 0;

    /// Send a message to a teammate
    virtual void send_message(
        std::string_view agent_id, const TeammateMessage& message) = 0;

    /// Terminate a teammate (graceful shutdown request)
    [[nodiscard]] virtual bool terminate(
        std::string_view agent_id, std::optional<std::string_view> reason = std::nullopt) = 0;

    /// Force kill a teammate (immediate termination)
    [[nodiscard]] virtual bool kill(std::string_view agent_id) = 0;

    /// Check if a teammate is still active
    [[nodiscard]] virtual bool is_active(std::string_view agent_id) const = 0;
};

// ============================================================================
// Detection (from detection.ts)
// ============================================================================

/// Environment detection utilities for swarm backend selection.
/// Caches results since environment won't change during process lifetime.
class EnvironmentDetection {
public:
    /// Check if currently running inside a tmux session (synchronous).
    /// Uses TMUX env var captured at module load.
    [[nodiscard]] static bool is_inside_tmux_sync() {
        return !original_tmux_env_.empty();
    }

    /// Check if currently running inside a tmux session.
    /// Caches the result.
    [[nodiscard]] static bool is_inside_tmux() {
        std::lock_guard lock(mutex_);
        if (!tmux_cached_) {
            is_inside_tmux_result_ = !original_tmux_env_.empty();
            tmux_cached_ = true;
        }
        return is_inside_tmux_result_;
    }

    /// Get the leader's tmux pane ID captured at module load.
    /// Returns empty if not running inside tmux.
    [[nodiscard]] static std::string_view get_leader_pane_id() {
        return original_tmux_pane_;
    }

    /// Check if tmux is available on the system (installed and in PATH)
    [[nodiscard]] static bool is_tmux_available();

    /// Check if currently running inside iTerm2.
    /// Uses multiple detection methods: TERM_PROGRAM, ITERM_SESSION_ID.
    [[nodiscard]] static bool is_in_iterm2() {
        std::lock_guard lock(mutex_);
        if (!iterm2_cached_) {
            is_in_iterm2_result_ = detect_iterm2();
            iterm2_cached_ = true;
        }
        return is_in_iterm2_result_;
    }

    /// Check if the it2 CLI tool is available AND can reach iTerm2 Python API
    [[nodiscard]] static bool is_it2_cli_available();

    /// Reset all cached detection results (for testing)
    static void reset_cache() {
        std::lock_guard lock(mutex_);
        tmux_cached_ = false;
        iterm2_cached_ = false;
    }

    /// Set the original TMUX env value (called at startup)
    static void capture_env(std::string_view tmux_env, std::string_view tmux_pane) {
        original_tmux_env_ = std::string(tmux_env);
        original_tmux_pane_ = std::string(tmux_pane);
    }

private:
    static bool detect_iterm2() {
        // Check TERM_PROGRAM and ITERM_SESSION_ID environment variables
        const char* term_program = std::getenv("TERM_PROGRAM");
        if (term_program && std::string_view(term_program) == "iTerm.app") {
            return true;
        }
        const char* iterm_session = std::getenv("ITERM_SESSION_ID");
        return iterm_session != nullptr && iterm_session[0] != '\0';
    }

    static inline std::mutex mutex_;
    static inline bool tmux_cached_ = false;
    static inline bool is_inside_tmux_result_ = false;
    static inline bool iterm2_cached_ = false;
    static inline bool is_in_iterm2_result_ = false;
    static inline std::string original_tmux_env_;
    static inline std::string original_tmux_pane_;
};

// ============================================================================
// TmuxBackend (from TmuxBackend.ts)
// ============================================================================

/// TmuxBackend implements PaneBackend using tmux for pane management.
///
/// When running INSIDE tmux (leader is in tmux):
/// - Splits the current window to add teammates alongside the leader
/// - Leader stays on left (30%), teammates on right (70%)
///
/// When running OUTSIDE tmux (leader is in regular terminal):
/// - Creates a claude-swarm session with a swarm-view window
/// - All teammates are equally distributed (no leader pane)
class TmuxBackend : public PaneBackend {
public:
    [[nodiscard]] BackendType type() const override { return BackendType::Tmux; }
    [[nodiscard]] std::string_view display_name() const override { return "tmux"; }
    [[nodiscard]] bool supports_hide_show() const override { return true; }

    [[nodiscard]] bool is_available() const override;
    [[nodiscard]] bool is_running_inside() const override;

    [[nodiscard]] CreatePaneResult create_teammate_pane(
        std::string_view name, AgentColor color) override;

    void send_command_to_pane(
        const PaneId& pane_id, std::string_view command,
        bool use_external_session = false) override;

    void set_pane_border_color(
        const PaneId& pane_id, AgentColor color,
        bool use_external_session = false) override;

    void set_pane_title(
        const PaneId& pane_id, std::string_view name, AgentColor color,
        bool use_external_session = false) override;

    void enable_pane_border_status(
        std::optional<std::string_view> window_target = std::nullopt,
        bool use_external_session = false) override;

    void rebalance_panes(
        std::string_view window_target, bool has_leader) override;

    [[nodiscard]] bool kill_pane(
        const PaneId& pane_id, bool use_external_session = false) override;

    [[nodiscard]] bool hide_pane(
        const PaneId& pane_id, bool use_external_session = false) override;

    [[nodiscard]] bool show_pane(
        const PaneId& pane_id, std::string_view target_window_or_pane,
        bool use_external_session = false) override;

private:
    /// Create teammate pane when inside tmux (with leader)
    CreatePaneResult create_pane_with_leader(std::string_view name, AgentColor color);

    /// Create teammate pane when outside tmux (external session)
    CreatePaneResult create_pane_external(std::string_view name, AgentColor color);

    /// Rebalance panes with leader at 30% width
    void rebalance_with_leader(std::string_view window_target);

    /// Rebalance panes with tiled layout (no leader)
    void rebalance_tiled(std::string_view window_target);

    /// Get current pane ID (leader's pane)
    [[nodiscard]] std::optional<std::string> get_current_pane_id() const;

    /// Get current window target (session:window format)
    [[nodiscard]] std::optional<std::string> get_current_window_target() const;

    /// Get tmux color name for agent color
    [[nodiscard]] static std::string_view get_tmux_color(AgentColor color);

    /// Pane creation lock to prevent race conditions
    std::mutex pane_creation_mutex_;

    /// Track first pane usage for external session
    bool first_pane_used_external_ = false;

    /// Cached leader window target
    mutable std::optional<std::string> cached_leader_window_;

    /// Shell init delay after pane creation (ms)
    static constexpr int kPaneShellInitDelayMs = 200;
};

// ============================================================================
// ITermBackend (from ITermBackend.ts)
// ============================================================================

/// ITermBackend implements pane management using iTerm2's native split panes
/// via the it2 CLI tool.
///
/// Layout strategy:
/// - First teammate: vertical split from leader's session
/// - Subsequent teammates: horizontal split from last teammate
///
/// Includes at-fault recovery: if a targeted teammate session is dead,
/// prune it and retry with the next-to-last.
class ITermBackend : public PaneBackend {
public:
    [[nodiscard]] BackendType type() const override { return BackendType::ITerm2; }
    [[nodiscard]] std::string_view display_name() const override { return "iTerm2"; }
    [[nodiscard]] bool supports_hide_show() const override { return false; }

    [[nodiscard]] bool is_available() const override;
    [[nodiscard]] bool is_running_inside() const override;

    [[nodiscard]] CreatePaneResult create_teammate_pane(
        std::string_view name, AgentColor color) override;

    void send_command_to_pane(
        const PaneId& pane_id, std::string_view command,
        bool use_external_session = false) override;

    /// No-op for iTerm2 — color requires slow Python API calls
    void set_pane_border_color(
        const PaneId& pane_id, AgentColor color,
        bool use_external_session = false) override;

    /// No-op for iTerm2 — title requires slow Python API calls
    void set_pane_title(
        const PaneId& pane_id, std::string_view name, AgentColor color,
        bool use_external_session = false) override;

    /// No-op for iTerm2 — pane titles shown in tabs automatically
    void enable_pane_border_status(
        std::optional<std::string_view> window_target = std::nullopt,
        bool use_external_session = false) override;

    /// No-op for iTerm2 — pane balancing handled automatically
    void rebalance_panes(
        std::string_view window_target, bool has_leader) override;

    [[nodiscard]] bool kill_pane(
        const PaneId& pane_id, bool use_external_session = false) override;

    /// Not supported in iTerm2 (no equivalent to tmux break-pane)
    [[nodiscard]] bool hide_pane(
        const PaneId& pane_id, bool use_external_session = false) override;

    /// Not supported in iTerm2 (no equivalent to tmux join-pane)
    [[nodiscard]] bool show_pane(
        const PaneId& pane_id, std::string_view target_window_or_pane,
        bool use_external_session = false) override;

private:
    /// Parse session ID from `it2 session split` output
    [[nodiscard]] static std::string parse_split_output(std::string_view output);

    /// Get the leader's session ID from ITERM_SESSION_ID env var
    [[nodiscard]] static std::optional<std::string> get_leader_session_id();

    /// Pane creation lock
    std::mutex pane_creation_mutex_;

    /// Track session IDs for teammates
    std::vector<std::string> teammate_session_ids_;

    /// Track whether first pane has been used
    bool first_pane_used_ = false;
};

// ============================================================================
// InProcessBackend (from InProcessBackend.ts)
// ============================================================================

/// InProcessBackend implements TeammateExecutor for in-process teammates.
///
/// Unlike pane-based backends, in-process teammates run in the same process
/// with isolated context. They:
/// - Share resources (API client, connections) with the leader
/// - Communicate via file-based mailbox
/// - Are terminated via abort signaling (not kill-pane)
class InProcessBackend : public TeammateExecutor {
public:
    [[nodiscard]] BackendType type() const override { return BackendType::InProcess; }

    /// In-process backend is always available (no external dependencies)
    [[nodiscard]] bool is_available() const override { return true; }

    [[nodiscard]] TeammateSpawnResult spawn(
        const TeammateSpawnConfig& config) override;

    void send_message(
        std::string_view agent_id, const TeammateMessage& message) override;

    [[nodiscard]] bool terminate(
        std::string_view agent_id, std::optional<std::string_view> reason = std::nullopt) override;

    [[nodiscard]] bool kill(std::string_view agent_id) override;

    [[nodiscard]] bool is_active(std::string_view agent_id) const override;

private:
    /// Track active in-process teammates
    mutable std::mutex mutex_;
    std::map<std::string, bool> active_teammates_;
};

// ============================================================================
// PaneBackendExecutor — Adapter: PaneBackend -> TeammateExecutor
// (from PaneBackendExecutor.ts)
// ============================================================================

/// PaneBackendExecutor adapts a PaneBackend to the TeammateExecutor interface.
///
/// This allows pane-based backends (tmux, iTerm2) to be used through the same
/// TeammateExecutor abstraction as InProcessBackend.
///
/// Handles:
/// - spawn(): Creates a pane and sends the CLI command to it
/// - sendMessage(): Writes to teammate's file-based mailbox
/// - terminate(): Sends a shutdown request via mailbox
/// - kill(): Kills the pane via the backend
/// - isActive(): Checks if the pane is still running
class PaneBackendExecutor : public TeammateExecutor {
public:
    explicit PaneBackendExecutor(std::shared_ptr<PaneBackend> backend)
        : backend_(std::move(backend)) {}

    [[nodiscard]] BackendType type() const override { return backend_->type(); }

    [[nodiscard]] bool is_available() const override {
        return backend_->is_available();
    }

    [[nodiscard]] TeammateSpawnResult spawn(
        const TeammateSpawnConfig& config) override;

    void send_message(
        std::string_view agent_id, const TeammateMessage& message) override;

    [[nodiscard]] bool terminate(
        std::string_view agent_id, std::optional<std::string_view> reason = std::nullopt) override;

    [[nodiscard]] bool kill(std::string_view agent_id) override;

    [[nodiscard]] bool is_active(std::string_view agent_id) const override;

private:
    std::shared_ptr<PaneBackend> backend_;

    /// Track spawned teammates: agentId -> {paneId, insideTmux}
    struct TeammateInfo {
        PaneId pane_id;
        bool inside_tmux = false;
    };
    mutable std::mutex spawned_mutex_;
    std::map<std::string, TeammateInfo> spawned_teammates_;
};

// ============================================================================
// TeammateModeSnapshot (from teammateModeSnapshot.ts)
// ============================================================================

/// Captures teammate mode at session startup.
/// Ensures runtime config changes don't affect mode for current session.
class TeammateModeSnapshot {
public:
    /// Set CLI override for teammate mode. Must be called before capture().
    static void set_cli_override(TeammateMode mode) {
        std::lock_guard lock(mutex_);
        cli_override_ = mode;
    }

    /// Get current CLI override, if any
    [[nodiscard]] static std::optional<TeammateMode> get_cli_override() {
        std::lock_guard lock(mutex_);
        return cli_override_;
    }

    /// Clear CLI override and update snapshot to new mode.
    /// Called when user changes setting in UI.
    static void clear_cli_override(TeammateMode new_mode) {
        std::lock_guard lock(mutex_);
        cli_override_ = std::nullopt;
        captured_mode_ = new_mode;
    }

    /// Capture the teammate mode at session startup.
    /// CLI override takes precedence over config.
    static void capture() {
        std::lock_guard lock(mutex_);
        if (cli_override_.has_value()) {
            captured_mode_ = *cli_override_;
        } else {
            // Default to Auto if not configured
            captured_mode_ = TeammateMode::Auto;
        }
        captured_ = true;
    }

    /// Get the teammate mode for this session.
    /// Returns the snapshot captured at startup.
    [[nodiscard]] static TeammateMode get() {
        std::lock_guard lock(mutex_);
        if (!captured_) {
            capture_unlocked();
        }
        return captured_mode_;
    }

private:
    static void capture_unlocked() {
        if (cli_override_.has_value()) {
            captured_mode_ = *cli_override_;
        } else {
            captured_mode_ = TeammateMode::Auto;
        }
        captured_ = true;
    }

    static inline std::mutex mutex_;
    static inline bool captured_ = false;
    static inline TeammateMode captured_mode_ = TeammateMode::Auto;
    static inline std::optional<TeammateMode> cli_override_;
};

// ============================================================================
// It2Setup — iTerm2 it2 CLI installation and verification (from it2Setup.ts)
// ============================================================================

/// Python package manager types for installing it2
enum class PythonPackageManager {
    Uvx,    ///< uv tool install (preferred)
    Pipx,   ///< pipx (good for isolated environments)
    Pip,    ///< pip install --user (fallback)
};

/// Result of attempting to install it2
struct It2InstallResult {
    bool success = false;
    std::optional<std::string> error;
    std::optional<PythonPackageManager> package_manager;
};

/// Result of verifying it2 setup
struct It2VerifyResult {
    bool success = false;
    std::optional<std::string> error;
    bool needs_python_api_enabled = false;
};

/// Utilities for iTerm2 it2 CLI setup and verification
class It2Setup {
public:
    /// Detect which Python package manager is available.
    /// Checks in order: uvx, pipx, pip.
    [[nodiscard]] static std::optional<PythonPackageManager> detect_package_manager();

    /// Install the it2 CLI using the detected package manager
    [[nodiscard]] static It2InstallResult install(PythonPackageManager pm);

    /// Verify it2 is properly configured and can communicate with iTerm2
    [[nodiscard]] static It2VerifyResult verify();

    /// Get instructions for enabling Python API in iTerm2
    [[nodiscard]] static std::vector<std::string> get_python_api_instructions() {
        return {
            "Almost done! Enable the Python API in iTerm2:",
            "",
            "  iTerm2 → Settings → General → Magic → Enable Python API",
            "",
            "After enabling, you may need to restart iTerm2.",
        };
    }

    /// Mark it2 setup complete (prevents showing prompt again)
    static void mark_setup_complete();

    /// Set user preference for tmux over iTerm2
    static void set_prefer_tmux(bool prefer);

    /// Check if user prefers tmux over iTerm2
    [[nodiscard]] static bool get_prefer_tmux();

private:
    static inline std::mutex mutex_;
    static inline bool setup_complete_ = false;
    static inline bool prefer_tmux_ = false;
};

// ============================================================================
// BackendRegistry — Detection and caching of backends (from registry.ts)
// ============================================================================

/// Central registry for backend detection, caching, and retrieval.
///
/// Detection priority:
/// 1. If inside tmux → always use tmux (even in iTerm2)
/// 2. If in iTerm2 with it2 available → use iTerm2 backend
/// 3. If in iTerm2 without it2 → return result indicating setup needed
/// 4. If tmux available → use tmux (creates external session)
/// 5. Otherwise → throw error with installation instructions
class BackendRegistry {
public:
    /// Detect and get the appropriate pane backend for this environment.
    /// Caches the result after first detection.
    [[nodiscard]] static BackendDetectionResult detect_and_get_backend();

    /// Get a backend by explicit type selection
    [[nodiscard]] static std::shared_ptr<PaneBackend> get_backend_by_type(PaneBackendType type);

    /// Get the currently cached backend, if any
    [[nodiscard]] static std::shared_ptr<PaneBackend> get_cached_backend() {
        std::lock_guard lock(mutex_);
        return cached_backend_;
    }

    /// Get the cached detection result, if any
    [[nodiscard]] static std::optional<BackendDetectionResult> get_cached_detection_result() {
        std::lock_guard lock(mutex_);
        return cached_detection_result_;
    }

    /// Record that spawn fell back to in-process mode
    static void mark_in_process_fallback() {
        std::lock_guard lock(mutex_);
        in_process_fallback_active_ = true;
    }

    /// Check if in-process teammate execution is enabled.
    ///
    /// Logic:
    /// - If teammateMode is InProcess → always enabled
    /// - If teammateMode is Tmux → always disabled
    /// - If teammateMode is Auto (default) → check environment:
    ///   - If inside tmux → use pane backend (false)
    ///   - If inside iTerm2 → use pane backend (false)
    ///   - Otherwise → use in-process (true)
    [[nodiscard]] static bool is_in_process_enabled();

    /// Get the resolved teammate mode for this session.
    /// Unlike TeammateMode which may be Auto, this returns what Auto resolves to.
    [[nodiscard]] static TeammateMode get_resolved_teammate_mode() {
        return is_in_process_enabled() ? TeammateMode::InProcess : TeammateMode::Tmux;
    }

    /// Get the InProcessBackend instance (creates on first call)
    [[nodiscard]] static std::shared_ptr<TeammateExecutor> get_in_process_backend();

    /// Get a TeammateExecutor for spawning teammates.
    /// Returns InProcessBackend or PaneBackendExecutor based on configuration.
    [[nodiscard]] static std::shared_ptr<TeammateExecutor> get_teammate_executor(
        bool prefer_in_process = false);

    /// Reset backend detection cache (for testing)
    static void reset() {
        std::lock_guard lock(mutex_);
        cached_backend_.reset();
        cached_detection_result_.reset();
        cached_in_process_backend_.reset();
        cached_pane_executor_.reset();
        in_process_fallback_active_ = false;
    }

    /// Get platform-specific tmux installation instructions
    [[nodiscard]] static std::string get_tmux_install_instructions();

private:
    static inline std::mutex mutex_;
    static inline std::shared_ptr<PaneBackend> cached_backend_;
    static inline std::optional<BackendDetectionResult> cached_detection_result_;
    static inline std::shared_ptr<TeammateExecutor> cached_in_process_backend_;
    static inline std::shared_ptr<TeammateExecutor> cached_pane_executor_;
    static inline bool in_process_fallback_active_ = false;
};

// ============================================================================
// Utility: Format/Parse agent IDs
// ============================================================================

/// Format an agent ID from name and team: "agentName@teamName"
[[nodiscard]] inline std::string format_agent_id(
    std::string_view agent_name, std::string_view team_name) {
    std::string result;
    result.reserve(agent_name.size() + 1 + team_name.size());
    result.append(agent_name);
    result.push_back('@');
    result.append(team_name);
    return result;
}

/// Parsed agent ID components
struct ParsedAgentId {
    std::string agent_name;
    std::string team_name;
};

/// Parse an agent ID string into components.
/// Returns nullopt if format is invalid (missing '@').
[[nodiscard]] inline std::optional<ParsedAgentId> parse_agent_id(std::string_view agent_id) {
    auto pos = agent_id.find('@');
    if (pos == std::string_view::npos || pos == 0 || pos == agent_id.size() - 1) {
        return std::nullopt;
    }
    return ParsedAgentId{
        .agent_name = std::string(agent_id.substr(0, pos)),
        .team_name = std::string(agent_id.substr(pos + 1)),
    };
}

// ============================================================================
// Constants (swarm-related constants from constants.ts)
// ============================================================================

/// Tmux command name
inline constexpr std::string_view TMUX_COMMAND = "tmux";

/// it2 CLI command name
inline constexpr std::string_view IT2_COMMAND = "it2";

/// Swarm session name for external tmux sessions
inline constexpr std::string_view SWARM_SESSION_NAME = "claude-swarm";

/// Window name in the swarm session
inline constexpr std::string_view SWARM_VIEW_WINDOW_NAME = "swarm-view";

/// Hidden session name for pane hide/show
inline constexpr std::string_view HIDDEN_SESSION_NAME = "claude-swarm-hidden";

} // namespace cc::utils::swarm_backends
