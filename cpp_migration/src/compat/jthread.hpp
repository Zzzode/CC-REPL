/// @file jthread.hpp
/// @brief Polyfill for std::jthread / std::stop_token for libc++18 which lacks them.
/// Provides a minimal compatible implementation using std::thread + shared atomic stop flag.
///
/// Usage: In the global module fragment, add #include "compat/jthread.hpp"
///        before any <thread> include. Existing code using std::jthread / std::stop_token
///        will work without changes.
#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace std {

// Minimal stop_token — wraps a shared atomic bool
class stop_token {
public:
    stop_token() = default;
    stop_token(const stop_token&) = default;
    stop_token(stop_token&&) = default;
    stop_token& operator=(const stop_token&) = default;
    stop_token& operator=(stop_token&&) = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

private:
    friend class stop_source;
    std::shared_ptr<std::atomic<bool>> flag_;

    explicit stop_token(std::shared_ptr<std::atomic<bool>> f)
        : flag_(std::move(f)) {}
};

class stop_source {
public:
    stop_source() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    [[nodiscard]] stop_token get_token() const noexcept { return stop_token(flag_); }

    bool request_stop() noexcept {
        bool expected = false;
        return flag_->compare_exchange_strong(expected, true, std::memory_order_release);
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class jthread {
public:
    jthread() = default;

    /// Constructor that accepts a callable taking stop_token as first argument,
    /// followed by zero or more additional arguments — matching std::jthread API.
    /// We wrap the user's callable to inject the stop_token.
    template<typename Fn, typename... Args,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, jthread>>>
    explicit jthread(Fn&& fn, Args&&... args) {
        auto src = std::make_unique<stop_source>();
        auto tok = src->get_token();
        // Bind the stop_token as the first argument
        auto wrapper = [f = std::forward<Fn>(fn), tok](auto&&... a) {
            f(tok, std::forward<decltype(a)>(a)...);
        };
        thread_ = std::thread(std::move(wrapper), std::forward<Args>(args)...);
        token_ = tok;
        src_ = std::move(src);
    }

    ~jthread() {
        request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    jthread(const jthread&) = delete;
    jthread& operator=(const jthread&) = delete;

    jthread(jthread&& other) noexcept
        : thread_(std::move(other.thread_))
        , token_(std::move(other.token_))
        , src_(std::move(other.src_)) {}

    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            request_stop();
            if (thread_.joinable()) thread_.join();
            thread_ = std::move(other.thread_);
            token_ = std::move(other.token_);
            src_ = std::move(other.src_);
        }
        return *this;
    }

    void request_stop() noexcept {
        if (src_) src_->request_stop();
    }

    void join() { thread_.join(); }
    void detach() { thread_.detach(); }
    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    [[nodiscard]] stop_token get_stop_token() const noexcept { return token_; }

    [[nodiscard]] static unsigned int hardware_concurrency() noexcept {
        return std::thread::hardware_concurrency();
    }

private:
    std::thread thread_;
    stop_token token_;
    std::unique_ptr<stop_source> src_;
};

} // namespace std
