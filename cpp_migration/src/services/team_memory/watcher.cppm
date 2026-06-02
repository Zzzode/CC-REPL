/// @file watcher.cppm
/// @brief File watcher for team memory directory changes
module;
#include <string>
#include <vector>
#include <functional>
#include <expected>
export module cc.services.team_memory.watcher;
export namespace cc::services::team_memory {
enum class WatchEventType { Created, Modified, Deleted };
struct WatchEvent { std::string path; WatchEventType type; };
using WatchCallback = std::function<void(const WatchEvent&)>;
struct MemoryWatcher {
    std::string watch_path;
    bool is_active{false};
    [[nodiscard]] std::expected<void, std::string> start() { is_active = true; return {}; }
    void stop() { is_active = false; }
};
[[nodiscard]] inline std::expected<MemoryWatcher, std::string> create_memory_watcher(std::string_view path) {
    return MemoryWatcher{std::string(path), false};
}
} // namespace
