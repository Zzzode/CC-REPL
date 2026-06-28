/// @file skill_usage_impl.cpp
/// @brief SL-04 impl unit — heavy I/O (sidecar load/write, debounce map) kept
/// out of the interface module, mirroring the utils/json_impl.cpp PRIVATE pattern.
module;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

module cc.utils.skill_usage;

namespace cc::utils::skill_usage {

namespace fs = std::filesystem;

namespace {

struct Entry {
    long long count = 0;
    long long last_ms = 0;
};

struct Cache {
    std::map<std::string, Entry> entries;
    bool loaded = false;
    std::unordered_set<std::string> debounce;  // per-process dedupe (60s analog)
    std::mutex mu;
};

Cache& cache() {
    static Cache c;
    return c;
}

fs::path sidecar_path() {
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".claude" / "skill_usage.json";
    }
    return fs::path{"skill_usage.json"};
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void load_locked(Cache& c) {
    if (c.loaded) return;
    c.loaded = true;
    std::ifstream f(sidecar_path());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Entry e;
        e.count = std::atoll(line.substr(t1 + 1, t2 - t1 - 1).c_str());
        e.last_ms = std::atoll(line.substr(t2 + 1).c_str());
        c.entries[line.substr(0, t1)] = e;
    }
}

void write_locked(const Cache& c) {
    // Best-effort write; create parent dir if missing.
    std::error_code ec;
    fs::create_directories(sidecar_path().parent_path(), ec);
    std::ofstream f(sidecar_path());
    if (!f) return;
    for (const auto& [name, e] : c.entries) {
        f << name << '\t' << e.count << '\t' << e.last_ms << '\n';
    }
}

}  // namespace

double get_skill_usage_score(std::string_view skill_name) {
    Cache& c = cache();
    std::lock_guard lk(c.mu);
    load_locked(c);
    const auto it = c.entries.find(std::string(skill_name));
    if (it == c.entries.end() || it->second.count == 0) return 0.0;
    const double days = static_cast<double>(now_ms() - it->second.last_ms) / 86400000.0;
    const double factor = std::max(std::pow(0.5, days / 7.0), 0.1);
    return static_cast<double>(it->second.count) * factor;
}

void record_skill_usage(std::string_view skill_name) {
    if (skill_name.empty()) return;
    Cache& c = cache();
    const std::string name(skill_name);
    std::lock_guard lk(c.mu);
    load_locked(c);
    // Per-process dedupe (TS uses a 60s window; a once-per-process gate is a
    // faithful-enough simplification — restarts reset it, matching a session).
    if (c.debounce.count(name)) return;
    c.debounce.insert(name);
    Entry& e = c.entries[name];
    e.count += 1;
    e.last_ms = now_ms();
    write_locked(c);
}

}  // namespace cc::utils::skill_usage
