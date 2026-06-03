module;
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <format>
#include <sstream>

export module cc.utils.error_log_sink;

import cc.utils.errors_utils;

export namespace cc::utils {


class ErrorLogSink {
public:

    static std::filesystem::path default_log_path() {
        const char* home = std::getenv("HOME");
        if (!home) home = "/tmp";
        return std::filesystem::path(home) / ".claude" / "logs" / "errors.jsonl";
    }

    explicit ErrorLogSink(std::filesystem::path log_path = default_log_path(),
                          size_t max_file_size = 10 * 1024 * 1024) // 10MB
        : log_path_(std::move(log_path)),
          max_file_size_(max_file_size),
          running_(true) {

        auto parent = log_path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        writer_thread_ = std::thread([this]() { writer_loop(); });
    }

    ~ErrorLogSink() {

        {
            std::lock_guard lock(queue_mutex_);
            running_ = false;
        }
        queue_cv_.notify_one();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }


    ErrorLogSink(const ErrorLogSink&) = delete;
    ErrorLogSink& operator=(const ErrorLogSink&) = delete;


    void log_error(const ClassifiedError& error) {
        auto json_line = serialize_error(error);
        {
            std::lock_guard lock(queue_mutex_);
            write_queue_.push(std::move(json_line));
        }
        queue_cv_.notify_one();
    }


    [[nodiscard]] std::vector<ClassifiedError> get_recent_errors(size_t n) const {
        std::vector<ClassifiedError> result;
        std::ifstream file(log_path_);
        if (!file.is_open()) return result;


        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty()) lines.push_back(std::move(line));
        }

        size_t start = lines.size() > n ? lines.size() - n : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            if (auto err = deserialize_error(lines[i]); err.has_value()) {
                result.push_back(std::move(*err));
            }
        }
        return result;
    }


    void rotate_logs() {
        std::lock_guard lock(file_mutex_);
        std::error_code ec;
        auto size = std::filesystem::file_size(log_path_, ec);
        if (ec || size < max_file_size_) return;


        auto backup_path = log_path_;
        backup_path += ".1";
        std::filesystem::rename(log_path_, backup_path, ec);

        auto old_backup = log_path_;
        old_backup += ".2";
        std::filesystem::remove(old_backup, ec);
    }


    [[nodiscard]] const std::filesystem::path& path() const { return log_path_; }

private:
    std::filesystem::path log_path_;
    size_t max_file_size_;
    std::atomic<bool> running_;

    std::queue<std::string> write_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::mutex file_mutex_;
    std::thread writer_thread_;


    void writer_loop() {
        while (true) {
            std::string entry;
            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return !write_queue_.empty() || !running_;
                });
                if (!running_ && write_queue_.empty()) break;
                if (write_queue_.empty()) continue;
                entry = std::move(write_queue_.front());
                write_queue_.pop();
            }


            {
                std::lock_guard lock(file_mutex_);
                std::ofstream file(log_path_, std::ios::app);
                if (file.is_open()) {
                    file << entry << '\n';
                }
            }


            std::error_code ec;
            auto size = std::filesystem::file_size(log_path_, ec);
            if (!ec && size >= max_file_size_) {
                rotate_logs();
            }
        }
    }


    [[nodiscard]] static std::string serialize_error(const ClassifiedError& error) {
        auto now = std::chrono::system_clock::now();
        auto time_t_val = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        gmtime_r(&time_t_val, &tm_buf);

        std::ostringstream oss;
        oss << "{\"timestamp\":\"" << std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
               tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
               tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec) << "\"";
        oss << ",\"severity\":\"" << severity_to_string(error.severity) << "\"";
        oss << ",\"category\":\"" << category_to_string(error.category) << "\"";
        oss << ",\"code\":\"" << error.code << "\"";
        oss << ",\"message\":\"" << escape_json_string(error.message) << "\"";
        if (error.suggestion) {
            oss << ",\"suggestion\":\"" << escape_json_string(*error.suggestion) << "\"";
        }
        oss << "}";
        return oss.str();
    }


    [[nodiscard]] static std::optional<ClassifiedError> deserialize_error(std::string_view line) {

        auto extract = [&](std::string_view key) -> std::string {
            auto search = "\"" + std::string(key) + "\":\"";
            auto pos = line.find(search);
            if (pos == std::string_view::npos) return {};
            pos += search.size();
            auto end = line.find('"', pos);
            if (end == std::string_view::npos) return {};
            return std::string(line.substr(pos, end - pos));
        };

        ClassifiedError error;
        error.message = extract("message");
        error.code = extract("code");
        auto sev = extract("severity");
        auto cat = extract("category");
        auto sug = extract("suggestion");

        if (sev == "info") error.severity = ErrorSeverity::Info;
        else if (sev == "warning") error.severity = ErrorSeverity::Warning;
        else if (sev == "fatal") error.severity = ErrorSeverity::Fatal;
        else error.severity = ErrorSeverity::Error;

        if (cat == "network") error.category = ErrorCategory::Network;
        else if (cat == "filesystem") error.category = ErrorCategory::FileSystem;
        else if (cat == "permission") error.category = ErrorCategory::Permission;
        else if (cat == "parse") error.category = ErrorCategory::Parse;
        else if (cat == "api") error.category = ErrorCategory::Api;
        else error.category = ErrorCategory::Internal;

        if (!sug.empty()) error.suggestion = sug;

        if (error.message.empty() && error.code.empty()) return std::nullopt;
        return error;
    }


    [[nodiscard]] static std::string escape_json_string(std::string_view sv) {
        std::string result;
        result.reserve(sv.size());
        for (char c : sv) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }
};

} // namespace cc::utils
