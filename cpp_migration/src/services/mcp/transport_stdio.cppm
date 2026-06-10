/// @file transport_stdio.cppm
/// @brief Phase 3-D: MCP stdio transport — real subprocess + JSON-RPC line parser
///
/// Standalone C++23 named module.  Wraps a child process (fork/exec) with two
/// pipes for bidirectional JSON-RPC 2.0 messaging.  The child process's
/// stdout is parsed line-by-line (one JSON document per line, NDJSON style)
/// using yyjson, and each parsed line is dispatched as a `JsonRpcMessage`
/// through the user-supplied `on_incoming_message` callback.
///
/// Capabilities:
///   * POSIX fork()+pipe2()+execve — no shell interpretation
///   * Optional extra environment variables passed to the child
///   * Optional working directory
///   * Optional watchdog: if no bytes are received within `timeout_ms` the
///     child is killed and `on_exit` fires with a descriptive exit code
///   * Stop() drains both pipes and reaps the child — no zombies
///
/// This module is intentionally independent of the larger `cc.services.mcp`
/// infrastructure so it can be unit-tested with a trivial mock server
/// (e.g. `/bin/sh -c 'while IFS= read -r line; do echo "$line"; done'`)
/// without bringing in any other C++ modules.
module;

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <map>

#include <yyjson.h>

export module cc.services.mcp.stdio;

export namespace cc::services::mcp::stdio {

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Configuration used to launch an MCP stdio subprocess.
struct StdioServerSpec {
    /// Absolute or PATH-resolved binary to execute (e.g. "/usr/bin/python3").
    std::string command;
    /// Arguments *without* argv[0]; the command itself is prepended.
    std::vector<std::string> args;
    /// Extra environment variables that will be set (overriding any inherited
    /// variable of the same name).  The rest of the environment is inherited.
    std::vector<std::pair<std::string, std::string>> env;
    /// If set, the child process chdir()'s here before exec.
    std::optional<std::string> working_dir;
    /// If set (>0) and no bytes arrive from the child's stdout within this
    /// many milliseconds, the watchdog terminates the child with SIGKILL and
    /// fires `on_exit` with exit_code = -ETIMEDOUT.
    std::optional<int> timeout_ms;
};

/// Parsed JSON-RPC 2.0 message.  The payload is intentionally kept simple:
///   * JSON bodies are stored as raw strings (avoids deep yyjson conversion)
///   * A variant captures the scalar result types returned by MCP servers
///     (strings, numbers, booleans).  Complex `result` objects are carried
///     as the raw JSON string in `result_json`.
struct JsonRpcMessage {
    /// Request id.  -1 means "no id" (i.e. a notification or malformed msg).
    std::int64_t                    id = -1;
    /// Request method name.  Empty for plain responses/errors.
    std::string                     method;
    /// `params` key serialized back to JSON.  Empty if the message had no
    /// params key or if it was null.
    std::string                     params_json;
    /// Scalar result (filled for int64/double/bool/string results).
    std::variant<std::monostate, std::string, std::int64_t, double, bool> result;
    /// Raw JSON of `result` when it's a complex object/array.  Falls back to
    /// the same value as the scalar when scalar is set.
    std::string                     result_json;
    /// Populated when the incoming message has an `error` object.
    std::optional<std::string>      error_message;
    std::optional<int>              error_code;
    /// True if parsing succeeded and the message looks like valid JSON-RPC.
    bool                            is_valid = false;
};

/// Stdio-based bidirectional transport.
///
/// Typical lifecycle:
///   ```
///   StdioTransport t;
///   t.on_incoming_message = [](JsonRpcMessage m) { ... };
///   t.on_exit = [](int code, string err) { ... };
///   bool ok = t.Start(spec);
///   t.SendJsonRpc(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
///   t.Stop();
///   ```
///
/// Thread safety: Start() and Stop() must be called from the owning thread.
/// SendJsonRpc() is internally synchronised so it is safe to call from any
/// thread (including from within on_incoming_message / on_exit).
class StdioTransport {
public:
    StdioTransport() = default;

    // Non-copyable (we own fds and a thread).
    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    /// Move semantics.  Not generally useful but makes the class Regular-ish.
    StdioTransport(StdioTransport&& o) noexcept { MoveFrom(std::move(o)); }
    StdioTransport& operator=(StdioTransport&& o) noexcept {
        if (this != &o) { Stop(); MoveFrom(std::move(o)); }
        return *this;
    }

    /// Calls Stop() to ensure no lingering subprocess or threads.
    ~StdioTransport() { Stop(); }

    // ---- User-configurable callbacks --------------------------------------

    /// Fired exactly once for every newline-delimited message received from
    /// the child process's stdout.  Invoked on the reader thread.
    std::function<void(JsonRpcMessage)> on_incoming_message;

    /// Fired exactly once when the reader thread terminates, whether because
    /// the child exited, the watchdog tripped, or Stop() was called.
    /// Invoked on the reader thread before it joins.
    ///   * exit_code: child's WEXITSTATUS() if exited normally,
    ///                -SIGNUM if killed by signal,
    ///                -ETIMEDOUT if watchdog terminated it,
    ///                -1 if waitpid() itself failed.
    ///   * error: empty on clean shutdown, otherwise a diagnostic string.
    std::function<void(int exit_code, std::string error)> on_exit;

    // ---- Lifecycle --------------------------------------------------------

    /// Fork+exec the child described by `spec`.  Returns true on success.
    /// If false, `on_exit` is NOT fired — the caller should check errno or
    /// use a wrapper that logs.  Calling Start() twice without an
    /// intervening Stop() is a no-op that returns false.
    [[nodiscard]] bool Start(const StdioServerSpec& spec);

    /// Atomically write `json` followed by a single '\n' to the child's
    /// stdin.  Returns true if all bytes were written.  Safe to call from
    /// any thread (including from inside on_incoming_message).
    [[nodiscard]] bool SendJsonRpc(std::string_view json);

    /// Gracefully shut down the transport.  Sends SIGTERM, waits 100ms,
    /// then SIGKILL if still alive.  Closes all fds, joins the reader
    /// thread, and reaps the child.  Idempotent.
    void Stop();

    /// Returns true after Start() succeeded and before Stop() has begun.
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Access the pid.  Only meaningful between Start() success and Stop().
    [[nodiscard]] pid_t ChildPid() const noexcept { return child_pid_; }

private:
    // ---- Helpers ----------------------------------------------------------

    void MoveFrom(StdioTransport&& o) noexcept {
        write_fd_ = o.write_fd_;     o.write_fd_ = -1;
        read_fd_  = o.read_fd_;      o.read_fd_  = -1;
        child_pid_ = o.child_pid_;   o.child_pid_ = -1;
        running_.store(o.running_.load());
        o.running_.store(false);
        reader_thread_ = std::move(o.reader_thread_);
        watchdog_thread_ = std::move(o.watchdog_thread_);
        on_incoming_message = std::move(o.on_incoming_message);
        on_exit = std::move(o.on_exit);
    }

    void ReaderLoop(const StdioServerSpec& spec);
    void WatchdogLoop(int timeout_ms);

    // ---- State ------------------------------------------------------------

    int  write_fd_  = -1;   // parent → child stdin
    int  read_fd_   = -1;   // child stdout → parent
    pid_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread     reader_thread_;
    std::thread     watchdog_thread_;
    std::mutex      write_mutex_;
    /// Last activity timestamp (monotonic ms) updated by ReaderLoop.
    /// Written by reader thread; read by watchdog thread.
    std::atomic<std::uint64_t> last_rx_ms_{0};
};

// ---------------------------------------------------------------------------
// Implementation: Start
// ---------------------------------------------------------------------------

inline bool StdioTransport::Start(const StdioServerSpec& spec) {
    if (running_.load()) return false;
    if (spec.command.empty()) return false;

    // Two pairs of pipes: [0]=read, [1]=write (from parent's POV).
    int child_stdin[2]{-1, -1};   // parent writes [1] → child reads [0]
    int child_stdout[2]{-1, -1};  // child writes [1] → parent reads [0]

    auto fail = [&]() {
        for (int* fdp : {&child_stdin[0], &child_stdin[1], &child_stdout[0], &child_stdout[1]})
            if (*fdp >= 0) { ::close(*fdp); *fdp = -1; }
        return false;
    };

    if (::pipe(child_stdin)  != 0) return fail();
    if (::pipe(child_stdout) != 0) return fail();

#if defined(__linux__) && defined(O_CLOEXEC)
    // Best-effort cloexec — not strictly required because we close unused
    // ends in the child anyway before exec, but keeps things tidy.
    ::fcntl(child_stdin[0],  F_SETFD, FD_CLOEXEC);
    ::fcntl(child_stdin[1],  F_SETFD, FD_CLOEXEC);
    ::fcntl(child_stdout[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(child_stdout[1], F_SETFD, FD_CLOEXEC);
#endif

    const pid_t pid = ::fork();
    if (pid < 0) return fail();

    if (pid == 0) {
        // ─── Child process ─────────────────────────────────────────────────
        // Wire stdin to the read end of child_stdin.
        if (::dup2(child_stdin[0], STDIN_FILENO)  < 0) ::_exit(126);
        // Wire stdout to the write end of child_stdout.
        if (::dup2(child_stdout[1], STDOUT_FILENO) < 0) ::_exit(126);

        // Close everything we don't need.
        ::close(child_stdin[0]);
        ::close(child_stdin[1]);
        ::close(child_stdout[0]);
        ::close(child_stdout[1]);

        // Apply environment overrides on top of the inherited environment.
        for (const auto& [k, v] : spec.env) {
            ::setenv(k.c_str(), v.c_str(), 1);
        }

        // Optional chdir.
        if (spec.working_dir) {
            if (::chdir(spec.working_dir->c_str()) != 0) {
                // If chdir fails, write an error to our new stderr (which
                // is still inherited from the parent) and bail.
                const char* msg = "StdioTransport: chdir failed\n";
                ::write(STDERR_FILENO, msg, std::strlen(msg));
                ::_exit(126);
            }
        }

        // Build argv.
        std::vector<char*> argv;
        argv.reserve(spec.args.size() + 2);
        argv.push_back(const_cast<char*>(spec.command.c_str()));
        for (const auto& a : spec.args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        ::execvp(spec.command.c_str(), argv.data());

        // Exec failed — write a one-line diagnostic to stderr before dying.
        char buf[256];
        int n = std::snprintf(buf, sizeof(buf),
                              "StdioTransport: execvp(%s) failed: %s\n",
                              spec.command.c_str(), std::strerror(errno));
        ::write(STDERR_FILENO, buf, static_cast<std::size_t>(n));
        ::_exit(127);
    }

    // ─── Parent process ──────────────────────────────────────────────────
    ::close(child_stdin[0]);   // parent will not read from the child's stdin
    ::close(child_stdout[1]); // parent will not write to the child's stdout

    write_fd_  = child_stdin[1];
    read_fd_   = child_stdout[0];
    child_pid_ = pid;
    running_.store(true, std::memory_order_release);

    // Reset activity timestamp.
    last_rx_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count(), std::memory_order_release);

    // Kick off the reader thread.
    reader_thread_ = std::thread([this, spec_copy = spec] { ReaderLoop(spec_copy); });

    // Kick off the watchdog thread if a timeout was specified.
    if (spec.timeout_ms && *spec.timeout_ms > 0) {
        watchdog_thread_ = std::thread([this, t = *spec.timeout_ms] { WatchdogLoop(t); });
    }

    return true;
}

// ---------------------------------------------------------------------------
// Implementation: SendJsonRpc
// ---------------------------------------------------------------------------

inline bool StdioTransport::SendJsonRpc(std::string_view json) {
    if (!running_.load(std::memory_order_acquire)) return false;
    if (write_fd_ < 0) return false;

    // Build the complete payload (json + '\n') on the stack when possible.
    std::string payload;
    payload.reserve(json.size() + 1);
    payload.append(json.data(), json.size());
    payload.push_back('\n');

    std::lock_guard<std::mutex> lock(write_mutex_);
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto n = ::write(write_fd_, payload.data() + written, payload.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Implementation: Stop
// ---------------------------------------------------------------------------

inline void StdioTransport::Stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);

    // Close the write pipe first — cleanly signals EOF to the child's stdin,
    // which for line-based servers (cat, sh read-loops, etc.) triggers exit.
    if (write_fd_ >= 0) {
        ::close(write_fd_);
        write_fd_ = -1;
    }

    // Give a polite SIGTERM window, then SIGKILL.
    if (child_pid_ > 0) {
        int status = 0;
        // Non-blocking reap attempt.
        const pid_t w = ::waitpid(child_pid_, &status, WNOHANG);
        if (w == 0) {
            ::kill(child_pid_, SIGTERM);
            // Busy-poll up to 100ms (small total, rare path).
            for (int i = 0; i < 10; ++i) {
                if (::waitpid(child_pid_, &status, WNOHANG) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // If still alive, force-kill.
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }

    // Close read fd (wakes any blocking read() in the reader thread).
    if (read_fd_ >= 0) {
        ::close(read_fd_);
        read_fd_ = -1;
    }

    // Join the reader / watchdog threads if we actually had started them.
    if (reader_thread_.joinable()) reader_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();

    (void)was_running;
}

// ---------------------------------------------------------------------------
// Implementation: ReaderLoop (runs on reader_thread_)
// ---------------------------------------------------------------------------

inline void StdioTransport::ReaderLoop(const StdioServerSpec& /*spec*/) {
    std::string line_buf;
    line_buf.reserve(1024);
    std::array<char, 8192> scratch{};
    int   exit_code = 0;
    std::string error_reason;

    const auto update_ts = [this] {
        last_rx_ms_.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count(), std::memory_order_release);
    };

    update_ts();

    auto dispatch_line = [&](std::string_view line) {
        // Strip trailing '\r' if present (some servers emit CRLF).
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return;   // blank lines are silent
        if (line.front() == ':') return;   // SSE-style comment lines

        JsonRpcMessage msg;
        // Parse with yyjson.
        yyjson_read_err err{};
        auto* doc = yyjson_read_opts(const_cast<char*>(line.data()), line.size(),
                                     0, nullptr, &err);
        if (!doc) {
            msg.is_valid = false;
            msg.error_message = std::string("yyjson parse failed: ") + (err.msg ? err.msg : "unknown");
            if (on_incoming_message) on_incoming_message(std::move(msg));
            return;
        }
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            msg.is_valid = false;
            msg.error_message = "JSON root is not an object";
            yyjson_doc_free(doc);
            if (on_incoming_message) on_incoming_message(std::move(msg));
            return;
        }
        msg.is_valid = true;

        // id
        if (yyjson_val* v = yyjson_obj_get(root, "id")) {
            if (yyjson_is_int(v)) msg.id = yyjson_get_int(v);
            else if (yyjson_is_sint(v)) msg.id = yyjson_get_sint(v);
            else if (yyjson_is_uint(v)) msg.id = static_cast<std::int64_t>(yyjson_get_uint(v));
            // string ids are ignored for simplicity (the dispatcher uses ints).
        }

        // method
        if (yyjson_val* m = yyjson_obj_get(root, "method")) {
            if (yyjson_is_str(m)) {
                msg.method.assign(yyjson_get_str(m), static_cast<std::size_t>(yyjson_get_len(m)));
            }
        }

        // params (serialized back to JSON string)
        if (yyjson_val* p = yyjson_obj_get(root, "params")) {
            size_t sz = 0;
            char*  s = yyjson_val_write(p, 0, &sz);
            if (s) {
                msg.params_json.assign(s, sz);
                free(s);
            }
        }

        // error
        if (yyjson_val* e = yyjson_obj_get(root, "error"); yyjson_is_obj(e)) {
            if (yyjson_val* code = yyjson_obj_get(e, "code"); yyjson_is_int(code) || yyjson_is_sint(code)) {
                msg.error_code = static_cast<int>(yyjson_get_sint(code));
            } else if (yyjson_is_uint(code)) {
                msg.error_code = static_cast<int>(yyjson_get_uint(code));
            }
            if (yyjson_val* mes = yyjson_obj_get(e, "message"); yyjson_is_str(mes)) {
                msg.error_message.emplace(yyjson_get_str(mes), static_cast<std::size_t>(yyjson_get_len(mes)));
            }
        }

        // result
        if (yyjson_val* r = yyjson_obj_get(root, "result")) {
            // Always write the raw JSON representation.
            size_t sz = 0;
            char*  s = yyjson_val_write(r, 0, &sz);
            if (s) {
                msg.result_json.assign(s, sz);
                free(s);
            }
            // Attempt scalar storage.
            if (yyjson_is_str(r)) {
                msg.result = std::string(yyjson_get_str(r),
                                         static_cast<std::size_t>(yyjson_get_len(r)));
            } else if (yyjson_is_int(r) || yyjson_is_sint(r)) {
                msg.result = static_cast<std::int64_t>(yyjson_get_sint(r));
            } else if (yyjson_is_uint(r)) {
                msg.result = static_cast<std::int64_t>(yyjson_get_uint(r));
            } else if (yyjson_is_real(r)) {
                msg.result = yyjson_get_real(r);
            } else if (yyjson_is_bool(r)) {
                msg.result = yyjson_get_bool(r);
            }
            // else: monostate — complex result, caller inspects result_json
        }

        yyjson_doc_free(doc);
        if (on_incoming_message) on_incoming_message(std::move(msg));
    };

    // Keep reading until the read fd closes or Stop() is requested.
    for (;;) {
        if (!running_.load(std::memory_order_acquire) && read_fd_ < 0) break;
        if (read_fd_ < 0) break;

        ssize_t n = ::read(read_fd_, scratch.data(), scratch.size());
        if (n > 0) {
            update_ts();
            // Split on '\n', carrying leftovers into line_buf.
            std::size_t start = 0;
            for (ssize_t i = 0; i < n; ++i) {
                if (scratch[static_cast<std::size_t>(i)] == '\n') {
                    const std::size_t chunk = static_cast<std::size_t>(i) - start;
                    if (!line_buf.empty()) {
                        line_buf.append(scratch.data() + start, chunk);
                        dispatch_line(line_buf);
                        line_buf.clear();
                    } else {
                        dispatch_line(std::string_view(scratch.data() + start, chunk));
                    }
                    start = static_cast<std::size_t>(i) + 1;
                }
            }
            if (start < static_cast<std::size_t>(n)) {
                line_buf.append(scratch.data() + start,
                                static_cast<std::size_t>(n) - start);
            }
            continue;
        }
        if (n == 0) {
            // EOF — child closed stdout.
            break;
        }
        // n < 0
        if (errno == EINTR) {
            if (!running_.load()) break;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        // Real error.
        error_reason = std::string("read() failed: ") + std::strerror(errno);
        break;
    }

    // Flush any partial line remaining in the buffer (defensive; JSON-RPC
    // lines are always newline-terminated).
    if (!line_buf.empty()) dispatch_line(line_buf);

    // Reap child to determine exit code.
    if (child_pid_ > 0) {
        int status = 0;
        // If we already reaped in Stop(), waitpid returns ECHILD — harmless.
        const pid_t w = ::waitpid(child_pid_, &status, 0);
        if (w > 0) {
            if (WIFEXITED(status))      exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exit_code = -WTERMSIG(status);
            else                        exit_code = -1;
            child_pid_ = -1;
        } else if (errno == ECHILD) {
            // Already reaped by Stop() — that's fine.
            exit_code = 0;
        } else {
            exit_code = -1;
            if (error_reason.empty())
                error_reason = std::string("waitpid() failed: ") + std::strerror(errno);
        }
    }

    if (on_exit) on_exit(exit_code, error_reason);
}

// ---------------------------------------------------------------------------
// Implementation: WatchdogLoop
// ---------------------------------------------------------------------------

inline void StdioTransport::WatchdogLoop(int timeout_ms) {
    using namespace std::chrono;
    const auto slice = std::max(10, timeout_ms / 4);

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(milliseconds(slice));
        if (!running_.load()) break;

        const auto now = duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        const auto last = last_rx_ms_.load(std::memory_order_acquire);
        if (now - last > static_cast<std::uint64_t>(timeout_ms)) {
            if (child_pid_ > 0 && running_.load()) {
                ::kill(child_pid_, SIGKILL);
                // Wait briefly to ensure the child is gone.  ReaderLoop will
                // unblock when read_fd_ hits EOF, then fire on_exit.
                int status = 0;
                ::waitpid(child_pid_, &status, 0);
                // Force-stop to surface a single clear error.
                running_.store(false, std::memory_order_release);
                if (on_exit) {
                    const int code = -ETIMEDOUT;
                    on_exit(code, "watchdog: no output for " +
                                  std::to_string(timeout_ms) + "ms");
                }
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers (small free functions used by tests and higher layers)
// ---------------------------------------------------------------------------

/// Build an initialize-request JSON string suitable for SendJsonRpc().
inline std::string BuildInitializeRequest(std::int64_t id,
                                          std::string_view client_name,
                                          std::string_view client_version) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    auto add_str = [&](const char* key, std::string_view val) {
        yyjson_mut_obj_add(root,
            yyjson_mut_strncpy(doc, key, std::strlen(key)),
            yyjson_mut_strncpy(doc, val.data(), val.size()));
    };

    add_str("jsonrpc", "2.0");
    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "%lld", static_cast<long long>(id));
    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "id", 2),
        yyjson_mut_sint(doc, id));

    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "method", 6),
        yyjson_mut_strncpy(doc, "initialize", 10));

    // params
    yyjson_mut_val* params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add(params,
        yyjson_mut_strncpy(doc, "protocolVersion", 15),
        yyjson_mut_strncpy(doc, "2024-11-05", 10));
    yyjson_mut_val* ci = yyjson_mut_obj(doc);
    yyjson_mut_obj_add(ci,
        yyjson_mut_strncpy(doc, "name", 4),
        yyjson_mut_strncpy(doc, client_name.data(), client_name.size()));
    yyjson_mut_obj_add(ci,
        yyjson_mut_strncpy(doc, "version", 7),
        yyjson_mut_strncpy(doc, client_version.data(), client_version.size()));
    yyjson_mut_obj_add(params,
        yyjson_mut_strncpy(doc, "clientInfo", 10), ci);
    yyjson_mut_val* caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add(caps,
        yyjson_mut_strncpy(doc, "roots", 5), yyjson_mut_bool(doc, true));
    yyjson_mut_obj_add(params,
        yyjson_mut_strncpy(doc, "capabilities", 12), caps);

    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "params", 6), params);

    size_t out_len = 0;
    char* out = yyjson_mut_write(doc, 0, &out_len);
    std::string result(out ? out : "", out ? out_len : 0);
    if (out) ::free(out);
    yyjson_mut_doc_free(doc);
    return result;
}

/// Build a tools/list request JSON string.
inline std::string BuildToolsListRequest(std::int64_t id) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "jsonrpc", 7),
        yyjson_mut_strncpy(doc, "2.0", 3));
    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "id", 2),
        yyjson_mut_sint(doc, id));
    yyjson_mut_obj_add(root,
        yyjson_mut_strncpy(doc, "method", 6),
        yyjson_mut_strncpy(doc, "tools/list", 10));

    size_t out_len = 0;
    char* out = yyjson_mut_write(doc, 0, &out_len);
    std::string result(out ? out : "", out ? out_len : 0);
    if (out) ::free(out);
    yyjson_mut_doc_free(doc);
    return result;
}

/// Convenience: make a StdioServerSpec that runs a tiny /bin/sh script which
/// echoes every received line back to stdout.  Used as a mock server by
/// unit tests to verify Start → SendJsonRpc → on_incoming_message end to end.
inline StdioServerSpec MockEchoSpec() {
    StdioServerSpec s;
    s.command = "/bin/sh";
    s.args = {
        "-c",
        "while IFS= read -r line; do printf '%s\\n' \"$line\"; done"
    };
    return s;
}

} // namespace cc::services::mcp::stdio
