// C++23 Coroutine Infrastructure Module (based on libuv)
// Provides Task<T>, EventLoop, timers, async IO, channels, and combinators
module;

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <uv.h>

export module cc.utils.async;

import cc.utils.error;

export namespace cc::utils::async {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;

// =========================================================================

// =========================================================================
template<typename T = void>
class Task {
public:
    struct Promise;
    using promise_type = Promise;
    using handle_type = std::coroutine_handle<Promise>;

    struct Promise {
        std::optional<T> value;
        std::coroutine_handle<> continuation;
        std::exception_ptr exception;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }


        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                if (h.promise().continuation)
                    return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T val) { value = std::move(val); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    Task() noexcept : handle_(nullptr) {}
    explicit Task(handle_type h) noexcept : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }


    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;


    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle_.promise().continuation = awaiting;
        return handle_;
    }
    T await_resume() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
        return std::move(*handle_.promise().value);
    }


    T get() {
        handle_.resume();
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
        return std::move(*handle_.promise().value);
    }

    [[nodiscard]] handle_type raw_handle() const noexcept { return handle_; }

private:
    handle_type handle_;
};


template<>
class Task<void> {
public:
    struct Promise;
    using promise_type = Promise;
    using handle_type = std::coroutine_handle<Promise>;

    struct Promise {
        std::coroutine_handle<> continuation;
        std::exception_ptr exception;
        bool completed = false;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                if (h.promise().continuation)
                    return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() { completed = true; }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    Task() noexcept : handle_(nullptr) {}
    explicit Task(handle_type h) noexcept : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle_.promise().continuation = awaiting;
        return handle_;
    }
    void await_resume() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
    }

    [[nodiscard]] handle_type raw_handle() const noexcept { return handle_; }

private:
    handle_type handle_;
};

// =========================================================================

// =========================================================================
class EventLoop {
public:
    EventLoop() { uv_loop_init(&loop_); }
    ~EventLoop() { uv_loop_close(&loop_); }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;


    void run() { uv_run(&loop_, UV_RUN_DEFAULT); }

    void run_once() { uv_run(&loop_, UV_RUN_ONCE); }

    void stop() { uv_stop(&loop_); }

    [[nodiscard]] uv_loop_t* raw() noexcept { return &loop_; }


    static EventLoop& default_loop() {
        static EventLoop instance;
        return instance;
    }

private:
    uv_loop_t loop_{};
};

// =========================================================================

// =========================================================================
struct SleepAwaiter {
    EventLoop& loop;
    uint64_t ms;
    uv_timer_t timer{};
    std::coroutine_handle<> handle;

    bool await_ready() const noexcept { return ms == 0; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        uv_timer_init(loop.raw(), &timer);
        timer.data = this;
        uv_timer_start(&timer, [](uv_timer_t* t) {
            auto* self = static_cast<SleepAwaiter*>(t->data);
            uv_timer_stop(t);
            uv_close(reinterpret_cast<uv_handle_t*>(t), nullptr);
            self->handle.resume();
        }, ms, 0);
    }

    void await_resume() noexcept {}
};

[[nodiscard]] inline SleepAwaiter sleep(
    uint64_t ms, EventLoop& loop = EventLoop::default_loop()) {
    return SleepAwaiter{loop, ms, {}, {}};
}

// =========================================================================

// =========================================================================
template<typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity = 0) : capacity_(capacity) {}


    struct SendAwaiter {
        Channel& ch;
        T value;
        bool await_ready() { return ch.buffer_.size() < ch.capacity_; }
        void await_suspend(std::coroutine_handle<> h) {
            ch.send_waiters_.push_back({h, std::move(value)});
        }
        void await_resume() {}
    };


    struct RecvAwaiter {
        Channel& ch;
        bool await_ready() { return !ch.buffer_.empty(); }
        void await_suspend(std::coroutine_handle<> h) {
            ch.recv_waiters_.push_back(h);
        }
        T await_resume() {
            T val = std::move(ch.buffer_.front());
            ch.buffer_.pop_front();

            if (!ch.send_waiters_.empty()) {
                auto [handle, send_val] = std::move(ch.send_waiters_.front());
                ch.send_waiters_.pop_front();
                ch.buffer_.push_back(std::move(send_val));
                handle.resume();
            }
            return val;
        }
    };

    SendAwaiter send(T value) { return {*this, std::move(value)}; }
    RecvAwaiter recv() { return {*this}; }

    [[nodiscard]] bool empty() const { return buffer_.empty(); }
    [[nodiscard]] std::size_t size() const { return buffer_.size(); }

private:
    std::size_t capacity_;
    std::deque<T> buffer_;

    struct PendingSend {
        std::coroutine_handle<> handle;
        T value;
    };
    std::deque<PendingSend> send_waiters_;
    std::deque<std::coroutine_handle<>> recv_waiters_;
};

// =========================================================================

// =========================================================================
class AsyncFile {
public:
    explicit AsyncFile(EventLoop& loop = EventLoop::default_loop())
        : loop_(loop), fd_(-1) {}

    ~AsyncFile() { if (fd_ >= 0) close_sync(); }

    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;


    Task<Result<void>> open(const std::string& path, int flags, int mode = 0644) {
        struct OpenCtx {
            uv_fs_t req{};
            std::coroutine_handle<> handle;
            int result = -1;
        };
        OpenCtx ctx;
        ctx.req.data = &ctx;

        uv_fs_open(loop_.raw(), &ctx.req, path.c_str(), flags, mode,
            [](uv_fs_t* req) {
                auto* c = static_cast<OpenCtx*>(req->data);
                c->result = static_cast<int>(req->result);
                uv_fs_req_cleanup(req);
                c->handle.resume();
            });


        co_await std::suspend_always{};

        if (ctx.result < 0) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("Failed to open file: {}", uv_strerror(ctx.result))));
        }
        fd_ = ctx.result;
        co_return Result<void>{};
    }


    Task<Result<std::string>> read(std::size_t size, int64_t offset = -1) {
        std::string buffer(size, '\0');
        uv_buf_t buf = uv_buf_init(buffer.data(), static_cast<unsigned int>(size));

        struct ReadCtx {
            uv_fs_t req{};
            std::coroutine_handle<> handle;
            ssize_t result = -1;
        };
        ReadCtx ctx;
        ctx.req.data = &ctx;

        uv_fs_read(loop_.raw(), &ctx.req, fd_, &buf, 1, offset,
            [](uv_fs_t* req) {
                auto* c = static_cast<ReadCtx*>(req->data);
                c->result = req->result;
                uv_fs_req_cleanup(req);
                c->handle.resume();
            });

        co_await std::suspend_always{};

        if (ctx.result < 0) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("Read failed: {}", uv_strerror(static_cast<int>(ctx.result)))));
        }
        buffer.resize(static_cast<std::size_t>(ctx.result));
        co_return buffer;
    }


    Task<Result<std::size_t>> write(std::string_view data, int64_t offset = -1) {
        uv_buf_t buf = uv_buf_init(const_cast<char*>(data.data()),
                                    static_cast<unsigned int>(data.size()));
        struct WriteCtx {
            uv_fs_t req{};
            std::coroutine_handle<> handle;
            ssize_t result = -1;
        };
        WriteCtx ctx;
        ctx.req.data = &ctx;

        uv_fs_write(loop_.raw(), &ctx.req, fd_, &buf, 1, offset,
            [](uv_fs_t* req) {
                auto* c = static_cast<WriteCtx*>(req->data);
                c->result = req->result;
                uv_fs_req_cleanup(req);
                c->handle.resume();
            });

        co_await std::suspend_always{};

        if (ctx.result < 0) {
            co_return std::unexpected(Error(ErrorCode::io_error,
                std::format("Write failed: {}", uv_strerror(static_cast<int>(ctx.result)))));
        }
        co_return static_cast<std::size_t>(ctx.result);
    }

private:
    void close_sync() {
        uv_fs_t req;
        uv_fs_close(loop_.raw(), &req, fd_, nullptr);
        uv_fs_req_cleanup(&req);
        fd_ = -1;
    }

    EventLoop& loop_;
    int fd_;
};

// =========================================================================

// =========================================================================
struct ProcessOutput {
    int exit_code;
    std::string stdout_data;
    std::string stderr_data;
};

// =========================================================================

// =========================================================================
template<typename... Tasks>
Task<std::tuple<typename std::remove_reference_t<Tasks>::handle_type::promise_type...>>
when_all(Tasks&&... tasks) {

    co_return std::make_tuple((co_await std::forward<Tasks>(tasks))...);
}

// WhenAll for vector of same-type tasks
template<typename T>
Task<std::vector<T>> when_all_vec(std::vector<Task<T>> tasks) {
    std::vector<T> results;
    results.reserve(tasks.size());
    for (auto& task : tasks) {
        results.push_back(co_await std::move(task));
    }
    co_return results;
}

// =========================================================================

// =========================================================================
template<typename T>
struct WhenAnyResult {
    std::size_t index;
    T value;
};

template<typename T>
Task<WhenAnyResult<T>> when_any(std::vector<Task<T>> tasks) {

    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].raw_handle() && !tasks[i].raw_handle().done()) {
            auto result = co_await std::move(tasks[i]);
            co_return WhenAnyResult<T>{i, std::move(result)};
        }
    }

    auto result = co_await std::move(tasks[0]);
    co_return WhenAnyResult<T>{0, std::move(result)};
}

} // namespace cc::utils::async
