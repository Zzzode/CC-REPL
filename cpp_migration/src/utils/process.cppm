// C++23 Process Management Module (based on libuv)
// Provides async process spawning, management, and pooling
module;

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <uv.h>
#include <signal.h>
#include <sys/types.h>

export module cc.utils.process;

import cc.utils.error;
import cc.utils.async;

export namespace cc::utils::process {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::VoidResult;
using cc::utils::async::EventLoop;
using cc::utils::async::Task;


struct ProcessOptions {
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    std::optional<std::chrono::milliseconds> timeout;
    bool inherit_env = true;
    bool capture_stdout = true;
    bool capture_stderr = true;
};


struct ProcessResult {
    int exit_code;
    std::string stdout_data;
    std::string stderr_data;
    bool timed_out = false;
};

// =========================================================================

// =========================================================================
namespace detail {

struct ProcessContext {
    uv_process_t process{};
    uv_pipe_t stdout_pipe{};
    uv_pipe_t stderr_pipe{};
    uv_timer_t timer{};
    std::string stdout_buf;
    std::string stderr_buf;
    int64_t exit_status = -1;
    bool completed = false;
    bool timed_out = false;
    std::coroutine_handle<> continuation;

    static void on_exit(uv_process_t* proc, int64_t status, int /*signal*/) {
        auto* ctx = static_cast<ProcessContext*>(proc->data);
        ctx->exit_status = status;
        ctx->completed = true;

        uv_timer_stop(&ctx->timer);

        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->stdout_pipe), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->stderr_pipe), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->timer), nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(proc), [](uv_handle_t* h) {
            auto* c = static_cast<ProcessContext*>(h->data);
            if (c->continuation) c->continuation.resume();
        });
    }

    static void on_alloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
        buf->base = new char[suggested];
        buf->len = static_cast<unsigned int>(suggested);
    }

    static void on_stdout_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* ctx = static_cast<ProcessContext*>(stream->data);
        if (nread > 0) {
            ctx->stdout_buf.append(buf->base, static_cast<std::size_t>(nread));
        }
        delete[] buf->base;
        if (nread < 0) {
            uv_read_stop(stream);
        }
    }

    static void on_stderr_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* ctx = static_cast<ProcessContext*>(stream->data);
        if (nread > 0) {
            ctx->stderr_buf.append(buf->base, static_cast<std::size_t>(nread));
        }
        delete[] buf->base;
        if (nread < 0) {
            uv_read_stop(stream);
        }
    }

    static void on_timeout(uv_timer_t* timer) {
        auto* ctx = static_cast<ProcessContext*>(timer->data);
        ctx->timed_out = true;

        uv_process_kill(&ctx->process, SIGKILL);
    }
};

} // namespace detail

// =========================================================================

// =========================================================================
inline Task<Result<ProcessResult>> spawn_process(
    ProcessOptions opts, EventLoop& loop = EventLoop::default_loop()) {

    auto ctx = std::make_unique<detail::ProcessContext>();


    uv_pipe_init(loop.raw(), &ctx->stdout_pipe, 0);
    uv_pipe_init(loop.raw(), &ctx->stderr_pipe, 0);
    uv_timer_init(loop.raw(), &ctx->timer);

    ctx->stdout_pipe.data = ctx.get();
    ctx->stderr_pipe.data = ctx.get();
    ctx->timer.data = ctx.get();
    ctx->process.data = ctx.get();


    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(opts.command.c_str()));
    for (auto& arg : opts.args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);


    std::vector<std::string> env_strings;
    std::vector<char*> envp;
    if (!opts.env.empty()) {
        for (auto& [key, val] : opts.env) {
            env_strings.push_back(std::format("{}={}", key, val));
        }
        for (auto& s : env_strings) {
            envp.push_back(const_cast<char*>(s.c_str()));
        }
        envp.push_back(nullptr);
    }


    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&ctx->stdout_pipe);
    stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&ctx->stderr_pipe);

    uv_process_options_t uv_opts{};
    uv_opts.exit_cb = detail::ProcessContext::on_exit;
    uv_opts.file = opts.command.c_str();
    uv_opts.args = argv.data();
    uv_opts.env = envp.empty() ? nullptr : envp.data();
    uv_opts.cwd = opts.cwd.empty() ? nullptr : opts.cwd.c_str();
    uv_opts.stdio_count = 3;
    uv_opts.stdio = stdio;


    int r = uv_spawn(loop.raw(), &ctx->process, &uv_opts);
    if (r < 0) {
        co_return std::unexpected(Error(ErrorCode::io_error,
            std::format("Failed to spawn '{}': {}", opts.command, uv_strerror(r))));
    }


    uv_read_start(reinterpret_cast<uv_stream_t*>(&ctx->stdout_pipe),
        detail::ProcessContext::on_alloc, detail::ProcessContext::on_stdout_read);
    uv_read_start(reinterpret_cast<uv_stream_t*>(&ctx->stderr_pipe),
        detail::ProcessContext::on_alloc, detail::ProcessContext::on_stderr_read);


    if (opts.timeout) {
        uv_timer_start(&ctx->timer, detail::ProcessContext::on_timeout,
            static_cast<uint64_t>(opts.timeout->count()), 0);
    }


    co_await std::suspend_always{};

    co_return ProcessResult{
        .exit_code = static_cast<int>(ctx->exit_status),
        .stdout_data = std::move(ctx->stdout_buf),
        .stderr_data = std::move(ctx->stderr_buf),
        .timed_out = ctx->timed_out,
    };
}

// =========================================================================

// =========================================================================
[[nodiscard]] inline Result<pid_t> spawn_detached(
    ProcessOptions opts, EventLoop& loop = EventLoop::default_loop()) {

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(opts.command.c_str()));
    for (auto& arg : opts.args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    uv_process_t process{};
    uv_process_options_t uv_opts{};
    uv_opts.exit_cb = [](uv_process_t* p, int64_t, int) {
        uv_close(reinterpret_cast<uv_handle_t*>(p), nullptr);
    };
    uv_opts.file = opts.command.c_str();
    uv_opts.args = argv.data();
    uv_opts.cwd = opts.cwd.empty() ? nullptr : opts.cwd.c_str();
    uv_opts.flags = UV_PROCESS_DETACHED;


    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_IGNORE;
    stdio[2].flags = UV_IGNORE;
    uv_opts.stdio_count = 3;
    uv_opts.stdio = stdio;

    int r = uv_spawn(loop.raw(), &process, &uv_opts);
    if (r < 0) {
        return std::unexpected(Error(ErrorCode::io_error,
            std::format("Failed to spawn detached '{}': {}", opts.command, uv_strerror(r))));
    }

    pid_t pid = process.pid;
    uv_unref(reinterpret_cast<uv_handle_t*>(&process));
    return pid;
}

// =========================================================================

// =========================================================================
[[nodiscard]] inline VoidResult kill_process(pid_t pid, int signal = SIGTERM) {
    int r = uv_kill(pid, signal);
    if (r < 0) {
        return std::unexpected(Error(ErrorCode::io_error,
            std::format("Failed to kill process {}: {}", pid, uv_strerror(r))));
    }
    return {};
}

// =========================================================================

// =========================================================================
[[nodiscard]] inline bool is_process_running(pid_t pid) noexcept {

    return uv_kill(pid, 0) == 0;
}

// =========================================================================

// =========================================================================
class ProcessPool {
public:
    explicit ProcessPool(std::size_t max_concurrent = 4,
                         EventLoop& loop = EventLoop::default_loop())
        : max_concurrent_(max_concurrent), loop_(loop) {}


    void submit(ProcessOptions opts,
                std::function<void(Result<ProcessResult>)> callback) {
        pending_.push_back({std::move(opts), std::move(callback)});
        try_start_next();
    }

    [[nodiscard]] std::size_t active_count() const noexcept { return active_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    void try_start_next() {
        while (active_ < max_concurrent_ && !pending_.empty()) {
            auto [opts, cb] = std::move(pending_.front());
            pending_.pop_front();
            ++active_;


        }
    }

    struct PendingJob {
        ProcessOptions opts;
        std::function<void(Result<ProcessResult>)> callback;
    };

    std::size_t max_concurrent_;
    std::size_t active_ = 0;
    std::deque<PendingJob> pending_;
    EventLoop& loop_;
};

} // namespace cc::utils::process
