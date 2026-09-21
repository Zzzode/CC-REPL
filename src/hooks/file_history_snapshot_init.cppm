module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <filesystem>
#include <chrono>

export module cc.hooks.file_history_snapshot_init;

import cc.state.app_state;

export namespace cc::hooks::file_history_snapshot_init {

struct FileSnapshot {
    std::string file_path;
    std::string content_hash;
    std::chrono::system_clock::time_point captured_at;
};

struct FileHistorySnapshotInitState {
    std::vector<FileSnapshot> snapshots;
    bool initialized{false};
    bool scanning{false};
};

struct FileHistorySnapshotInitOptions {
    std::string workspace_root;
    std::vector<std::string> include_patterns{"*"};
    std::vector<std::string> exclude_patterns{".git", "node_modules"};
};

class FileHistorySnapshotInitHook {
public:
    explicit FileHistorySnapshotInitHook(const FileHistorySnapshotInitOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const FileHistorySnapshotInitState& state() const { return state_; }

    /// Initialize snapshots for tracked files
    void initialize() {
        state_.scanning = true;
        notify();
        // Actual scanning would happen here
        state_.scanning = false;
        state_.initialized = true;
        notify();
    }

    void add_snapshot(FileSnapshot snapshot) {
        state_.snapshots.push_back(std::move(snapshot));
        notify();
    }

    [[nodiscard]] std::optional<FileSnapshot> get_snapshot(std::string_view path) const {
        for (const auto& s : state_.snapshots) {
            if (s.file_path == path) return s;
        }
        return std::nullopt;
    }

    void clear() {
        state_.snapshots.clear();
        state_.initialized = false;
        notify();
    }

    void on_change(std::function<void(const FileHistorySnapshotInitState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    FileHistorySnapshotInitState state_;
    FileHistorySnapshotInitOptions options_;
    std::vector<std::function<void(const FileHistorySnapshotInitState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::file_history_snapshot_init
