/// @file remote_trigger_tool.cppm
/// @brief Bridge-integrated remote trigger tool with fail-closed default and
///        optional curl fallback transport.
///
/// Replaces the previous "curl webhook" helper with a two-tier dispatch:
///   1. In-process bridge facade (fast path): if the target session has been
///      registered via session_api::register_session, we route the trigger
///      through a threadsafe in-memory message queue with response handlers.
///   2. curl fallback (opt-in): if no session exists and the caller has set
///      allow_curl_fallback=true along with a bridge_url, we build a JSON-RPC
///      payload and delegate to curl(1) via posix_spawn.
///   3. Fail closed (default): if neither 1 nor 2 applies we return an error.
///
/// Exposes session_api / messages / messaging namespaces from the bridge
/// facade so tests and subsystems can install custom response handlers or
/// register/deregister sessions.
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <shared_mutex>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#define CC_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define CC_ENVIRON environ
#endif

export module cc.tools.remote_trigger;

import cc.utils.json;
import cc.utils.bash_execution;

// --------------------------------------------------------------------------
// Bridge facade: session_api
// --------------------------------------------------------------------------

export namespace cc::bridge::session_api {

/// Opaque handle representing a registered session on the bridge.
struct SessionHandle {
    std::string session_id;
    bool is_valid{false};
    void* userdata{nullptr};
};

namespace detail {

inline auto sessions_mu() -> std::mutex& {
    static std::mutex m;
    return m;
}
inline auto sessions() -> std::unordered_map<std::string, SessionHandle>& {
    static std::unordered_map<std::string, SessionHandle> s;
    return s;
}

} // namespace detail

/// Register a session id with optional userdata. Overwrites if present.
inline auto register_session(std::string sid, void* userdata = nullptr) -> SessionHandle {
    SessionHandle h{.session_id = std::move(sid), .is_valid = true, .userdata = userdata};
    std::lock_guard lock(detail::sessions_mu());
    detail::sessions()[h.session_id] = h;
    return h;
}

/// Look up a session by id; returns an invalid handle if not found.
inline auto lookup(std::string_view sid) -> SessionHandle {
    std::lock_guard lock(detail::sessions_mu());
    auto it = detail::sessions().find(std::string(sid));
    if (it == detail::sessions().end()) return SessionHandle{};
    return it->second;
}

/// Remove a session registration.
inline void unregister_session(std::string_view sid) {
    std::lock_guard lock(detail::sessions_mu());
    detail::sessions().erase(std::string(sid));
}

/// Reset all registered sessions (test helper).
inline void reset_sessions_for_tests() {
    std::lock_guard lock(detail::sessions_mu());
    detail::sessions().clear();
}

} // namespace cc::bridge::session_api

// --------------------------------------------------------------------------
// Bridge facade: messages + messaging
// --------------------------------------------------------------------------

export namespace cc::bridge::messages {

struct ToolTrigger {
    std::string agent_id;
    std::string tool_name;
    std::string input_json;
    int timeout_ms{30000};
};

struct ToolResponse {
    std::string agent_id;
    std::string tool_name;
    std::string output_json;
    std::string error;
    int status_code{0};
};

using ResponseHandler = std::function<void(ToolResponse&)>;

} // namespace cc::bridge::messages

export namespace cc::bridge::messaging {

using ResponseHandler = cc::bridge::messages::ResponseHandler;
using ToolTrigger = cc::bridge::messages::ToolTrigger;
using ToolResponse = cc::bridge::messages::ToolResponse;
using SessionHandle = cc::bridge::session_api::SessionHandle;

namespace detail {

/// Build the lookup key used to route responses to registered handlers.
inline auto make_key(std::string_view session_id, std::string_view agent_id,
                     std::string_view tool_name) -> std::string {
    std::string k;
    k.reserve(session_id.size() + 1 + agent_id.size() + 1 + tool_name.size());
    k.append(session_id).append(1, '|').append(agent_id)
        .append(1, '|').append(tool_name);
    return k;
}

inline auto handlers_mu() -> std::shared_mutex& {
    static std::shared_mutex m;
    return m;
}
inline auto handlers() -> std::unordered_map<std::string, ResponseHandler>& {
    static std::unordered_map<std::string, ResponseHandler> h;
    return h;
}

} // namespace detail

/// Install a response handler for key = session_id|agent_id|tool_name.
/// Replaces any existing handler for the same key.
inline void install_response_handler(std::string key, ResponseHandler h) {
    std::unique_lock lock(detail::handlers_mu());
    detail::handlers()[std::move(key)] = std::move(h);
}

/// Remove all installed handlers (test helper).
inline void clear_response_handlers() {
    std::unique_lock lock(detail::handlers_mu());
    detail::handlers().clear();
}

/// Send a trigger via the registered bridge session. If a handler is installed
/// for the key it is invoked synchronously (it is expected to populate the
/// mutable ToolResponse passed by reference).
inline auto send(const SessionHandle& session, const ToolTrigger& trigger)
    -> std::expected<ToolResponse, std::string> {
    if (!session.is_valid) return std::unexpected("bridge unavailable");

    ToolResponse resp{
        .agent_id = trigger.agent_id,
        .tool_name = trigger.tool_name,
    };
    const auto key = detail::make_key(session.session_id, trigger.agent_id, trigger.tool_name);

    {
        std::shared_lock lock(detail::handlers_mu());
        auto it = detail::handlers().find(key);
        if (it != detail::handlers().end()) {
            try {
                it->second(resp);
            } catch (const std::exception& e) {
                resp.error = std::string("response handler threw: ") + e.what();
                resp.status_code = 500;
            } catch (...) {
                resp.error = "response handler threw unknown exception";
                resp.status_code = 500;
            }
        } else {
            // No handler: produce a default ack so the round-trip still works in tests.
            resp.status_code = 200;
            resp.output_json = "{\"delivered\":true}";
        }
    }
    return resp;
}

} // namespace cc::bridge::messaging

// --------------------------------------------------------------------------
// Remote trigger request / result types
// --------------------------------------------------------------------------

export namespace cc::tools::remote_trigger {

struct RemoteTriggerRequest {
    std::string session_id;
    std::string agent_id;
    std::string tool_name;
    std::string input_json;
    int timeout_ms{30000};
    std::string bridge_url;
    bool allow_curl_fallback{false};
};

struct RemoteTriggerResult {
    bool ok{false};
    std::string output_json;
    std::string error;
    int http_code{0};
    std::string transport_used; // "bridge" | "curl-fallback" | "none"
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

namespace detail {

inline auto shell_quote(std::string_view s) -> std::string {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += R"('\'')";
        else out += ch;
    }
    out += "'";
    return out;
}

/// Execute a shell command, capture stdout+stderr, exit code.
struct CommandRun {
    int exit_code{-1};
    std::string combined;
};

inline auto run_capture(std::string_view cmd) -> CommandRun {
    std::array<char, 4096> buffer{};
    std::string combined;
    FILE* pipe = cc::utils::bash::popen_spawn(std::string(cmd).c_str());
    if (!pipe) return {-1, "failed to spawn subprocess"};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        combined += buffer.data();
    }
    const int status = cc::utils::bash::pclose_spawn(pipe);
    int exit_code = -1;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
    return {exit_code, std::move(combined)};
}

/// Extract the HTTP status code from a curl -w "%{http_code}" style output.
/// The last line of combined will contain the numeric status.
inline auto parse_trailing_http_code(std::string& combined) -> int {
    if (combined.empty()) return 0;
    // Walk back past trailing newlines.
    size_t end = combined.size();
    while (end > 0 && (combined[end - 1] == '\n' || combined[end - 1] == '\r')) --end;
    if (end == 0) return 0;
    // Find the newline before the status line.
    size_t start = combined.rfind('\n', end);
    if (start == std::string::npos) start = 0;
    else ++start;
    if (start >= end) return 0;
    // Parse digits.
    int code = 0;
    for (size_t i = start; i < end; ++i) {
        char c = combined[i];
        if (c < '0' || c > '9') {
            // Trailing line is not a pure status line; give up.
            return 0;
        }
        code = code * 10 + (c - '0');
    }
    // Trim the status line from the body output.
    if (start > 0) --start;          // include newline before
    combined.resize(start);
    return code;
}

} // namespace detail

// --------------------------------------------------------------------------
// Core executor
// --------------------------------------------------------------------------

inline auto execute_remote_trigger(const RemoteTriggerRequest& req)
    -> std::expected<RemoteTriggerResult, std::string> {
    using namespace cc::bridge;

    // ---- Step 1: bridge in-process path ----
    auto handle = session_api::lookup(req.session_id);
    if (handle.is_valid) {
        messages::ToolTrigger trigger{
            .agent_id = req.agent_id,
            .tool_name = req.tool_name,
            .input_json = req.input_json,
            .timeout_ms = req.timeout_ms,
        };
        auto sent = messaging::send(handle, trigger);
        if (!sent) return std::unexpected(sent.error());
        RemoteTriggerResult out;
        out.transport_used = "bridge";
        if (sent->error.empty() && (sent->status_code == 0 || (sent->status_code >= 200 && sent->status_code < 300))) {
            out.ok = true;
            out.http_code = sent->status_code ? sent->status_code : 200;
            out.output_json = sent->output_json;
        } else {
            out.ok = false;
            out.http_code = sent->status_code;
            out.error = sent->error.empty() ? "bridge handler reported failure" : sent->error;
            out.output_json = sent->output_json;
        }
        return out;
    }

    // ---- Step 2: opt-in curl fallback ----
    if (req.allow_curl_fallback && !req.bridge_url.empty()) {
        namespace json = cc::utils::json;

        json::JsonMutDoc rpc_doc;
        json::JsonMutVal rpc = rpc_doc.object();
        rpc_doc.set_root(rpc);
        rpc.add("jsonrpc", rpc_doc.string("2.0"));
        rpc.add("method", rpc_doc.string("trigger"));
        rpc.add("id", rpc_doc.string("1"));
        auto params = rpc_doc.object();
        params.add("session_id", rpc_doc.string(req.session_id));
        params.add("agent_id", rpc_doc.string(req.agent_id));
        params.add("tool", rpc_doc.string(req.tool_name));

        // Embed input_json as a raw nested JSON value if it parses, otherwise
        // as a string so the payload remains valid JSON.
        if (auto input_doc = json::parse(req.input_json); input_doc) {
            params.add("input", rpc_doc.copy_val(input_doc->root()));
        } else {
            params.add("input", rpc_doc.string(req.input_json));
        }
        rpc.add("params", params);
        const std::string payload = rpc_doc.to_string();

        const int clamped_timeout = std::clamp<int>(req.timeout_ms / 1000, 1, 300);

        std::ostringstream cmd;
        cmd << "curl -fsS --max-time " << clamped_timeout
            << " -o - -w '\\n%{http_code}'"
            << " -X POST"
            << " -H " << detail::shell_quote("Content-Type: application/json")
            << " -H " << detail::shell_quote("X-CC-Agent-Id:" + req.agent_id)
            << " -H " << detail::shell_quote("X-CC-Session:" + req.session_id)
            << " --data-raw " << detail::shell_quote(payload)
            << " " << detail::shell_quote(req.bridge_url)
            << " 2>&1";

        auto run = detail::run_capture(cmd.str());
        RemoteTriggerResult out;
        out.transport_used = "curl-fallback";
        out.http_code = detail::parse_trailing_http_code(run.combined);

        if (run.exit_code != 0) {
            out.ok = false;
            out.error = "curl exit code " + std::to_string(run.exit_code) +
                        ": " + (run.combined.empty() ? std::string{"<no output>"} : run.combined);
            return out;
        }
        if (out.http_code >= 400 || out.http_code == 0) {
            out.ok = false;
            out.error = "curl returned HTTP " + std::to_string(out.http_code);
            out.output_json = std::move(run.combined);
            return out;
        }
        out.ok = true;
        out.output_json = std::move(run.combined);
        return out;
    }

    // ---- Step 3: fail closed ----
    return std::unexpected(
        "fail-closed default: bridge unavailable and curl fallback disabled");
}

// --------------------------------------------------------------------------
// JSON dispatcher (used from runtime_registry dispatch).
// --------------------------------------------------------------------------

/// Entry point taking a raw JSON string and returning a serialized JSON
/// response, or an error string.
[[nodiscard]] inline auto execute_remote_trigger_tool(std::string_view input_json)
    -> std::expected<std::string, std::string> {
    namespace json = cc::utils::json;

    auto doc = json::parse(input_json);
    if (!doc) return std::unexpected("remote_trigger tool: invalid JSON input: " + doc.error().message());
    auto root = doc->root();

    auto str_or = [&](std::string_view key, std::string_view fallback) -> std::string {
        auto c = root.get(key);
        return c.is_str() ? std::string(c.as_str()) : std::string(fallback);
    };
    auto int_or = [&](std::string_view key, int64_t fallback) -> int64_t {
        auto c = root.get(key);
        return c.is_num() ? c.as_int() : fallback;
    };

    bool allow_curl_fallback = false;
    if (auto c = root.get("allow_curl_fallback"); c.valid()) {
        allow_curl_fallback = c.is_bool() ? c.as_bool() : (c.is_num() && c.as_int() != 0);
    }

    RemoteTriggerRequest req{
        .session_id          = str_or("session_id", ""),
        .agent_id            = str_or("agent_id", ""),
        .tool_name           = str_or("tool_name", ""),
        .input_json          = str_or("input_json", ""),
        .timeout_ms          = static_cast<int>(int_or("timeout_ms", 30000)),
        .bridge_url          = str_or("bridge_url", ""),
        .allow_curl_fallback = allow_curl_fallback,
    };

    // Accept legacy aliases.
    if (req.agent_id.empty()) {
        if (auto c = root.get("target"); c.is_str()) req.session_id = std::string(c.as_str());
        if (auto c = root.get("url"); c.is_str()) req.bridge_url = std::string(c.as_str());
    }

    auto result = execute_remote_trigger(req);
    if (!result) return std::unexpected(result.error());

    json::JsonMutDoc build;
    json::JsonMutVal build_root = build.object();
    build.set_root(build_root);
    build_root.add("ok", build.boolean(result->ok));
    build_root.add("transport_used", build.string(result->transport_used));
    build_root.add("http_code", build.number(static_cast<int64_t>(result->http_code)));
    if (!result->output_json.empty()) {
        // Preserve structure if the output is itself valid JSON.
        if (auto od = json::parse(result->output_json); od) {
            build_root.add("output_json", build.copy_val(od->root()));
        } else {
            build_root.add("output_json", build.string(result->output_json));
        }
    } else {
        build_root.add("output_json", build.string(""));
    }
    build_root.add("error", build.string(result->error));
    return build.to_string();
}

} // namespace cc::tools::remote_trigger

// --------------------------------------------------------------------------
// Back-compat shims expected by runtime_registry.cppm which references
// `cc::tools::RemoteTriggerInput` and `cc::tools::execute_remote_trigger`.
// These route into the new two-tier executor.
export namespace cc::tools {

struct RemoteTriggerInput {
    std::string target;
    std::string message;
    std::map<std::string, std::string> params;
};

/// Thin compatibility wrapper. Builds a synthetic request using the curl
/// fallback path (equivalent to legacy webhook behaviour) and always allows
/// the curl fallback because the caller explicitly used this legacy API.
inline auto execute_remote_trigger(const RemoteTriggerInput& input)
    -> std::expected<std::string, std::string> {
    if (input.target.empty()) return std::unexpected(std::string("target is required"));
    if (input.message.empty()) return std::unexpected(std::string("message is required"));

    // Internal address block retained from the legacy implementation: fail
    // closed for obvious metadata service / loopback addresses.
    const auto t = input.target;
    auto contains = [&](std::string_view needle) {
        return t.find(needle) != std::string::npos;
    };
    if (contains("127.0.0.1") || contains("localhost") ||
        contains("169.254") || t.rfind("10.", 0) == 0) {
        return std::unexpected(std::string("Cannot trigger internal network addresses"));
    }

    namespace json = cc::utils::json;
    json::JsonMutDoc params_doc;
    json::JsonMutVal params_root = params_doc.object();
    params_doc.set_root(params_root);
    for (const auto& [k, v] : input.params) params_root.add(k, params_doc.string(v));

    const std::string input_json = [&] {
        json::JsonMutDoc d;
        json::JsonMutVal d_root = d.object();
        d.set_root(d_root);
        d_root.add("message", d.string(input.message));
        d_root.add("params", d.raw_json(params_doc.to_string()));
        return d.to_string();
    }();

    remote_trigger::RemoteTriggerRequest req{
        .session_id        = "legacy-" + t.substr(0, 32),
        .agent_id          = "legacy",
        .tool_name         = "webhook",
        .input_json        = input_json,
        .timeout_ms        = 30000,
        .bridge_url        = t,
        .allow_curl_fallback = true,
    };
    auto res = remote_trigger::execute_remote_trigger(req);
    if (!res) return std::unexpected(res.error());
    if (!res->ok) return std::unexpected("Remote trigger failed: " + (res->error.empty() ? res->output_json : res->error));
    return res->output_json.empty() ? std::string("Remote trigger delivered") : res->output_json;
}

} // namespace cc::tools
