// MonitorTool - Real-time process and file monitoring
module;
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.tools.monitor;


export namespace cc::tools {

// 监控操作类型
enum class MonitorAction {
    WatchFile,       // 监控文件变化
    WatchProcess,    // 监控进程输出
    StatusCheck,     // 周期性状态检查
    Stop,            // 停止监控
};

// 监控错误类型
enum class MonitorError {
    TargetNotFound,
    AlreadyWatching,
    NotWatching,
    WatchFailed,
    ProcessNotFound,
    Timeout,
    TooManyWatchers,
    InvalidInterval,
};

constexpr auto format_error(MonitorError err) -> std::string_view {
    switch (err) {
        case MonitorError::TargetNotFound:   return "Monitor target not found";
        case MonitorError::AlreadyWatching:  return "Already watching this target";
        case MonitorError::NotWatching:      return "Not currently watching this target";
        case MonitorError::WatchFailed:      return "Failed to set up file watcher";
        case MonitorError::ProcessNotFound:  return "Process not found";
        case MonitorError::Timeout:          return "Monitor operation timed out";
        case MonitorError::TooManyWatchers:  return "Maximum number of watchers exceeded";
        case MonitorError::InvalidInterval:  return "Invalid check interval (min 1 second)";
        default:                             return "Unknown monitor error";
    }
}

// 文件变化事件类型
enum class FileChangeType {
    Created,
    Modified,
    Deleted,
    Renamed,
};

constexpr auto change_type_name(FileChangeType t) -> std::string_view {
    switch (t) {
        case FileChangeType::Created:  return "created";
        case FileChangeType::Modified: return "modified";
        case FileChangeType::Deleted:  return "deleted";
        case FileChangeType::Renamed:  return "renamed";
        default:                       return "unknown";
    }
}

// 文件变化事件
struct FileChangeEvent {
    std::filesystem::path path;
    FileChangeType type;
    std::chrono::system_clock::time_point timestamp;
    std::optional<size_t> new_size;
};

// 进程输出事件
struct ProcessOutputEvent {
    pid_t pid;
    std::string output_line;
    bool is_stderr{false};
    std::chrono::system_clock::time_point timestamp;
};

// 监控请求
struct MonitorRequest {
    MonitorAction action;
    std::optional<std::filesystem::path> file_path;      // 文件监控路径
    std::optional<pid_t> process_id;                     // 进程 ID
    std::chrono::seconds check_interval{5};              // 检查间隔
    std::chrono::seconds duration{60};                   // 监控总时长
    std::optional<std::string> watcher_id;               // 用于 Stop 操作
};

// 监控结果
struct MonitorResult {
    std::string watcher_id;
    std::vector<FileChangeEvent> file_events;
    std::vector<ProcessOutputEvent> process_events;
    std::chrono::milliseconds elapsed{0};
    bool still_running{false};
};

// 文件状态快照：用于检测变化
struct FileSnapshot {
    std::filesystem::path path;
    std::filesystem::file_time_type last_write_time;
    size_t file_size{0};
    bool exists{false};
};

// 监控器注册表
class WatcherRegistry {
public:
    static constexpr size_t kMaxWatchers = 10;

    auto register_watcher(std::string id) -> std::expected<void, MonitorError> {
        if (watchers_.size() >= kMaxWatchers) {
            return std::unexpected(MonitorError::TooManyWatchers);
        }
        if (watchers_.contains(id)) {
            return std::unexpected(MonitorError::AlreadyWatching);
        }
        watchers_.insert(std::move(id));
        return {};
    }

    auto unregister_watcher(const std::string& id) -> std::expected<void, MonitorError> {
        if (!watchers_.contains(id)) {
            return std::unexpected(MonitorError::NotWatching);
        }
        watchers_.erase(id);
        return {};
    }

    [[nodiscard]] bool is_watching(const std::string& id) const {
        return watchers_.contains(id);
    }

    [[nodiscard]] size_t count() const { return watchers_.size(); }

private:
    std::unordered_set<std::string> watchers_;
};

// MonitorTool - 实时监控工具
class MonitorTool {
public:
    static constexpr std::string_view name = "monitor";
    static constexpr std::string_view description = "Real-time monitoring of file changes and process output";
    static constexpr std::chrono::seconds kMinInterval{1};
    static constexpr std::chrono::seconds kMaxDuration{300};

    auto validate(const MonitorRequest& request) const -> std::expected<void, MonitorError> {
        if (request.check_interval < kMinInterval) {
            return std::unexpected(MonitorError::InvalidInterval);
        }
        if (request.action == MonitorAction::WatchFile && request.file_path) {
            if (!std::filesystem::exists(*request.file_path)) {
                return std::unexpected(MonitorError::TargetNotFound);
            }
        }
        if (request.action == MonitorAction::Stop) {
            if (!request.watcher_id) {
                return std::unexpected(MonitorError::NotWatching);
            }
        }
        return {};
    }

    auto execute(MonitorRequest request) -> std::expected<MonitorResult, MonitorError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        switch (request.action) {
            case MonitorAction::WatchFile:
                return watch_file(request);
            case MonitorAction::WatchProcess:
                return watch_process(request);
            case MonitorAction::StatusCheck:
                return status_check(request);
            case MonitorAction::Stop:
                return stop_watching(*request.watcher_id);
            default:
                return std::unexpected(MonitorError::WatchFailed);
        }
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["watch_file", "watch_process", "status_check", "stop"], "description": "Monitor action" }},
      "file_path": {{ "type": "string", "description": "File path to monitor for changes" }},
      "process_id": {{ "type": "integer", "description": "Process ID to monitor output" }},
      "check_interval": {{ "type": "integer", "description": "Check interval in seconds (default 5)" }},
      "duration": {{ "type": "integer", "description": "Total monitoring duration in seconds (default 60)" }},
      "watcher_id": {{ "type": "string", "description": "Watcher ID (for stop action)" }}
    }},
    "required": ["action"]
  }}
}})json", name, description);
    }

private:
    WatcherRegistry registry_;
    size_t next_id_{0};

    auto generate_watcher_id() -> std::string {
        return std::format("watch_{}", next_id_++);
    }

    // 监控文件变化 (单次轮询)
    auto watch_file(const MonitorRequest& request)
        -> std::expected<MonitorResult, MonitorError>
    {
        auto id = generate_watcher_id();
        auto reg_result = registry_.register_watcher(id);
        if (!reg_result) return std::unexpected(reg_result.error());

        auto start = std::chrono::steady_clock::now();
        MonitorResult result{.watcher_id = id};

        // 捕获初始状态
        auto snapshot = take_snapshot(*request.file_path);

        // 执行一次检查周期
        std::this_thread::sleep_for(request.check_interval);

        auto new_snapshot = take_snapshot(*request.file_path);

        // 比较变化
        if (new_snapshot.exists != snapshot.exists) {
            result.file_events.push_back(FileChangeEvent{
                .path = *request.file_path,
                .type = new_snapshot.exists ? FileChangeType::Created : FileChangeType::Deleted,
                .timestamp = std::chrono::system_clock::now(),
                .new_size = new_snapshot.exists ? std::optional(new_snapshot.file_size) : std::nullopt,
            });
        } else if (new_snapshot.last_write_time != snapshot.last_write_time) {
            result.file_events.push_back(FileChangeEvent{
                .path = *request.file_path,
                .type = FileChangeType::Modified,
                .timestamp = std::chrono::system_clock::now(),
                .new_size = new_snapshot.file_size,
            });
        }

        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        result.still_running = true;
        return result;
    }

    // 监控进程输出 (占位实现)
    auto watch_process(const MonitorRequest& request)
        -> std::expected<MonitorResult, MonitorError>
    {
        if (!request.process_id) {
            return std::unexpected(MonitorError::ProcessNotFound);
        }
        auto id = generate_watcher_id();
        return MonitorResult{
            .watcher_id = id,
            .still_running = true,
        };
    }

    // 状态检查
    auto status_check(const MonitorRequest& /*request*/)
        -> std::expected<MonitorResult, MonitorError>
    {
        return MonitorResult{
            .watcher_id = "status",
            .still_running = registry_.count() > 0,
        };
    }

    // 停止监控
    auto stop_watching(const std::string& watcher_id)
        -> std::expected<MonitorResult, MonitorError>
    {
        auto result = registry_.unregister_watcher(watcher_id);
        if (!result) return std::unexpected(result.error());
        return MonitorResult{.watcher_id = watcher_id, .still_running = false};
    }

    // 获取文件快照
    auto take_snapshot(const std::filesystem::path& path) const -> FileSnapshot {
        FileSnapshot snap{.path = path};
        if (std::filesystem::exists(path)) {
            snap.exists = true;
            snap.file_size = std::filesystem::file_size(path);
            snap.last_write_time = std::filesystem::last_write_time(path);
        }
        return snap;
    }
};

} // namespace cc::tools
