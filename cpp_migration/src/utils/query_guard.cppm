module;

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

export module cc.utils.query_guard;

export namespace cc::utils::query_guard {

enum class QueryStatus : unsigned char {
    Idle,
    Dispatching,
    Running,
};

class QueryGuard {
public:
    using Listener = std::function<void()>;
    using Unsubscribe = std::function<void()>;

    [[nodiscard]] bool reserve() {
        if (status_ != QueryStatus::Idle) return false;
        status_ = QueryStatus::Dispatching;
        notify();
        return true;
    }

    void cancel_reservation() {
        if (status_ != QueryStatus::Dispatching) return;
        status_ = QueryStatus::Idle;
        notify();
    }

    [[nodiscard]] std::optional<std::uint64_t> try_start() {
        if (status_ == QueryStatus::Running) return std::nullopt;
        status_ = QueryStatus::Running;
        ++generation_;
        notify();
        return generation_;
    }

    [[nodiscard]] bool end(std::uint64_t generation) {
        if (generation_ != generation) return false;
        if (status_ != QueryStatus::Running) return false;
        status_ = QueryStatus::Idle;
        notify();
        return true;
    }

    void force_end() {
        if (status_ == QueryStatus::Idle) return;
        status_ = QueryStatus::Idle;
        ++generation_;
        notify();
    }

    [[nodiscard]] bool is_active() const noexcept {
        return status_ != QueryStatus::Idle;
    }

    [[nodiscard]] bool get_snapshot() const noexcept {
        return is_active();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] QueryStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] Unsubscribe subscribe(Listener listener) {
        const auto id = next_listener_id_++;
        listeners_.emplace(id, std::move(listener));
        return [this, id] {
            listeners_.erase(id);
        };
    }

private:
    void notify() {
        auto snapshot = listeners_;
        for (const auto& [_, listener] : snapshot) {
            if (listener) listener();
        }
    }

    QueryStatus status_ = QueryStatus::Idle;
    std::uint64_t generation_ = 0;
    std::uint64_t next_listener_id_ = 1;
    std::map<std::uint64_t, Listener> listeners_;
};

} // namespace cc::utils::query_guard
