/// @file impl_bash.cppm
/// @brief Real child-process execution used by the Bash tool in Phase 3.
///
/// Implementation notes:
///   * Uses pipe() + fork() + execve("/bin/sh", {"sh","-c",cmd})
///   * Parent side uses poll() on the pipe set so streaming + timeout work on
///     Darwin/Linux without external event loops.
///   * Timeout handler: a watchdog thread waits on pid with a deadline and
///     sends SIGTERM then SIGKILL if the child is still alive.
///   * Sandboxing: defaults to SoftEnvvars (sets __LD_UNIQUE / SANDBOX=1);
///     MacOSSandboxExec wraps the command via sandbox-exec.  P0 simply
///     constructs the profile; Phase 3+ hardens the allowlist.
module;

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include <functional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

// POSIX
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <signal.h>

// Helper that copies libc's `environ` strings without exposing `environ` as
// a name inside the module purview (which would get module-qualified linkage).
// Both the declaration and definition live in the GMF as extern "C".
extern "C" {
    extern char** environ;
    auto copy_libc_environ(std::vector<std::string>& out) -> void {
        if (::environ) {
            for (char** p = ::environ; *p != nullptr; ++p) out.emplace_back(*p);
        }
    }
}

export module cc.tools.bash.impl;

export namespace cc::tools::bash::impl {

// --------------------------------------------------------------------------
// Types
// --------------------------------------------------------------------------

enum class SandboxMode {
    None,
    SoftEnvvars,      // default: only toggle env vars to hint sandboxing
    MacOSSandboxExec, // run under `sandbox-exec -p <profile>`
};

struct BashOptions {
    std::string command;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    std::optional<int>    timeout_sec = 30;
    bool                  allow_fail = false;
    bool                  combine_streams = true;
    bool                  sandboxed = false;
    SandboxMode           sandbox_mode = SandboxMode::SoftEnvvars;
    std::function<void(int fd, std::string_view chunk)> on_output_chunk;
};

struct BashOutput {
    int         exit_code = 0;
    std::string combined;
    std::string stdout_only;
    std::string stderr_only;
    std::string error_reason;
    bool        timed_out = false;
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

namespace detail {

inline std::string shell_quote(std::string_view s) {
    // Bourne-compatible single-quote escape: wrap body in '..', replace each
    // embedded ' with '\''.
    std::string out;
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out.push_back('\'');
            out.push_back('\\');
            out.push_back('\'');
            out.push_back('\'');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

inline auto scoped_fd(int fd) {
    auto closer = [](int* p) { if (p && *p >= 0) { ::close(*p); *p = -1; } };
    return std::unique_ptr<int, decltype(closer)>{new int{fd}, std::move(closer)};
}

struct pipe_pair {
    int read_fd  = -1;
    int write_fd = -1;
    auto create() -> bool {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return false;
        read_fd  = fds[0];
        write_fd = fds[1];
        return true;
    }
    void close_both() {
        if (read_fd  >= 0) { ::close(read_fd);  read_fd  = -1; }
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
    }
};

// Build a POSIX envp from the C runtime environ (via copy_libc_environ)
// with opts.env appended / overriding.
inline auto build_envp(const std::vector<std::pair<std::string,std::string>>& extra,
                       const SandboxMode mode)
    -> std::vector<std::string> {
    std::vector<std::string> base;
    copy_libc_environ(base);
    // Append overrides.
    for (auto& kv : extra) {
        std::string e = kv.first;
        e.push_back('=');
        e.append(kv.second);
        base.push_back(std::move(e));
    }
    // 3. Sandbox-mode markers.
    if (mode == SandboxMode::SoftEnvvars) {
        base.push_back("__LD_UNIQUE=1");
        base.push_back("SANDBOX=1");
    }
    return base;
}

inline void nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace detail

// --------------------------------------------------------------------------
// Sandboxing
// --------------------------------------------------------------------------

/// Build the `sandbox-exec` invocation wrapping the given user command.
/// The profile is deliberately conservative for P0: allows basic filesystem
/// reads and writes, HTTPS outbound, and common IPC endpoints; forbids raw
/// disk access and privileged syscalls.  Hardening happens in a later pass.
[[nodiscard]] inline std::string BuildSandboxedInvocation(std::string_view command) {
    using namespace std::string_literals;
    // SEPL-style profile literal (Apple sandbox-exec(1)).
    constexpr std::string_view profile =
        "(version 1)\n"
        "(deny default)\n"
        "(allow file-read*)\n"
        "(allow file-write*)\n"
        "(allow process-exec)\n"
        "(allow process-fork)\n"
        "(allow mach-lookup)\n"
        "(allow system-socket)\n"
        "(allow network-outbound (remote tcp port 443))\n"
        "(allow network-outbound (remote tcp port 80))\n"
        "(allow network-outbound (local ipc unix))\n";
    std::string cmd;
    cmd.reserve(profile.size() + command.size() + 64);
    cmd.assign("/usr/bin/sandbox-exec -p ");
    cmd.append(detail::shell_quote(profile));
    cmd.push_back(' ');
    cmd.append("/bin/sh -c ");
    cmd.append(detail::shell_quote(command));
    return cmd;
}

// --------------------------------------------------------------------------
// ExecuteBash
// --------------------------------------------------------------------------

[[nodiscard]] inline BashOutput ExecuteBash(BashOptions opts) {
    using namespace std::chrono;
    BashOutput out;

    // ---- Resolve final command to exec ---------------------------------
    std::string final_cmd = opts.command;
    if (opts.sandboxed && opts.sandbox_mode == SandboxMode::MacOSSandboxExec) {
        final_cmd = BuildSandboxedInvocation(final_cmd);
    } else if (opts.sandboxed && opts.sandbox_mode == SandboxMode::SoftEnvvars) {
        // SoftEnvvars already covered via build_envp(); command unchanged.
    }

    // ---- Build envp ----------------------------------------------------
    std::vector<std::string> env_store = detail::build_envp(opts.env,
        (opts.sandboxed ? opts.sandbox_mode : SandboxMode::None));
    std::vector<char*> envp; envp.reserve(env_store.size() + 1);
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);

    // ---- Prepare pipes -------------------------------------------------
    detail::pipe_pair out_pipe, err_pipe;
    if (!out_pipe.create()) {
        out.error_reason = "pipe(2) failed for stdout";
        out.exit_code = -1;
        return out;
    }
    if (opts.combine_streams) {
        err_pipe.read_fd  = out_pipe.read_fd;
        err_pipe.write_fd = out_pipe.write_fd;
    } else {
        if (!err_pipe.create()) {
            out_pipe.close_both();
            out.error_reason = "pipe(2) failed for stderr";
            out.exit_code = -1;
            return out;
        }
    }

    // ---- Fork ----------------------------------------------------------
    pid_t pid = ::fork();
    if (pid < 0) {
        out_pipe.close_both();
        if (!opts.combine_streams) err_pipe.close_both();
        out.error_reason = "fork() failed: " + std::string{std::strerror(errno)};
        out.exit_code = -1;
        return out;
    }

    if (pid == 0) {
        // CHILD: wire std{out,err} to pipes, optional cwd, execve.
        ::close(out_pipe.read_fd);
        if (!opts.combine_streams) ::close(err_pipe.read_fd);
        ::dup2(out_pipe.write_fd, STDOUT_FILENO);
        ::dup2(err_pipe.write_fd, STDERR_FILENO);
        ::close(out_pipe.write_fd);
        if (!opts.combine_streams && err_pipe.write_fd != out_pipe.write_fd) ::close(err_pipe.write_fd);

        if (!opts.cwd.empty() && ::chdir(opts.cwd.c_str()) != 0) {
            ::perror("chdir");
            std::_Exit(125);
        }

        std::string arg0{"/bin/sh"};
        std::string arg1{"-c"};
        std::string arg2{final_cmd};
        char* argv[] = {arg0.data(), arg1.data(), arg2.data(), nullptr};
        ::execve(argv[0], argv, envp.data());
        ::perror("execve");
        std::_Exit(126);
    }

    // PARENT
    ::close(out_pipe.write_fd);
    if (!opts.combine_streams && err_pipe.write_fd != out_pipe.write_fd) ::close(err_pipe.write_fd);

    detail::nonblock(out_pipe.read_fd);
    if (!opts.combine_streams) detail::nonblock(err_pipe.read_fd);

    const auto start = steady_clock::now();
    const auto deadline = opts.timeout_sec.has_value()
        ? std::make_optional(start + seconds(*opts.timeout_sec))
        : std::optional<steady_clock::time_point>{};

    // ---- Watchdog thread ----------------------------------------------
    // The watchdog handles the timeout because poll() on multiple fds with a
    // deadline is easier to express as: drain with short poll, then waitpid
    // in a watchdog that can SIGTERM/SIGKILL on expiry.
    struct WatchdogCtx {
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        std::atomic<bool> timed_out{false};
    };
    auto wd = std::make_shared<WatchdogCtx>();
    std::thread watchdog;
    if (deadline.has_value()) {
        watchdog = std::thread([pid, deadline, wd]() {
            std::unique_lock<std::mutex> lk(wd->mtx);
            if (!wd->cv.wait_until(lk, *deadline, [&]{ return wd->done; })) {
                wd->timed_out.store(true);
                // SIGTERM then SIGKILL after 50ms grace.
                ::kill(pid, SIGTERM);
                std::this_thread::sleep_for(milliseconds(50));
                int wstatus{0};
                if (::waitpid(pid, &wstatus, WNOHANG) == 0) {
                    ::kill(pid, SIGKILL);
                }
            }
        });
    }

    // ---- Drain pipes ---------------------------------------------------
    std::array<char, 4096> buf{};
    int  out_fd = out_pipe.read_fd;
    int  err_fd = opts.combine_streams ? out_pipe.read_fd : err_pipe.read_fd;
    bool out_open = true, err_open = opts.combine_streams ? true : true;

    auto read_from = [&](int fd, std::string& target, int cb_fd) {
        while (true) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0) {
                target.append(buf.data(), static_cast<size_t>(n));
                out.combined.append(buf.data(), static_cast<size_t>(n));
                if (opts.on_output_chunk) {
                    opts.on_output_chunk(cb_fd, std::string_view{buf.data(), static_cast<size_t>(n)});
                }
            } else if (n == 0) {
                return false; // EOF
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true; // more later
                return false;
            }
        }
    };

    auto drain_available = [&]() {
        // read out_fd until EAGAIN/EOF, then err_fd if separate.
        if (out_open) {
            if (!read_from(out_fd, out.stdout_only, STDOUT_FILENO)) out_open = false;
        }
        if (!opts.combine_streams && err_open) {
            if (!read_from(err_fd, out.stderr_only, STDERR_FILENO)) err_open = false;
        }
    };

    while (out_open || (err_open && !opts.combine_streams)) {
        std::array<struct pollfd, 2> pfds{};
        nfds_t n = 0;
        if (out_open) {
            pfds[n].fd = out_fd; pfds[n].events = POLLIN; ++n;
        }
        if (!opts.combine_streams && err_open) {
            pfds[n].fd = err_fd; pfds[n].events = POLLIN; ++n;
        }

        int timeout_ms = 250;
        if (deadline.has_value()) {
            auto remain = duration_cast<milliseconds>(*deadline - steady_clock::now());
            if (remain.count() <= 0) timeout_ms = 0;
            else timeout_ms = static_cast<int>(std::min<decltype(remain)::rep>(remain.count(), 250));
        }

        int rv = ::poll(pfds.data(), n, timeout_ms);
        if (rv > 0) {
            drain_available();
        } else if (rv == 0) {
            // Timeout from poll — loop again; deadline may have elapsed.
            if (deadline.has_value() && steady_clock::now() >= *deadline) {
                // Drain whatever is left then break; watchdog will kill.
                drain_available();
                break;
            }
        } else if (rv < 0 && errno != EINTR) {
            out.error_reason = "poll() failed: " + std::string{std::strerror(errno)};
            break;
        }
    }

    // Close read sides
    ::close(out_pipe.read_fd);
    if (!opts.combine_streams && err_pipe.read_fd != out_pipe.read_fd) ::close(err_pipe.read_fd);

    // ---- Reap child ----------------------------------------------------
    int wstatus{0};
    pid_t got = -1;
    for (int tries = 0; tries < 200 && got < 0; ++tries) {
        got = ::waitpid(pid, &wstatus, WNOHANG);
        if (got < 0 && errno == EINTR) { got = -1; continue; }
        if (got == 0) {
            std::this_thread::sleep_for(milliseconds(10));
            got = -1;
        }
    }
    if (got < 0) {
        // Forceful wait forever (watchdog should have SIGKILL'd).
        got = ::waitpid(pid, &wstatus, 0);
    }

    // Signal watchdog completion
    {
        std::lock_guard<std::mutex> lk(wd->mtx);
        wd->done = true;
    }
    wd->cv.notify_all();
    if (watchdog.joinable()) watchdog.join();

    if (wd->timed_out.load()) out.timed_out = true;

    if (WIFEXITED(wstatus)) {
        out.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        out.exit_code = -WTERMSIG(wstatus);
        out.error_reason = out.error_reason.empty()
            ? ("child killed by signal " + std::to_string(WTERMSIG(wstatus)))
            : out.error_reason;
    } else {
        out.exit_code = -1;
    }

    if (out.timed_out && out.error_reason.empty()) {
        out.error_reason = "command timed out after " + std::to_string(*opts.timeout_sec) + "s";
    }
    if (!opts.allow_fail && out.exit_code != 0 && out.error_reason.empty()) {
        out.error_reason = "command exited with code " + std::to_string(out.exit_code);
    }
    return out;
}

/// Convenience facade: no streaming, combined output, short-hand.
[[nodiscard]] inline BashOutput RunBash(std::string cmd, int timeout_sec = 30) {
    BashOptions o; o.command = std::move(cmd);
    if (timeout_sec > 0) o.timeout_sec = timeout_sec;
    return ExecuteBash(std::move(o));
}

} // namespace cc::tools::bash::impl
