// Implementation unit for cc.query.query_engine — the single out-of-line
// definition of the shared ID/jitter utilities. Their function-local
// thread_local RNGs stay INSIDE these functions on purpose: promoting them
// to namespace scope would risk static-initialization-order fiasco and
// per-TU engine identity (each definition must remain exactly once).
module;

module cc.query.query_engine;

import std;

namespace cc::core {

[[nodiscard]] std::chrono::milliseconds QueryEngine::add_jitter(std::chrono::milliseconds base) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    auto jitter_range = base.count() / 4;
    std::uniform_int_distribution<long long> dist(-jitter_range, jitter_range);
    return std::chrono::milliseconds(base.count() + dist(rng));
}

[[nodiscard]] std::string QueryEngine::generate_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    return std::format("{:016x}{:016x}", dist(rng), dist(rng));
}

[[nodiscard]] std::string QueryEngine::generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    return std::format("session_{}_{}", ms, generate_id().substr(0, 8));
}

} // namespace cc::core
