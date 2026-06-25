/// @file hooks_execution.cppm
/// @brief Full hook execution engine: condition evaluation, command/http/
///        prompt/agent/function runners, pipeline helpers.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

// POSIX `environ` is not declared by default; declare it explicitly so the
// command-hook runner can propagate the parent process environment.
extern "C" char** environ;

export module cc.utils.hooks_execution;

import cc.utils.json;
import cc.utils.async;
import cc.utils.hooks_registry;
import cc.utils.ssrf_guard;

export namespace cc::utils::hooks_execution {

using namespace cc::utils::hooks_registry;
using cc::utils::async::Task;
using cc::utils::json::JsonVal;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::parse;
using cc::utils::json::to_string;
namespace json_ns = cc::utils::json;

namespace chr = std::chrono;
namespace fs = std::filesystem;

// ==========================================================================
// Core types
// ==========================================================================

/// Context passed through all hook execution paths.
struct HookExecutionContext {
    std::string session_id;
    std::string agent_id;
    std::string user_id;
    std::string conversation_id;
    std::unordered_map<std::string, JsonVal> context_vars;
    std::optional<JsonVal> hook_payload;
    /// Optional delegate used by Function / Agent triggers.
    std::function<std::expected<std::string, std::string>(std::string_view)> tool_runner_delegate;

    /// Owned JSON documents backing context_vars / hook_payload views.
    /// Uses shared_ptr so copying a context copy keeps the documents alive.
    /// A deque is used so push_back does not invalidate JsonVal references.
    std::deque<std::shared_ptr<json_ns::JsonDoc>> owned_docs;

    /// Safely store a context variable by taking ownership of a parsed document.
    /// The document's root is stored in context_vars under `name`.
    /// Returns the root JsonVal (valid for the lifetime of this context
    /// and any copies made from it).
    JsonVal set_context_doc(std::string_view name, json_ns::JsonDoc&& doc) {
        auto ptr = std::make_shared<json_ns::JsonDoc>(std::move(doc));
        owned_docs.push_back(ptr);
        JsonVal root = owned_docs.back()->root();
        context_vars[std::string(name)] = root;
        return root;
    }

    /// Set the hook_payload from a parsed document, taking ownership.
    void set_payload_doc(json_ns::JsonDoc&& doc) {
        auto ptr = std::make_shared<json_ns::JsonDoc>(std::move(doc));
        owned_docs.push_back(ptr);
        hook_payload = owned_docs.back()->root();
    }
};

/// Result of evaluating a hook matcher condition.
struct HookEvaluationResult {
    bool matched{false};
    std::string matched_rule_id;
    std::string error;
};

/// Result of running a single hook command.
struct HookCommandResult {
    bool executed{false};
    int exit_code{0};
    std::string stdout;
    std::string stderr;
    std::string error;
    chr::milliseconds elapsed{0};
};

/// Action returned by a hook to the caller (controls query flow).
struct HookResponseAction {
    enum Action {
        Continue = 0,
        AbortQuery,
        RetryWithModifiedPayload,
        BlockToolCall,
    };
    Action action{Continue};
    std::string reason;
    std::string modified_payload;
    std::vector<std::string> extra_messages;
};

// ==========================================================================
// Small helpers
// ==========================================================================

namespace hooks_detail {

[[nodiscard]] std::string trim(std::string_view s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) ++start;
    auto end = s.end();
    if (start == end) return {};
    --end;
    while (end != start && std::isspace(static_cast<unsigned char>(*end))) --end;
    ++end;
    return std::string(start, end);
}

[[nodiscard]] std::string strip_quotes(std::string_view s) {
    std::string out = trim(s);
    if (out.size() >= 2) {
        const char f = out.front(), b = out.back();
        if ((f == '"' && b == '"') || (f == '\'' && b == '\'')) {
            return out.substr(1, out.size() - 2);
        }
    }
    return out;
}

/// RAII helper that close()s a POSIX fd on scope exit.
struct FdGuard {
    int fd{-1};
    FdGuard() = default;
    explicit FdGuard(int f) : fd(f) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& o) noexcept : fd(o.fd) { o.fd = -1; }
    FdGuard& operator=(FdGuard&& o) noexcept {
        if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; }
        return *this;
    }
    ~FdGuard() { if (fd >= 0) ::close(fd); }
    int release() { int r = fd; fd = -1; return r; }
    [[nodiscard]] bool valid() const { return fd >= 0; }
};

[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '"':  out += R"(\")"; break;
            case '\\': out += R"(\\)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out += ch; break;
        }
    }
    return out;
}

} // namespace hooks_detail
using namespace hooks_detail;

// ==========================================================================
// extract_matcher_field  (dotted-path lookup in context_vars / payload)
// ==========================================================================

[[nodiscard]] std::optional<std::string> extract_matcher_field(
    std::string_view dotted_path,
    const HookExecutionContext& ctx)
{
    if (dotted_path.empty()) return std::nullopt;

    std::vector<std::string> parts;
    {
        std::string cur;
        for (char ch : dotted_path) {
            if (ch == '.') {
                if (!cur.empty()) parts.push_back(std::move(cur));
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) parts.push_back(std::move(cur));
    }
    if (parts.empty()) return std::nullopt;

    auto lookup = [&](JsonVal root) -> std::optional<std::string> {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!root.is_obj()) return std::nullopt;
            root = root.get(parts[i]);
            if (!root.valid() || root.is_null()) return std::nullopt;
        }
        if (root.is_str()) return std::string(root.as_str());
        if (root.is_num()) return std::to_string(root.as_int());
        if (root.is_bool()) return root.as_bool() ? "true" : "false";
        if (root.is_null()) return std::string("null");
        return to_string(root);
    };

    auto top = ctx.context_vars.find(parts[0]);
    if (top != ctx.context_vars.end() && top->second.valid()) {
        JsonVal cur = top->second;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            if (!cur.is_obj()) return std::nullopt;
            cur = cur.get(parts[i]);
            if (!cur.valid() || cur.is_null()) return std::nullopt;
        }
        if (cur.is_str()) return std::string(cur.as_str());
        if (cur.is_num()) return std::to_string(cur.as_int());
        if (cur.is_bool()) return cur.as_bool() ? "true" : "false";
        if (cur.is_null()) return std::string("null");
        return to_string(cur);
    }

    if (ctx.hook_payload && ctx.hook_payload->is_obj()) {
        return lookup(*ctx.hook_payload);
    }
    return std::nullopt;
}

// ==========================================================================
// evaluate_hook_condition (mini boolean DSL)
// ==========================================================================

namespace hooks_detail {

[[nodiscard]] std::expected<bool, std::string> eval_atomic(std::string_view expr,
                                                           const HookExecutionContext& ctx)
{
    std::string_view ex = expr;
    bool negate = false;
    while (!ex.empty() && std::isspace(static_cast<unsigned char>(ex.front()))) ex.remove_prefix(1);
    if (!ex.empty() && ex.front() == '!') {
        negate = true;
        ex.remove_prefix(1);
        while (!ex.empty() && std::isspace(static_cast<unsigned char>(ex.front()))) ex.remove_prefix(1);
    }

    // " in " operator.
    {
        std::size_t i = 0;
        std::size_t in_pos = std::string_view::npos;
        while (i + 4 <= ex.size()) {
            if (ex[i] == ' ' && ex[i+1] == 'i' && ex[i+2] == 'n' && ex[i+3] == ' ') {
                in_pos = i; break;
            }
            ++i;
        }
        if (in_pos != std::string_view::npos) {
            auto field_sv = trim(ex.substr(0, in_pos));
            auto value_sv = trim(ex.substr(in_pos + 4));
            auto field = extract_matcher_field(field_sv, ctx).value_or(std::string{});
            std::string cur;
            for (std::size_t k = 0; k < value_sv.size(); ++k) {
                char ch = value_sv[k];
                if (ch == ',') {
                    if (strip_quotes(cur) == field) { return {!negate}; }
                    cur.clear(); continue;
                }
                cur.push_back(ch);
            }
            bool found = strip_quotes(cur) == field;
            return found != negate ? std::expected<bool,std::string>{true}
                                   : std::expected<bool,std::string>{false};
        }
    }

    // `!=` / `==`
    auto neq = ex.find("!=");
    auto eq  = ex.find("==");
    if (neq != std::string_view::npos && (eq == std::string_view::npos || neq < eq)) {
        auto left  = trim(ex.substr(0, neq));
        auto right = trim(ex.substr(neq + 2));
        auto field = extract_matcher_field(left, ctx).value_or(std::string{});
        bool different = (field != strip_quotes(right));
        return different != negate ? std::expected<bool,std::string>{true}
                                   : std::expected<bool,std::string>{false};
    }
    if (eq != std::string_view::npos) {
        auto left  = trim(ex.substr(0, eq));
        auto right = trim(ex.substr(eq + 2));
        auto field = extract_matcher_field(left, ctx).value_or(std::string{});
        bool same = (field == strip_quotes(right));
        return same != negate ? std::expected<bool,std::string>{true}
                              : std::expected<bool,std::string>{false};
    }

    // Bare identifier / field truthiness.
    auto name = trim(ex);
    if (name.empty()) return {negate};
    auto val = extract_matcher_field(name, ctx);
    bool truthy = val.has_value() && (*val != "false") && (*val != "0")
                  && (*val != "null") && !val->empty();
    return truthy != negate ? std::expected<bool,std::string>{true}
                            : std::expected<bool,std::string>{false};
}

[[nodiscard]] std::expected<bool, std::string> eval_and_chain(std::string_view expr,
                                                              const HookExecutionContext& ctx)
{
    std::vector<std::string_view> atoms;
    std::size_t start = 0;
    for (std::size_t i = 0; i + 2 <= expr.size(); ) {
        if (expr[i] == '&' && expr[i + 1] == '&') {
            atoms.push_back(expr.substr(start, i - start));
            i += 2;
            start = i;
        } else {
            ++i;
        }
    }
    atoms.push_back(expr.substr(start));

    bool result = true;
    for (auto a : atoms) {
        auto r = eval_atomic(a, ctx);
        if (!r) return r;
        result = result && *r;
    }
    return result;
}

} // namespace hooks_detail
using namespace hooks_detail;

[[nodiscard]] std::expected<bool, std::string> evaluate_hook_condition(
    std::string_view condition_expression_string,
    const HookExecutionContext& ctx)
{
    std::string_view expr = condition_expression_string;
    if (trim(expr).empty()) return {true};

    std::vector<std::string_view> or_parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i + 2 <= expr.size(); ) {
        if (expr[i] == '|' && expr[i + 1] == '|') {
            or_parts.push_back(expr.substr(start, i - start));
            i += 2;
            start = i;
        } else {
            ++i;
        }
    }
    or_parts.push_back(expr.substr(start));

    std::string last_error;
    for (auto part : or_parts) {
        auto r = eval_and_chain(part, ctx);
        if (!r) { last_error = r.error(); continue; }
        if (*r) return {true};
    }
    if (!last_error.empty()) return std::unexpected(last_error);
    return {false};
}

// Legacy 3-arg bridge for callers that don't build a full context.
[[nodiscard]] std::expected<bool, std::string> evaluate_hook_condition(
    std::string_view condition,
    std::string_view tool_name,
    std::string_view input_json)
{
    HookExecutionContext ctx;
    if (!tool_name.empty()) {
        std::string json_str = std::string("{\"name\": \"") + json_escape(tool_name) + "\"}";
        auto tool_doc = parse(json_str);
        if (tool_doc) ctx.set_context_doc("tool", std::move(*tool_doc));
    }
    if (!input_json.empty()) {
        auto payload = parse(input_json);
        if (payload) ctx.set_payload_doc(std::move(*payload));
    }
    return evaluate_hook_condition(condition, ctx);
}

// ==========================================================================
// filter_hooks_by_matcher
// ==========================================================================

[[nodiscard]] std::vector<IndividualHookConfig> filter_hooks_by_matcher(
    const std::vector<IndividualHookConfig>& hooks_vector,
    const HookExecutionContext& ctx)
{
    std::vector<IndividualHookConfig> out;
    out.reserve(hooks_vector.size());
    for (const auto& cfg : hooks_vector) {
        if (!cfg.matcher || trim(*cfg.matcher).empty()) {
            out.push_back(cfg);
            continue;
        }
        auto r = evaluate_hook_condition(*cfg.matcher, ctx);
        if (r && *r) out.push_back(cfg);
    }
    return out;
}

// ==========================================================================
// CommandHookRunner::run_raw  (posix_spawn + poll loop)
// ==========================================================================

namespace cmd_detail {

bool drain_fd(int fd, std::string& out, bool& eof_out) {
    char buf[4096];
    eof_out = false;
    bool any = false;
    while (true) {
        auto n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            any = true;
            continue;
        }
        if (n == 0) { eof_out = true; return any; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return any;
        if (errno == EINTR) continue;
        eof_out = true;
        return any;
    }
}

} // namespace cmd_detail

struct CommandHookRunner {
    /// Run a raw command with optional stdin input.
    /// @param cmd Command to execute
    /// @param args Arguments
    /// @param timeout_ms Timeout in milliseconds
    /// @param env_pairs Additional environment variables
    /// @param stdin_input Optional data to write to stdin before reading output
    static HookCommandResult run_raw(
        const std::string& cmd,
        const std::vector<std::string>& args = {},
        int timeout_ms = 30000,
        const std::vector<std::pair<std::string, std::string>>& env_pairs = {},
        std::string_view stdin_input = {})
    {
        using namespace cmd_detail;
        HookCommandResult result;
        auto t0 = chr::steady_clock::now();
        if (timeout_ms < 0) timeout_ms = 30000;

        if (cmd.empty()) {
            result.error = "empty command";
            return result;
        }

        std::vector<std::string> owned_args;
        owned_args.push_back(cmd);
        for (const auto& a : args) owned_args.push_back(a);
        std::vector<char*> argv;
        argv.reserve(owned_args.size() + 1);
        for (auto& s : owned_args) argv.push_back(s.data());
        argv.push_back(nullptr);

        std::vector<std::string> env_store;
        std::vector<char*> envp;
        for (char** p = environ; *p; ++p) env_store.emplace_back(*p);
        for (const auto& [k, v] : env_pairs) env_store.emplace_back(k + "=" + v);
        envp.reserve(env_store.size() + 1);
        for (auto& s : env_store) envp.push_back(s.data());
        envp.push_back(nullptr);

        int stdin_pair[2] = {-1, -1};
        int stdout_pair[2] = {-1, -1};
        int stderr_pair[2] = {-1, -1};
        auto mk = [](int* p) { return ::pipe(p) == 0; };
        if (!mk(stdin_pair) || !mk(stdout_pair) || !mk(stderr_pair)) {
            for (int f : {stdin_pair[0], stdin_pair[1], stdout_pair[0], stdout_pair[1],
                          stderr_pair[0], stderr_pair[1]})
                if (f >= 0) ::close(f);
            result.error = "pipe() failed";
            return result;
        }

        FdGuard parent_stdin_w(stdin_pair[1]);
        FdGuard child_stdin_r(stdin_pair[0]);
        FdGuard parent_stdout_r(stdout_pair[0]);
        FdGuard child_stdout_w(stdout_pair[1]);
        FdGuard parent_stderr_r(stderr_pair[0]);
        FdGuard child_stderr_w(stderr_pair[1]);

        posix_spawn_file_actions_t acts;
        if (posix_spawn_file_actions_init(&acts) != 0) {
            result.error = "posix_spawn_file_actions_init failed";
            return result;
        }
        posix_spawn_file_actions_adddup2(&acts, child_stdin_r.fd, 0);
        posix_spawn_file_actions_adddup2(&acts, child_stdout_w.fd, 1);
        posix_spawn_file_actions_adddup2(&acts, child_stderr_w.fd, 2);
        posix_spawn_file_actions_addclose(&acts, parent_stdin_w.fd);
        posix_spawn_file_actions_addclose(&acts, parent_stdout_r.fd);
        posix_spawn_file_actions_addclose(&acts, parent_stderr_r.fd);
        posix_spawn_file_actions_addclose(&acts, child_stdin_r.fd);
        posix_spawn_file_actions_addclose(&acts, child_stdout_w.fd);
        posix_spawn_file_actions_addclose(&acts, child_stderr_w.fd);

        pid_t pid = -1;
        int spawn_rc = posix_spawnp(&pid, cmd.c_str(), &acts, nullptr,
                                    argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&acts);
        // Close child-side pipe ends in the parent. The child process
        // already received its own copies via posix_spawn file_actions;
        // failing to close our copies keeps the write ends of stdout/stderr
        // open, so poll() never sees POLLHUP and the command always waits
        // for the full timeout.
        child_stdin_r = FdGuard{};
        child_stdout_w = FdGuard{};
        child_stderr_w = FdGuard{};

        if (spawn_rc != 0) {
            result.error = "posix_spawnp failed: " + std::string(strerror(spawn_rc));
            return result;
        }

        // Write stdin input to child, then close stdin.
        if (!stdin_input.empty()) {
            std::size_t written = 0;
            const char* data = stdin_input.data();
            std::size_t remaining = stdin_input.size();
            while (remaining > 0) {
                ssize_t n = ::write(parent_stdin_w.fd, data + written, remaining);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;  // EPIPE or other error — child may have exited early
                }
                written += static_cast<std::size_t>(n);
                remaining -= static_cast<std::size_t>(n);
            }
            // Write newline to match TS behavior (jsonInput + '\n')
            if (remaining == 0) {
                ::write(parent_stdin_w.fd, "\n", 1);
            }
        }
        parent_stdin_w = FdGuard{};

        auto set_nonblock = [](int fd) -> bool {
            int fl = ::fcntl(fd, F_GETFL, 0);
            if (fl < 0) return false;
            return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
        };
        set_nonblock(parent_stdout_r.fd);
        set_nonblock(parent_stderr_r.fd);

        const auto deadline = t0 + chr::milliseconds(timeout_ms);
        bool timed_out = false;
        bool stdout_eof = false, stderr_eof = false;

        while (!stdout_eof || !stderr_eof) {
            auto now = chr::steady_clock::now();
            if (now >= deadline) { timed_out = true; break; }
            auto remaining = chr::duration_cast<chr::milliseconds>(deadline - now);
            int ms = std::max(1, static_cast<int>(remaining.count()));

            pollfd pfds[2] = {};
            nfds_t nfds = 0;
            if (!stdout_eof) { pfds[nfds].fd = parent_stdout_r.fd; pfds[nfds].events = POLLIN; ++nfds; }
            if (!stderr_eof) { pfds[nfds].fd = parent_stderr_r.fd; pfds[nfds].events = POLLIN; ++nfds; }
            if (nfds == 0) break;

            int pr = ::poll(pfds, nfds, ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                result.error = "poll failed: " + std::string(strerror(errno));
                break;
            }
            if (pr == 0) { timed_out = true; break; }

            nfds_t idx = 0;
            if (!stdout_eof) {
                if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                    bool eof;
                    drain_fd(pfds[idx].fd, result.stdout, eof);
                    if (pfds[idx].revents & (POLLHUP | POLLERR) || eof) stdout_eof = true;
                }
                ++idx;
            }
            if (!stderr_eof) {
                if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                    bool eof;
                    drain_fd(pfds[idx].fd, result.stderr, eof);
                    if (pfds[idx].revents & (POLLHUP | POLLERR) || eof) stderr_eof = true;
                }
            }
        }

        if (timed_out) {
            ::kill(pid, SIGTERM);
            const auto term_deadline = chr::steady_clock::now() + chr::milliseconds(2000);
            int status = 0;
            bool reaped = false;
            while (chr::steady_clock::now() < term_deadline) {
                auto w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid) { reaped = true; break; }
                if (w < 0 && errno != EINTR) break;
                ::usleep(20000);
            }
            if (!reaped) {
                ::kill(pid, SIGKILL);
                for (int loops = 0; loops < 50; ++loops) {
                    int s2;
                    auto w = ::waitpid(pid, &s2, WNOHANG);
                    if (w == pid) { status = s2; break; }
                    ::usleep(20000);
                }
            }
            result.exit_code = WIFSIGNALED(status) ? -WTERMSIG(status)
                               : WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (result.error.empty()) result.error = "command timed out";
        } else {
            bool dummy;
            drain_fd(parent_stdout_r.fd, result.stdout, dummy);
            drain_fd(parent_stderr_r.fd, result.stderr, dummy);
            int status = 0;
            for (int loops = 0; loops < 500; ++loops) {
                auto w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid) break;
                if (w < 0 && errno != EINTR) break;
                ::usleep(2000);
            }
            if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) result.exit_code = -WTERMSIG(status);
        }

        result.executed = true;
        auto t1 = chr::steady_clock::now();
        result.elapsed = chr::duration_cast<chr::milliseconds>(t1 - t0);
        return result;
    }
};

// ==========================================================================
// URL parsing + HTTP hook runner
// ==========================================================================

namespace hooks_detail {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    bool valid{false};
};

[[nodiscard]] ParsedUrl parse_url(std::string_view url) {
    ParsedUrl out;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return out;
    out.scheme = std::string(url.substr(0, scheme_end));
    auto rest = url.substr(scheme_end + 3);
    auto slash = rest.find('/');
    auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    auto at = authority.find('@');
    if (at != std::string_view::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return out;
        out.host = std::string(authority.substr(1, close - 1));
        auto after = authority.substr(close + 1);
        if (after.size() > 1 && after[0] == ':') out.port = std::string(after.substr(1));
    } else {
        auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            out.host = std::string(authority.substr(0, colon));
            out.port = std::string(authority.substr(colon + 1));
        } else {
            out.host = std::string(authority);
        }
    }
    out.valid = !out.host.empty() && (out.scheme == "http" || out.scheme == "https");
    return out;
}

inline const std::vector<std::string_view> kDefaultHttpAllowlist = {
    "https://api.anthropic.com",
    "https://hooks.slack.com",
};

[[nodiscard]] bool url_prefix_allowed(std::string_view url,
                                      const std::vector<std::string>& custom_prefixes = {})
{
    for (auto p : kDefaultHttpAllowlist) {
        if (url.size() >= p.size() &&
            std::string_view(url.data(), p.size()) == p)
            return true;
    }
    for (const auto& p : custom_prefixes) {
        if (url.size() >= p.size() && url.compare(0, p.size(), p) == 0)
            return true;
    }
    return custom_prefixes.empty();
}

} // namespace hooks_detail
using namespace hooks_detail;

struct HttpHookRunner {
    static HookCommandResult run(
        const std::string& url,
        const std::string& method = "POST",
        const std::vector<std::pair<std::string, std::string>>& headers = {},
        const std::string& body = "",
        const std::vector<std::string>& allowlist_prefixes = {})
    {
        HookCommandResult result;
        auto t0 = chr::steady_clock::now();

        if (url.empty()) { result.error = "empty HTTP url"; return result; }

        auto parsed = parse_url(url);
        if (!parsed.valid) { result.error = "invalid HTTP url"; return result; }

        using cc::utils::ssrf_guard::is_blocked_address;
        if (!parsed.host.empty() && is_blocked_address(parsed.host)) {
            result.error = "SSRF blocked: host resolves to private/reserved address";
            return result;
        }
        if (!url_prefix_allowed(url, allowlist_prefixes)) {
            result.error = "URL not in HTTP allowlist prefixes";
            return result;
        }

        std::string cmethod = method.empty() ? "POST" : method;
        std::vector<std::string> args;
        args.push_back("-fsSL");
        args.push_back("-X"); args.push_back(cmethod);
        args.push_back("-H"); args.push_back("Content-Type: application/json");
        for (const auto& [k, v] : headers) {
            args.push_back("-H");
            args.push_back(k + ": " + v);
        }
        args.push_back("--max-time"); args.push_back("30");
        args.push_back("-w"); args.push_back("\n%{http_code}");
        if (!body.empty()) {
            args.push_back("--data-raw");
            args.push_back(body);
        }
        args.push_back(url);

        auto r = CommandHookRunner::run_raw("curl", args, 35000, {});
        result = r;
        if (result.executed && !result.stdout.empty()) {
            auto nl = result.stdout.find_last_of('\n');
            if (nl != std::string::npos) {
                std::string code = trim(result.stdout.substr(nl + 1));
                result.stdout = result.stdout.substr(0, nl);
                result.stderr += "[http_status=" + code + "]";
                if (!code.empty()) {
                    int v = 0;
                    auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), v);
                    if (ec == std::errc{} && ptr == code.data() + code.size() && (v < 200 || v >= 300)) {
                        if (result.exit_code == 0) result.exit_code = 1;
                    }
                }
            }
        }
        if (result.exit_code != 0 && result.error.empty()) {
            result.error = "HTTP request failed (exit_code=" + std::to_string(result.exit_code) + ")";
        }
        if (result.elapsed.count() == 0) {
            auto t1 = chr::steady_clock::now();
            result.elapsed = chr::duration_cast<chr::milliseconds>(t1 - t0);
        }
        return result;
    }
};

// ==========================================================================
// PromptHookRunner::run  (template expansion + token cap)
// ==========================================================================

struct PromptHookRunner {
    static constexpr std::size_t CHARS_PER_TOKEN = 4;

    static std::string run(std::string_view template_string,
                           const HookExecutionContext& ctx,
                           std::size_t cap_tokens = 4000)
    {
        static const std::regex pattern(
            R"(\$\{([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\})");
        std::string input(template_string);
        std::string output;
        output.reserve(input.size());

        std::sregex_iterator it(input.begin(), input.end(), pattern);
        std::sregex_iterator end;
        std::size_t last_pos = 0;

        for (; it != end; ++it) {
            const auto& m = *it;
            output.append(input, last_pos,
                          static_cast<std::size_t>(m.position()) - last_pos);
            std::string dotted = m[1].str();
            auto val = extract_matcher_field(dotted, ctx);
            if (val) output += *val;
            else output += m.str();
            last_pos = static_cast<std::size_t>(m.position()) + m.length();
        }
        output.append(input, last_pos, std::string::npos);

        const std::size_t cap_chars = cap_tokens * CHARS_PER_TOKEN;
        if (output.size() > cap_chars) {
            output.resize(cap_chars);
            output += "\n\n[truncated]";
        }
        return output;
    }
};

// ==========================================================================
// AgentHookRunner::run  (sub-agent query via delegate)
// ==========================================================================

struct AgentHookRunner {
    static HookCommandResult run(std::string_view subprompt,
                                 const HookExecutionContext& ctx,
                                 int budget_tokens = 1024)
    {
        HookCommandResult result;
        auto t0 = chr::steady_clock::now();

        // Nested-loop guard (prevents recursive agent hooks).
        std::string lower;
        lower.reserve(subprompt.size());
        for (char ch : subprompt)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        if (lower.find("execagenthook") != std::string::npos ||
            lower.find("run_agent_hook") != std::string::npos) {
            result.error = "nested agent hooks are forbidden";
            result.exit_code = 1;
            return result;
        }

        if (!ctx.tool_runner_delegate) {
            result.error = "no tool_runner_delegate available";
            result.exit_code = 1;
            return result;
        }

        std::string ctx_dump;
        for (const auto& [k, v] : ctx.context_vars) {
            ctx_dump += "  " + k + ": " + to_string(v) + "\n";
        }

        std::string wrapped =
            "You are running a hook agent. Goal:\n\n"
            + std::string(subprompt)
            + "\n\nContext variables:\n" + ctx_dump
            + "\nToken budget: " + std::to_string(budget_tokens)
            + "\nRespond with the final result only.";

        auto r = ctx.tool_runner_delegate(wrapped);
        auto t1 = chr::steady_clock::now();
        result.elapsed = chr::duration_cast<chr::milliseconds>(t1 - t0);
        if (!r) {
            result.executed = true;
            result.exit_code = 1;
            result.stderr = r.error();
            return result;
        }
        result.executed = true;
        result.exit_code = 0;
        result.stdout = std::move(*r);
        return result;
    }
};

// ==========================================================================
// execute_hook (universal dispatcher)
// ==========================================================================

[[nodiscard]] HookResponseAction parse_structured_action(std::string_view body) {
    HookResponseAction out{HookResponseAction::Continue, {}, {}, {}};
    if (body.empty()) return out;
    auto doc = parse(body);
    if (!doc || !doc->root().is_obj()) return out;
    auto r = doc->root();
    if (auto act = r.get("action"); act.is_str()) {
        std::string a(act.as_str());
        if (a == "block") out.action = HookResponseAction::BlockToolCall;
        else if (a == "abort") out.action = HookResponseAction::AbortQuery;
        else if (a == "modify" || a == "retry") out.action = HookResponseAction::RetryWithModifiedPayload;
        else out.action = HookResponseAction::Continue;
    }
    if (auto rs = r.get("reason"); rs.is_str()) out.reason = std::string(rs.as_str());
    if (auto mp = r.get("modified_payload"); mp.is_str())
        out.modified_payload = std::string(mp.as_str());
    if (auto em = r.get("extra_messages"); em.is_arr()) {
        em.iter([&](JsonVal item) {
            if (item.is_str()) out.extra_messages.emplace_back(item.as_str());
        });
    }
    return out;
}

// We accept `HookConfig` — hooks_registry uses IndividualHookConfig which
// has a `config` field of type HookCommand. We keep the param for future
// expansion (e.g. http.allowlist_prefixes, timeouts etc.).
struct HookPolicyConfig {
    std::vector<std::string> http_allowlist_prefixes;
    std::optional<int> default_timeout_seconds;
    std::string on_error_action;
};

using HookConfig = HookPolicyConfig;

[[nodiscard]] std::pair<HookCommandResult, HookResponseAction> execute_hook(
    const HookCommand& cmd,
    const HookConfig& cfg,
    const HookExecutionContext& ctx)
{
    HookResponseAction action{HookResponseAction::Continue, {}, {}, {}};
    HookCommandResult result;

    switch (get_command_type(cmd)) {
        case HookCommandType::Command: {
            const auto& c = std::get<CommandHookConfig>(cmd);
            int timeout_ms = c.timeout_seconds
                                 ? (*c.timeout_seconds) * 1000
                                 : (cfg.default_timeout_seconds ? *cfg.default_timeout_seconds * 1000 : 120000);
            std::string shell = c.shell.empty() ? std::string("bash") : c.shell;
            result = CommandHookRunner::run_raw(shell, {"-c", c.command}, timeout_ms, {});
            if (result.exit_code != 0) {
                if (!cfg.on_error_action.empty()) {
                    std::string a;
                    a.reserve(cfg.on_error_action.size());
                    for (char ch : cfg.on_error_action)
                        a.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                    if (a == "block") action.action = HookResponseAction::BlockToolCall;
                    else if (a == "retry") action.action = HookResponseAction::RetryWithModifiedPayload;
                    else if (a == "continue") action.action = HookResponseAction::Continue;
                    else action.action = (result.exit_code == 2)
                                             ? HookResponseAction::BlockToolCall
                                             : HookResponseAction::AbortQuery;
                } else {
                    action.action = (result.exit_code == 2)
                                        ? HookResponseAction::BlockToolCall
                                        : HookResponseAction::AbortQuery;
                }
                action.reason = result.error.empty()
                                    ? ("command exited with " + std::to_string(result.exit_code))
                                    : result.error;
            }
            break;
        }
        case HookCommandType::Http: {
            const auto& h = std::get<HttpHookConfig>(cmd);
            std::string body;
            if (ctx.hook_payload && ctx.hook_payload->valid()) body = to_string(*ctx.hook_payload);
            result = HttpHookRunner::run(h.url, "POST", {}, body, cfg.http_allowlist_prefixes);
            if (!result.stdout.empty()) {
                action = parse_structured_action(result.stdout);
            }
            if (result.exit_code != 0 && action.action == HookResponseAction::Continue) {
                action.action = HookResponseAction::BlockToolCall;
                action.reason = result.error.empty() ? "HTTP hook failed" : result.error;
            }
            break;
        }
        case HookCommandType::Prompt: {
            const auto& p = std::get<PromptHookConfig>(cmd);
            std::size_t cap = p.timeout_seconds
                                  ? static_cast<std::size_t>(*p.timeout_seconds * 100)
                                  : 4000;
            std::string expanded = PromptHookRunner::run(p.prompt, ctx, cap);
            result.executed = true;
            result.exit_code = 0;
            result.stdout = std::move(expanded);
            action.action = HookResponseAction::Continue;
            break;
        }
        case HookCommandType::Agent: {
            const auto& a = std::get<AgentHookConfig>(cmd);
            int budget = a.timeout_seconds ? (*a.timeout_seconds) * 50 : 1024;
            result = AgentHookRunner::run(a.prompt, ctx, budget);
            if (result.exit_code != 0) {
                action.action = HookResponseAction::AbortQuery;
                action.reason = result.error.empty() ? "agent hook failed" : result.error;
            }
            break;
        }
        case HookCommandType::Function: {
            const auto& f = std::get<FunctionHookConfig>(cmd);
            if (ctx.tool_runner_delegate) {
                auto r = ctx.tool_runner_delegate(
                    f.status_message.value_or(std::string{}));
                result.executed = true;
                if (r) { result.exit_code = 0; result.stdout = std::move(*r); }
                else  { result.exit_code = 1; result.stderr = r.error(); }
            } else {
                result.executed = true;
                result.exit_code = 0;
                result.stdout = f.status_message.value_or(std::string{});
            }
            if (result.exit_code != 0) {
                action.action = HookResponseAction::AbortQuery;
                action.reason = result.stderr;
            }
            break;
        }
        default:
            result.error = "unknown hook type";
            action.action = HookResponseAction::AbortQuery;
            action.reason = "unknown hook type";
            break;
    }

    return {std::move(result), std::move(action)};
}

// ==========================================================================
// Pipeline APIs
// ==========================================================================

[[nodiscard]] std::pair<std::string, HookResponseAction> run_api_query_hooks(
    const std::vector<IndividualHookConfig>& registry,
    HookEventType event_type,
    const HookExecutionContext& ctx,
    std::string_view payload)
{
    HookResponseAction action{HookResponseAction::Continue, {}, {}, {}};
    std::string accumulated_prompt;

    std::vector<IndividualHookConfig> matching;
    for (const auto& h : registry) if (h.event == event_type) matching.push_back(h);
    auto filtered = filter_hooks_by_matcher(matching, ctx);

    HookConfig dummy_cfg{};

    auto rank = [](HookResponseAction::Action a) -> int {
        switch (a) {
            case HookResponseAction::BlockToolCall: return 4;
            case HookResponseAction::AbortQuery:    return 3;
            case HookResponseAction::RetryWithModifiedPayload: return 2;
            case HookResponseAction::Continue:     return 1;
        }
        return 0;
    };

    for (const auto& h : filtered) {
        HookExecutionContext lctx = ctx;
        if (h.plugin_name) {
            std::string j = std::string("{\"id\": \"") + json_escape(*h.plugin_name) + "\"}";
            auto id_doc = parse(j);
            if (id_doc) lctx.set_context_doc("hook", std::move(*id_doc));
        }
        auto [res, act] = execute_hook(h.config, dummy_cfg, lctx);

        if (get_command_type(h.config) == HookCommandType::Prompt && !res.stdout.empty()) {
            if (!accumulated_prompt.empty()) accumulated_prompt += "\n\n";
            accumulated_prompt += res.stdout;
        }

        if (rank(act.action) > rank(action.action)) action = act;
    }

    std::string modified_payload(payload);
    if (!accumulated_prompt.empty()) {
        modified_payload =
            "=== Hook prompt additions ===\n" +
            accumulated_prompt +
            "\n=== End hook prompt additions ===\n\n" +
            std::string(payload);
        if (!action.modified_payload.empty()) {
            modified_payload += "\n--- hook modified_payload ---\n" + action.modified_payload;
        }
        action.modified_payload = modified_payload;
    } else if (!action.modified_payload.empty()) {
        modified_payload = action.modified_payload;
    }

    return {std::move(modified_payload), std::move(action)};
}

[[nodiscard]] HookResponseAction execute_post_tool_hooks(
    const std::vector<IndividualHookConfig>& registry,
    const HookExecutionContext& ctx,
    std::string_view tool_name,
    std::string_view input,
    std::string_view output)
{
    HookResponseAction action{HookResponseAction::Continue, {}, {}, {}};
    auto esc = [](std::string_view s) -> std::string {
        std::string o; o.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  o += R"(\")"; break;
                case '\\': o += R"(\\)"; break;
                case '\n': o += R"(\n)"; break;
                case '\r': o += R"(\r)"; break;
                case '\t': o += R"(\t)"; break;
                default:   o += c;
            }
        }
        return o;
    };
    std::string combined =
        std::string("{\"tool\": \"") + esc(tool_name) +
        "\", \"input\": \"" + esc(input) +
        "\", \"output\": \"" + esc(output) + "\"}";

    HookExecutionContext lctx = ctx;
    auto payload_doc = parse(combined);
    if (payload_doc) lctx.set_payload_doc(std::move(*payload_doc));

    std::vector<IndividualHookConfig> matching;
    for (const auto& h : registry)
        if (h.event == HookEventType::PostToolUse) matching.push_back(h);
    auto filtered = filter_hooks_by_matcher(matching, lctx);
    HookConfig dummy{};

    auto rank = [](HookResponseAction::Action a) -> int {
        switch (a) {
            case HookResponseAction::BlockToolCall: return 4;
            case HookResponseAction::AbortQuery:    return 3;
            case HookResponseAction::RetryWithModifiedPayload: return 2;
            case HookResponseAction::Continue:     return 1;
        }
        return 0;
    };
    for (const auto& h : filtered) {
        auto [res, act] = execute_hook(h.config, dummy, lctx);
        (void)res;
        if (rank(act.action) > rank(action.action)) action = act;
    }
    return action;
}

[[nodiscard]] std::vector<HookCommandResult> execute_stop_hooks(
    const std::vector<IndividualHookConfig>& registry,
    const HookExecutionContext& ctx,
    std::string_view stop_condition_json)
{
    std::vector<HookCommandResult> out;
    HookExecutionContext lctx = ctx;
    if (!stop_condition_json.empty()) {
        auto d = parse(stop_condition_json);
        if (d) lctx.set_payload_doc(std::move(*d));
    }

    std::vector<IndividualHookConfig> matching;
    for (const auto& h : registry)
        if (h.event == HookEventType::Stop) matching.push_back(h);
    auto filtered = filter_hooks_by_matcher(matching, lctx);
    HookConfig dummy{};

    for (const auto& h : filtered) {
        auto [res, _] = execute_hook(h.config, dummy, lctx);
        out.push_back(std::move(res));
    }
    return out;
}

} // namespace cc::utils::hooks_execution
