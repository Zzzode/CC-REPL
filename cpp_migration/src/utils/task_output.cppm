// C++23 Task Output Module
// Provides task output management with memory and disk buffering
module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module cc.utils.task_output;

import cc.utils.circular_buffer;
import cc.utils.error;
import cc.utils.file;

export namespace cc::utils::task_output {

using cc::utils::Result;
using cc::utils::VoidResult;
using cc::utils::CircularBuffer;
namespace fs = std::filesystem;

// Helper: get last N elements from CircularBuffer
template<typename T, size_t Cap>
std::vector<T> get_recent(const CircularBuffer<T, Cap>& buf, size_t n) {
    std::vector<T> result;
    size_t sz = buf.size();
    size_t start = (sz > n) ? sz - n : 0;
    for (size_t i = start; i < sz; ++i) {
        result.push_back(buf[i]);
    }
    return result;
}


constexpr std::size_t DEFAULT_MAX_MEMORY = 8 * 1024 * 1024;  // 8MB
constexpr std::size_t MAX_TASK_OUTPUT_BYTES = 5ULL * 1024 * 1024 * 1024;  // 5GB
constexpr std::size_t PROGRESS_TAIL_BYTES = 4096;
constexpr int POLL_INTERVAL_MS = 1000;


using ProgressCallback = std::function<void(
    std::string_view last_lines,
    std::string_view all_lines,
    std::size_t total_lines,
    std::size_t total_bytes,
    bool is_incomplete)>;


class DiskTaskOutput;

// =========================================================================

// =========================================================================
class DiskTaskOutput {
public:
    explicit DiskTaskOutput(std::string task_id)
        : task_id_(std::move(task_id)),
          path_(get_task_output_path(task_id_)) {}

    ~DiskTaskOutput() {
        cancel();
        flush();
    }


    DiskTaskOutput(const DiskTaskOutput&) = delete;
    DiskTaskOutput& operator=(const DiskTaskOutput&) = delete;


    void append(std::string_view content) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capped_) return;

        bytes_written_ += content.size();
        if (bytes_written_ > MAX_TASK_OUTPUT_BYTES) {
            capped_ = true;
            queue_.push_back("\n[output truncated: exceeded 5GB disk cap]\n");
        } else {
            queue_.emplace_back(content);
        }

        if (!flush_future_.valid()) {
            flush_future_ = std::async(std::launch::async, [this]() { drain(); });
        }
    }


    VoidResult flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (flush_future_.valid()) {
            try {
                flush_future_.get();
            } catch (...) {

            }
        }
        return {};
    }


    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }


    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!queue_.empty() || !cancelled_) {

            std::deque<std::string> local_queue;
            local_queue.swap(queue_);
            lock.unlock();


            if (!local_queue.empty()) {
                try {
                    ensure_output_dir();
                    std::ofstream file(path_, std::ios::binary | std::ios::app);
                    if (file.is_open()) {
                        for (const auto& chunk : local_queue) {
                            file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                        }
                    }
                } catch (...) {

                }
            }

            lock.lock();
            if (queue_.empty()) break;
        }
    }

    static void ensure_output_dir() {
        static std::once_flag once_flag;
        std::call_once(once_flag, []() {
            std::error_code ec;
            fs::create_directories(get_task_output_dir(), ec);
        });
    }

    std::string task_id_;
    fs::path path_;
    std::mutex mutex_;
    std::deque<std::string> queue_;
    std::size_t bytes_written_ = 0;
    bool capped_ = false;
    bool cancelled_ = false;
    std::future<void> flush_future_;


    static fs::path& get_task_output_dir() {
        static fs::path dir;
        if (dir.empty()) {
            auto tmp_dir = fs::temp_directory_path();
            dir = tmp_dir / "cc_tasks";
        }
        return dir;
    }

    static fs::path get_task_output_path(std::string_view task_id) {
        return get_task_output_dir() / std::format("{}.output", task_id);
    }
};

// =========================================================================

// =========================================================================
class TaskOutput {
public:
    TaskOutput(std::string task_id,
               ProgressCallback on_progress = nullptr,
               bool stdout_to_file = false,
               std::size_t max_memory = DEFAULT_MAX_MEMORY)
        : task_id_(std::move(task_id)),
          path_(DiskTaskOutput(task_id_).path()),
          stdout_to_file_(stdout_to_file),
          max_memory_(max_memory),
          on_progress_(std::move(on_progress)) {}

    ~TaskOutput() {
        clear();
    }


    TaskOutput(const TaskOutput&) = delete;
    TaskOutput& operator=(const TaskOutput&) = delete;


    void write_stdout(std::string_view data) {
        write_buffered(data, false);
    }


    void write_stderr(std::string_view data) {
        write_buffered(data, true);
    }


    [[nodiscard]] Result<std::string> get_stdout() {
        if (stdout_to_file_) {
            return read_stdout_from_file();
        }


        if (disk_) {
            auto recent = get_recent(recent_lines_, 5);
            std::string tail;
            for (const auto& line : recent) {
                if (!tail.empty()) tail += '\n';
                tail += line;
            }
            auto size_kb = total_bytes_ / 1024;
            std::string notice = std::format("\nOutput truncated ({}KB total). Full output saved to: {}",
                                             size_kb, path_.string());
            return tail.empty() ? notice : tail + notice;
        }
        return stdout_buffer_;
    }


    [[nodiscard]] std::string get_stderr() const {
        if (disk_) return {};
        return stderr_buffer_;
    }


    [[nodiscard]] bool is_overflowed() const noexcept { return disk_ != nullptr; }


    [[nodiscard]] std::size_t total_lines() const noexcept { return total_lines_; }


    [[nodiscard]] std::size_t total_bytes() const noexcept { return total_bytes_; }


    [[nodiscard]] bool output_file_redundant() const noexcept { return output_file_redundant_; }


    [[nodiscard]] std::size_t output_file_size() const noexcept { return output_file_size_; }


    void spill_to_disk() {
        if (!disk_) {
            create_disk_output(nullptr, nullptr);
        }
    }


    VoidResult flush() {
        if (disk_) {
            return disk_->flush();
        }
        return {};
    }


    void delete_output_file() {
        std::error_code ec;
        fs::remove(path_, ec);
    }


    void clear() {
        stdout_buffer_.clear();
        stderr_buffer_.clear();
        recent_lines_.clear();
        on_progress_ = nullptr;
        if (disk_) {
            disk_->cancel();
            disk_.reset();
        }
        stop_polling(task_id_);
    }


    static void start_polling(const std::string& task_id) {
        auto& registry = get_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (auto it = registry.instances.find(task_id); it != registry.instances.end()) {
            registry.active_polling.insert(task_id);
            ensure_poller_thread();
        }
    }


    static void stop_polling(const std::string& task_id) {
        auto& registry = get_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.active_polling.erase(task_id);
    }


    void register_for_polling() {
        if (stdout_to_file_ && on_progress_) {
            auto& registry = get_registry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.instances[task_id_] = this;
        }
    }


    void unregister_for_polling() {
        auto& registry = get_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.instances.erase(task_id_);
        registry.active_polling.erase(task_id_);
    }

private:
    void write_buffered(std::string_view data, bool is_stderr) {
        total_bytes_ += data.size();
        update_progress(data);

        if (disk_) {
            disk_->append(is_stderr ? std::format("[stderr] {}", data) : std::string(data));
            return;
        }

        auto total_mem = stdout_buffer_.size() + stderr_buffer_.size() + data.size();
        if (total_mem > max_memory_) {
            create_disk_output(is_stderr ? &data : nullptr, is_stderr ? nullptr : &data);
            return;
        }

        if (is_stderr) {
            stderr_buffer_ += data;
        } else {
            stdout_buffer_ += data;
        }
    }

    void update_progress(std::string_view data) {
        if (!on_progress_) return;

        std::vector<std::string> lines;
        std::size_t extracted_bytes = 0;
        std::size_t line_count = 0;

        auto pos = data.size();
        while (pos > 0 && lines.size() < 100 && extracted_bytes < PROGRESS_TAIL_BYTES) {
            auto prev = data.rfind('\n', pos - 1);
            if (prev == std::string_view::npos) break;

            line_count++;
            auto line = data.substr(prev + 1, pos - prev - 1);
            if (!line.empty()) {
                lines.emplace_back(line);
                extracted_bytes += line.size();
            }
            pos = prev;
        }

        total_lines_ += line_count;

        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            recent_lines_.push_back(std::move(*it));
        }

        if (!lines.empty()) {
            // Get recent N lines from circular buffer
            auto get_recent_n = [&](size_t n) -> std::vector<std::string> {
                std::vector<std::string> result;
                size_t count = std::min(n, recent_lines_.size());
                size_t start = recent_lines_.size() - count;
                for (size_t i = start; i < recent_lines_.size(); ++i) {
                    result.push_back(recent_lines_[i]);
                }
                return result;
            };
            auto recent_5 = get_recent_n(5);
            auto recent_100 = get_recent_n(100);

            std::string last5, last100;
            for (const auto& l : recent_5) {
                if (!last5.empty()) last5 += '\n';
                last5 += l;
            }
            for (const auto& l : recent_100) {
                if (!last100.empty()) last100 += '\n';
                last100 += l;
            }

            on_progress_(last5, last100, total_lines_, total_bytes_, disk_ != nullptr);
        }
    }

    void create_disk_output(std::string_view* stderr_chunk, std::string_view* stdout_chunk) {
        disk_ = std::make_unique<DiskTaskOutput>(task_id_);

        if (!stdout_buffer_.empty()) {
            disk_->append(stdout_buffer_);
            stdout_buffer_.clear();
        }
        if (!stderr_buffer_.empty()) {
            disk_->append(std::format("[stderr] {}", stderr_buffer_));
            stderr_buffer_.clear();
        }

        if (stdout_chunk) {
            disk_->append(*stdout_chunk);
        }
        if (stderr_chunk) {
            disk_->append(std::format("[stderr] {}", *stderr_chunk));
        }
    }

    Result<std::string> read_stdout_from_file() {
        constexpr std::size_t MAX_READ = 8 * 1024 * 1024;
        try {
            auto content_result = file::read_file(path_);
            if (!content_result) {
                output_file_redundant_ = true;
                return "";
            }

            auto content = std::move(*content_result);
            output_file_size_ = content.size();
            output_file_redundant_ = content.size() <= MAX_READ;

            if (content.size() > MAX_READ) {
                content = content.substr(0, MAX_READ);
            }
            return content;
        } catch (...) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::io_error,
                std::format("Cannot read output file: {}", path_.string())));
        }
    }


    struct PollRegistry {
        std::mutex mutex;
        std::map<std::string, TaskOutput*> instances;
        std::set<std::string> active_polling;
        std::thread poller_thread;
        std::atomic<bool> poller_running{false};
        std::condition_variable poller_cv;
    };

    static PollRegistry& get_registry() {
        static PollRegistry registry;
        return registry;
    }

    static void ensure_poller_thread() {
        auto& registry = get_registry();
        if (!registry.poller_running.exchange(true)) {
            registry.poller_thread = std::thread([]() {
                auto& reg = get_registry();
                while (reg.poller_running) {
                    {
                        std::unique_lock<std::mutex> lock(reg.mutex);
                        reg.poller_cv.wait_for(lock, std::chrono::milliseconds(POLL_INTERVAL_MS));

                        if (!reg.poller_running) break;


                        std::vector<std::pair<std::string, TaskOutput*>> active;
                        for (const auto& task_id : reg.active_polling) {
                            if (auto it = reg.instances.find(task_id); it != reg.instances.end()) {
                                active.emplace_back(task_id, it->second);
                            }
                        }
                        lock.unlock();


                        for (auto& [task_id, instance] : active) {
                            poll_instance(instance);
                        }
                    }
                }
            });
        }
    }

    static void poll_instance(TaskOutput* instance) {
        if (!instance || !instance->on_progress_) return;

        try {
            auto size_result = file::get_file_size(instance->path_);
            std::size_t file_size = size_result ? *size_result : 0;


            instance->on_progress_("", "", instance->total_lines_, file_size, false);
        } catch (...) {

        }
    }

    std::string task_id_;
    fs::path path_;
    bool stdout_to_file_;
    std::string stdout_buffer_;
    std::string stderr_buffer_;
    std::unique_ptr<DiskTaskOutput> disk_;
    CircularBuffer<std::string, 1000> recent_lines_;
    std::size_t total_lines_ = 0;
    std::size_t total_bytes_ = 0;
    std::size_t max_memory_;
    ProgressCallback on_progress_;
    bool output_file_redundant_ = false;
    std::size_t output_file_size_ = 0;
};

// =========================================================================

// =========================================================================


[[nodiscard]] inline auto get_task_output_path(std::string_view task_id) -> fs::path {
    auto tmp_dir = fs::temp_directory_path();
    return tmp_dir / "cc_tasks" / std::format("{}.output", task_id);
}


inline void ensure_task_output_dir() {
    std::error_code ec;
    auto dir = fs::temp_directory_path() / "cc_tasks";
    fs::create_directories(dir, ec);
}


[[nodiscard]] inline auto init_task_output(std::string_view task_id) -> Result<fs::path> {
    ensure_task_output_dir();
    auto path = get_task_output_path(task_id);


    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::io_error,
            std::format("Cannot create output file: {}", path.string())));
    }
    return path;
}


[[nodiscard]] inline auto get_task_output(std::string_view task_id,
                                          std::size_t max_bytes = 8 * 1024 * 1024) -> Result<std::string> {
    auto path = get_task_output_path(task_id);
    auto size_result = file::get_file_size(path);
    if (!size_result) return "";

    auto file_size = *size_result;
    auto read_size = std::min(file_size, max_bytes);

    if (file_size == 0) return "";

    auto content_result = file::read_file(path);
    if (!content_result) return "";

    auto content = std::move(*content_result);
    if (content.size() > max_bytes) {
        auto omitted = (content.size() - max_bytes) / 1024;
        return std::format("[{}KB of earlier output omitted]\n{}",
                          omitted, content.substr(content.size() - max_bytes));
    }
    return content;
}


inline void cleanup_task_output(std::string_view task_id) {
    auto path = get_task_output_path(task_id);
    std::error_code ec;
    fs::remove(path, ec);
}

} // namespace cc::utils::task_output
