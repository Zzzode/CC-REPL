/// @file bridge_types.cppm
/// @brief Bridge protocol types

module;

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.bridge.bridge_types;

export namespace cc::bridge {

// ---- Constants ----

/// Default per-session timeout (24 hours).
constexpr auto DEFAULT_SESSION_TIMEOUT_MS = std::chrono::hours(24);

constexpr std::string_view BRIDGE_LOGIN_INSTRUCTION{
    "Remote Control is only available with claude.ai subscriptions. "
    "Please use `/login` to sign in with your claude.ai account."};

constexpr std::string_view BRIDGE_LOGIN_ERROR{
    "Error: You must be logged in to use Remote Control.\n\n"
    "Remote Control is only available with claude.ai subscriptions. "
    "Please use `/login` to sign in with your claude.ai account."};

constexpr std::string_view REMOTE_CONTROL_DISCONNECTED_MSG{
    "Remote Control disconnected."};


// ---- Enums ----

enum class WorkDataType { session, healthcheck };

enum class SessionDoneStatus { completed, failed, interrupted };

enum class SessionActivityType { tool_start, text, result, error };

enum class SpawnMode { single_session, worktree, same_dir };

enum class BridgeWorkerType { claude_code, claude_code_assistant };


// ---- Data structs ----

struct GitInfo {
    std::string type;
    std::string repo;
    std::optional<std::string> ref;
    std::optional<std::string> token;
};

struct WorkSource {
    std::string type;
    std::optional<GitInfo> git_info;
};

struct WorkAuthEntry {
    std::string type;
    std::string token;
};

struct WorkData {
    WorkDataType type{WorkDataType::session};
    std::string id;
};

struct WorkResponse {
    std::string id;
    std::string type{"work"};
    std::string environment_id;
    std::string state;
    WorkData data;
    std::string secret; // base64url-encoded JSON
    std::string created_at;
};

struct WorkSecret {
    int version{0};
    std::string session_ingress_token;
    std::string api_base_url;
    std::vector<WorkSource> sources;
    std::vector<WorkAuthEntry> auth;
    std::optional<std::unordered_map<std::string, std::string>> claude_code_args;
    std::optional<std::string> mcp_config_json;       // unknown | null -> nullopt
    std::optional<std::unordered_map<std::string, std::string>> environment_variables;
    std::optional<bool> use_code_sessions;
};

struct SessionActivity {
    SessionActivityType type{SessionActivityType::text};
    std::string summary;
    std::chrono::milliseconds timestamp{0};
};


// ---- BridgeConfig (worker-side) ----

struct BridgeConfig {
    std::string dir = {};
    std::string machine_name = {};
    std::string branch = {};
    std::optional<std::string> git_repo_url = std::nullopt;
    int max_sessions{1};
    SpawnMode spawn_mode{SpawnMode::single_session};
    bool verbose{false};
    bool sandbox{false};
    std::string bridge_id = {};
    std::string worker_type = {};
    std::string environment_id = {};
    std::optional<std::string> reuse_environment_id = std::nullopt;
    std::string api_base_url = {};
    std::string session_ingress_url = {};
    std::optional<std::string> debug_file = std::nullopt;
    std::optional<std::chrono::milliseconds> session_timeout_ms = std::nullopt;
};


// ---- Permission response ----

struct PermissionSuccessResponse {
    std::string request_id;
    std::unordered_map<std::string, std::string> response;
};

struct PermissionResponseEvent {
    std::string type{"control_response"};
    PermissionSuccessResponse response;
};


// ---- Session spawning ----

struct SessionSpawnOpts {
    std::string session_id;
    std::string sdk_url;
    std::string access_token;
    std::optional<bool> use_ccr_v2;
    std::optional<int> worker_epoch;
    std::function<void(const std::string&)> on_first_user_message;
};


// ---- Abstract interfaces ----

/// Abstract handle to an active bridge session (mirrors the TS SessionHandle object type).
class SessionHandle {
public:
    virtual ~SessionHandle() = default;

    virtual auto session_id() const -> const std::string& = 0;
    virtual auto done_status() const -> SessionDoneStatus = 0;
    virtual auto activities() const -> const std::vector<SessionActivity>& = 0;
    virtual auto current_activity() const -> const SessionActivity* = 0;
    virtual auto access_token() const -> const std::string& = 0;
    virtual auto last_stderr() const -> const std::vector<std::string>& = 0;

    virtual void kill() = 0;
    virtual void force_kill() = 0;
    virtual void write_stdin(std::string_view data) = 0;
    virtual void update_access_token(std::string token) = 0;
};

/// Abstract factory for spawning bridge sessions (mirrors the TS SessionSpawner interface).
class SessionSpawner {
public:
    virtual ~SessionSpawner() = default;

    virtual auto spawn(const SessionSpawnOpts& opts, const std::string& dir)
        -> std::unique_ptr<SessionHandle> = 0;
};

/// Abstract logger interface for bridge subsystems.
class BridgeLogger {
public:
    virtual ~BridgeLogger() = default;

    virtual void info(std::string_view msg) = 0;
    virtual void warn(std::string_view msg) = 0;
    virtual void error(std::string_view msg) = 0;
    virtual void debug(std::string_view msg) = 0;
};


} // namespace cc::bridge
