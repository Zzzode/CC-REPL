/// @file repl_tool.cppm
/// @brief Persistent REPL sessions via posix_spawn with bidirectional pipes.
///
/// Replaces the previous popen-based "one-shot" REPL helper with a true
/// persistent session manager. Interpreters (python3 / node / bun / ruby) are
/// kept alive as child processes and state is preserved across evaluations.
///
/// Architecture:
///   * ReplSession            -- a single live interpreter process (pid + pipes)
///   * ReplSessionManager     -- singleton owning a map<id, unique_ptr<ReplSession>>
///   * execute_repl_tool(json) -- JSON dispatcher for actions: create/eval/close/history/list
///
/// Sentinel protocol: after user-supplied code we write a language-specific
/// print of a magic string and poll until that string appears on stdout or
/// timeout fires. This reliably detects snippet completion, even for
/// multi-line / interactive inputs.
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <random>
#include <regex>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <spawn.h>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#define CC_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define CC_ENVIRON environ
#endif

export module cc.tools.repl;

import cc.utils.json;

export namespace cc::tools::repl {

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

inline constexpr std::string_view kSentinel = "__REPL_SENTINEL__9f8e7d__";
inline constexpr size_t kReadChunk = 8192;
inline constexpr size_t kMaxOutputPerEval = 4 * 1024 * 1024;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

namespace detail {

inline auto strip_ansi(std::string s) -> std::string {
    static const std::regex ansi(R"(\x1b\[[0-9;]*[mK])");
    return std::regex_replace(s, ansi, "");
}

inline void safe_close(int& fd) {
    if (fd >= 0) {
        int rc;
        do { rc = ::close(fd); } while (rc == -1 && errno == EINTR);
        fd = -1;
    }
}

inline auto set_nonblocking(int fd) -> bool {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline auto random_hex(size_t len = 8) -> std::string {
    static thread_local std::mt19937 rng{
        static_cast<std::mt19937::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count() ^
            (static_cast<uint64_t>(::getpid()) << 16))};
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) out += kHex[rng() & 0xf];
    return out;
}

inline auto write_all(int fd, std::string_view data) -> bool {
    size_t remaining = data.size();
    const char* p = data.data();
    while (remaining > 0) {
        ssize_t n;
        do { n = ::write(fd, p, remaining); } while (n == -1 && errno == EINTR);
        if (n == -1) return false;
        remaining -= static_cast<size_t>(n);
        p += n;
    }
    return true;
}

inline auto read_drain(int fd, std::string& out) -> bool {
    std::array<char, kReadChunk> buf{};
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n == 0) return false;
        if (n > 0) {
            out.append(buf.data(), static_cast<size_t>(n));
            if (out.size() > kMaxOutputPerEval) {
                out.resize(kMaxOutputPerEval);
                out += "\n[output truncated]\n";
                return true;
            }
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

} // namespace detail

// --------------------------------------------------------------------------
// ReplSession
// --------------------------------------------------------------------------

struct ReplSession {
    std::string id;
    std::string language;   // "python3" | "node" | "bun" | "ruby"
    ::pid_t pid{-1};
    int stdin_fd{-1};       // parent writes here
    int stdout_fd{-1};        // parent reads here
    int stderr_fd{-1};       // parent reads here
    std::regex prompt_regex{R"()"};
    std::vector<std::string> history;
    std::chrono::steady_clock::time_point last_used;
    bool alive{false};
    std::mutex mu;

    ReplSession() = default;
    ReplSession(ReplSession&&) = delete;
    ReplSession(const ReplSession&) = delete;
    ReplSession& operator=(ReplSession&&) = delete;
    ReplSession& operator=(const ReplSession&) = delete;

    ~ReplSession() { terminate_sync(); }

    void terminate_sync() {
        using namespace std::chrono;
        detail::safe_close(stdin_fd);
        if (pid <= 0) {
            detail::safe_close(stdout_fd);
            detail::safe_close(stderr_fd);
            alive = false;
            return;
        }
        ::kill(pid, SIGTERM);
        const auto deadline = steady_clock::now() + seconds(2);
        int status = 0;
        while (steady_clock::now() < deadline) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w > 0) break;
            std::this_thread::sleep_for(milliseconds(20));
        }
        if (::waitpid(pid, &status, WNOHANG) == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        pid = -1;
        alive = false;
        detail::safe_close(stdout_fd);
        detail::safe_close(stderr_fd);
    }

    auto check_alive() -> bool {
        if (!alive || pid <= 0) return false;
        int status = 0;
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w > 0) { alive = false; pid = -1; return false; }
        return true;
    }

    auto sentinel_line() const -> std::string {
        if (language == "python3") return "print(\"__REPL_SENTINEL__9f8e7d__\")\n";
        if (language == "node" || language == "bun") return ";console.log(\"__REPL_SENTINEL__9f8e7d__\")\n";
        if (language == "ruby") return "puts \"__REPL_SENTINEL__9f8e7d__\"\n";
        return std::string("echo ") + std::string(kSentinel) + "\n";
    }
};

// --------------------------------------------------------------------------
// ReplSessionManager (singleton)
// --------------------------------------------------------------------------

class ReplSessionManager {
public:
    static auto instance() -> ReplSessionManager& {
        static ReplSessionManager mgr;
        return mgr;
    }

    ~ReplSessionManager() { shutdown_all(); }

    auto create(
        const std::string& language,
        const std::optional<std::string>& requested_id = std::nullopt
    ) -> std::expected<std::string, std::string> {
        std::vector<std::string> argv_vec;
        std::regex prompt_re;
        if (language == "python3") {
            argv_vec = {"python3", "-i", "-q"};
            prompt_re = std::regex(R"(>>> |\.\.\. )");
        } else if (language == "node") {
            argv_vec = {"node", "--interactive", "--no-warnings"};
            prompt_re = std::regex(R"(> )");
        } else if (language == "bun") {
            argv_vec = {"bun", "repl"};
            prompt_re = std::regex(R"(> )");
        } else if (language == "ruby") {
            argv_vec = {"irb", "--simple-prompt"};
            prompt_re = std::regex(R"(irb(\(\S+\))?:\d+:\d+[>*] )");
        } else {
            return std::unexpected("unsupported REPL language: " + language);
        }

        std::string id = requested_id.value_or(
            "repl_" + language + "_" + detail::random_hex());

        std::unique_lock map_lock(map_mu_);
        if (sessions_.contains(id)) {
            return std::unexpected("session id already exists: " + id);
        }

        int stdin_pipe[2]{-1, -1};
        int stdout_pipe[2]{-1, -1};
        int stderr_pipe[2]{-1, -1};
        if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
            detail::safe_close(stdin_pipe[0]); detail::safe_close(stdin_pipe[1]);
            detail::safe_close(stdout_pipe[0]); detail::safe_close(stdout_pipe[1]);
            detail::safe_close(stderr_pipe[0]); detail::safe_close(stderr_pipe[1]);
            return std::unexpected(std::string("failed to create pipes for REPL: ") + std::strerror(errno));
        }

        posix_spawn_file_actions_t fa;
        if (::posix_spawn_file_actions_init(&fa) != 0) {
            detail::safe_close(stdin_pipe[0]); detail::safe_close(stdin_pipe[1]);
            detail::safe_close(stdout_pipe[0]); detail::safe_close(stdout_pipe[1]);
            detail::safe_close(stderr_pipe[0]); detail::safe_close(stderr_pipe[1]);
            return std::unexpected("posix_spawn_file_actions_init failed");
        }
        ::posix_spawn_file_actions_adddup2(&fa, stdin_pipe[0], STDIN_FILENO);
        ::posix_spawn_file_actions_addclose(&fa, stdin_pipe[0]);
        ::posix_spawn_file_actions_addclose(&fa, stdin_pipe[1]);
        ::posix_spawn_file_actions_adddup2(&fa, stdout_pipe[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&fa, stdout_pipe[0]);
        ::posix_spawn_file_actions_addclose(&fa, stdout_pipe[1]);
        ::posix_spawn_file_actions_adddup2(&fa, stderr_pipe[1], STDERR_FILENO);
        ::posix_spawn_file_actions_addclose(&fa, stderr_pipe[0]);
        ::posix_spawn_file_actions_addclose(&fa, stderr_pipe[1]);

        std::vector<char*> argv_c;
        argv_c.reserve(argv_vec.size() + 1);
        for (auto& s : argv_vec) argv_c.push_back(s.data());
        argv_c.push_back(nullptr);

        pid_t pid = 0;
        int rc = ::posix_spawnp(&pid, argv_vec[0].c_str(), &fa, nullptr,
                                argv_c.data(), CC_ENVIRON);
        ::posix_spawn_file_actions_destroy(&fa);

        detail::safe_close(stdin_pipe[0]);
        detail::safe_close(stdout_pipe[1]);
        detail::safe_close(stderr_pipe[1]);

        if (rc != 0) {
            detail::safe_close(stdin_pipe[1]);
            detail::safe_close(stdout_pipe[0]);
            detail::safe_close(stderr_pipe[0]);
            return std::unexpected("failed to spawn REPL interpreter '" + argv_vec[0] +
                "': " + std::strerror(rc));
        }

        (void)detail::set_nonblocking(stdout_pipe[0]);
        (void)detail::set_nonblocking(stderr_pipe[0]);

        auto session = std::make_unique<ReplSession>();
        session->id = id;
        session->language = language;
        session->pid = pid;
        session->stdin_fd = stdin_pipe[1];
        session->stdout_fd = stdout_pipe[0];
        session->stderr_fd = stderr_pipe[0];
        session->prompt_regex = std::move(prompt_re);
        session->last_used = std::chrono::steady_clock::now();
        session->alive = true;

        sessions_.emplace(id, std::move(session));
        map_lock.unlock();

        // Drain the initial banner / prompt noise with a short timeout so later
        // evaluations don't see interpreter version strings.
        using namespace std::chrono;
        const auto quiet_deadline = steady_clock::now() + milliseconds(500);
        std::string discard_out, discard_err;
        std::array<struct ::pollfd, 2> pfds{};
        pfds[0].fd = stdout_pipe[0];
        pfds[0].events = POLLIN;
        pfds[1].fd = stderr_pipe[0];
        pfds[1].events = POLLIN;
        while (steady_clock::now() < quiet_deadline) {
            const auto wait = duration_cast<milliseconds>(quiet_deadline - steady_clock::now()).count();
            int timeout_ms = static_cast<int>(std::clamp<long long>(wait, 0, 50));
            int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
            if (pr <= 0) break;
            if (pfds[0].revents & POLLIN) (void)detail::read_drain(stdout_pipe[0], discard_out);
            if (pfds[1].revents & POLLIN) (void)detail::read_drain(stderr_pipe[0], discard_err);
        }
        return id;
    }

    auto eval(
        const std::string& session_id,
        const std::string& code,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)
    ) -> std::expected<std::pair<std::string, std::string>, std::string> {
        using namespace std::chrono;

        std::shared_lock map_lock(map_mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return std::unexpected("session not found: " + session_id);
        auto& session = *it->second;
        map_lock.unlock();

        std::lock_guard sess_lock(session.mu);
        if (!session.check_alive()) {
            return std::unexpected("REPL session has exited unexpectedly");
        }

        std::string payload = code;
        if (!payload.empty() && payload.back() != '\n') payload += '\n';
        payload += session.sentinel_line();

        if (!detail::write_all(session.stdin_fd, payload)) {
            session.alive = false;
            return std::unexpected("failed to write code to REPL stdin");
        }

        std::string out, err;
        const auto deadline = steady_clock::now() + timeout;
        bool sentinel_seen = false;
        bool io_error = false;

        std::array<struct ::pollfd, 2> pfds{};
        pfds[0].fd = session.stdout_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = session.stderr_fd;
        pfds[1].events = POLLIN;

        while (!sentinel_seen) {
            const auto now = steady_clock::now();
            if (now >= deadline) {
                return std::unexpected("REPL eval timed out after " +
                    std::to_string(duration_cast<milliseconds>(timeout).count()) + "ms");
            }
            const auto wait_ms = duration_cast<milliseconds>(deadline - now).count();
            const int timeout_ms = static_cast<int>(std::clamp<long long>(wait_ms, 0, 500));
            int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
            if (pr == 0) continue;
            if (pr == -1) {
                if (errno == EINTR) continue;
                return std::unexpected(std::string("poll error: ") + std::strerror(errno));
            }
            if (pfds[0].revents & (POLLIN | POLLHUP)) {
                if (!detail::read_drain(session.stdout_fd, out)) io_error = true;
                if (out.find(kSentinel) != std::string::npos) sentinel_seen = true;
            }
            if (pfds[1].revents & (POLLIN | POLLHUP)) {
                if (!detail::read_drain(session.stderr_fd, err)) io_error = true;
            }
            if ((pfds[0].revents & POLLHUP) && (pfds[1].revents & POLLHUP)) break;
            if (io_error) return std::unexpected("I/O error reading from REPL process");
        }

        if (sentinel_seen) {
            auto pos = out.find(kSentinel);
            if (pos != std::string::npos) {
                while (pos > 0 && (out[pos - 1] == '\n' || out[pos - 1] == '\r')) --pos;
                out.resize(pos);
            }
        }
        out = std::regex_replace(out, session.prompt_regex, "");
        err = std::regex_replace(err, session.prompt_regex, "");
        out = detail::strip_ansi(std::move(out));
        err = detail::strip_ansi(std::move(err));

        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();

        session.history.push_back(code);
        session.last_used = steady_clock::now();
        return std::make_pair(std::move(out), std::move(err));
    }

    void close(const std::string& session_id) {
        std::unique_ptr<ReplSession> doomed;
        {
            std::unique_lock lock(map_mu_);
            auto it = sessions_.find(session_id);
            if (it == sessions_.end()) return;
            doomed = std::move(it->second);
            sessions_.erase(it);
        }
    }

    auto history(const std::string& session_id, size_t limit = SIZE_MAX) -> std::vector<std::string> {
        std::shared_lock map_lock(map_mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return {};
        auto& h = it->second->history;
        if (limit >= h.size()) return h;
        return {h.end() - static_cast<std::ptrdiff_t>(limit), h.end()};
    }

    auto list_alive() -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> out;
        std::shared_lock lock(map_mu_);
        out.reserve(sessions_.size());
        for (const auto& [k, s] : sessions_) {
            if (s->alive) out.emplace_back(k, s->language);
        }
        return out;
    }

    void shutdown_all() {
        std::unordered_map<std::string, std::unique_ptr<ReplSession>> doomed;
        {
            std::unique_lock lock(map_mu_);
            doomed = std::move(sessions_);
            sessions_.clear();
        }
    }

private:
    ReplSessionManager() = default;
    mutable std::shared_mutex map_mu_;
    std::unordered_map<std::string, std::unique_ptr<ReplSession>> sessions_;
};

// --------------------------------------------------------------------------
// Public convenience wrappers
// --------------------------------------------------------------------------

inline auto create_session(const std::string& language,
                       const std::optional<std::string>& requested = std::nullopt)
    -> std::expected<std::string, std::string> {
    return ReplSessionManager::instance().create(language, requested);
}

inline auto eval_session(const std::string& id, const std::string& code,
                        std::chrono::milliseconds timeout = std::chrono::seconds(30))
    -> std::expected<std::pair<std::string, std::string>, std::string> {
    return ReplSessionManager::instance().eval(id, code, timeout);
}

inline void close_session(const std::string& id) {
    ReplSessionManager::instance().close(id);
}

inline auto history_session(const std::string& id, size_t limit = SIZE_MAX)
    -> std::vector<std::string> {
    return ReplSessionManager::instance().history(id, limit);
}

inline auto list_sessions() -> std::vector<std::pair<std::string, std::string>> {
    return ReplSessionManager::instance().list_alive();
}

// --------------------------------------------------------------------------
// JSON dispatcher
// --------------------------------------------------------------------------

[[nodiscard]] inline auto execute_repl_tool(std::string_view input_json)
    -> std::expected<std::string, std::string> {
    namespace json = cc::utils::json;

    auto doc = json::parse(input_json);
    if (!doc) return std::unexpected("REPL tool: invalid JSON input: " + doc.error().message());
    auto root = doc->root();

    auto str_or = [&](std::string_view key, std::string_view fallback) -> std::string {
        auto c = root.get(key);
        return c.is_str() ? std::string(c.as_str()) : std::string(fallback);
    };
    auto int_or = [&](std::string_view key, int64_t fallback) -> int64_t {
        auto c = root.get(key);
        return c.is_num() ? c.as_int() : fallback;
    };

    const std::string action        = str_or("action", "eval");
    const std::string session_id    = str_or("session_id", "");
    const std::string language      = str_or("language", "python3");
    const std::string code          = str_or("code", "");
    const int64_t     history_limit = int_or("history_limit", 50);
    const int64_t     timeout_ms    = int_or("timeout_ms", 30000);

    json::JsonMutDoc build;
    json::JsonMutVal build_root = build.object();
    build.set_root(build_root);

    auto ok = [&](auto&& setter) {
        setter(build_root);
        build_root.add("ok", build.boolean(true));
        return build.to_string();
    };

    if (action == "create") {
        auto id = create_session(language, session_id.empty()
                                     ? std::nullopt
                                     : std::optional<std::string>{session_id});
        if (!id) return std::unexpected(id.error());
        return ok([&](auto& r) {
            r.add("session_id", build.string(*id));
            r.add("language", build.string(language));
        });
    }

    if (action == "list") {
        auto sessions = list_sessions();
        auto arr = build.array();
        for (const auto& [sid, lang] : sessions) {
            auto entry = build.object();
            entry.add("session_id", build.string(sid));
            entry.add("language", build.string(lang));
            arr.append(entry);
        }
        build_root.add("sessions", arr);
        build_root.add("ok", build.boolean(true));
        return build.to_string();
    }

    if (action == "close") {
        if (session_id.empty()) return std::unexpected("REPL close requires session_id");
        close_session(session_id);
        return ok([](auto&) {});
    }

    if (action == "history") {
        if (session_id.empty()) return std::unexpected("REPL history requires session_id");
        auto entries = history_session(session_id,
            history_limit > 0 ? static_cast<size_t>(history_limit) : SIZE_MAX);
        auto arr = build.array();
        for (const auto& entry : entries) arr.append(build.string(entry));
        build_root.add("history", arr);
        build_root.add("ok", build.boolean(true));
        return build.to_string();
    }

    if (action == "eval") {
        if (session_id.empty()) return std::unexpected("REPL eval requires session_id");
        auto timeout = std::chrono::milliseconds{
            std::clamp<int64_t>(timeout_ms, 10, 10 * 60 * 1000)};
        auto result = eval_session(session_id, code, timeout);
        if (!result) return std::unexpected(result.error());
        auto [out, err] = *std::move(result);
        return ok([&](auto& r) {
            r.add("stdout", build.string(out));
            r.add("stderr", build.string(err));
        });
    }

    return std::unexpected("REPL tool: unknown action: " + action);
}

} // namespace cc::tools::repl
