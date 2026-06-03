module;
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstddef>
#include <format>
#include <iomanip>
#include <sstream>

export module cc.utils.diag_logs;

export namespace cc::utils {


struct DiagEntry {
    std::chrono::system_clock::time_point time;
    std::string category;
    std::string message;
    std::map<std::string, std::string> metadata;
};


class DiagCollector {
public:
    explicit DiagCollector(size_t capacity = 1000)
        : capacity_(capacity) {
        buffer_.reserve(capacity);
    }


    void add_diag(DiagEntry entry) {
        std::lock_guard lock(mutex_);
        if (buffer_.size() < capacity_) {
            buffer_.push_back(std::move(entry));
        } else {

            buffer_[write_pos_] = std::move(entry);
        }
        write_pos_ = (write_pos_ + 1) % capacity_;
        total_count_++;
    }


    void add(std::string_view category, std::string_view message,
             std::map<std::string, std::string> metadata = {}) {
        add_diag(DiagEntry{
            .time = std::chrono::system_clock::now(),
            .category = std::string(category),
            .message = std::string(message),
            .metadata = std::move(metadata)
        });
    }


    [[nodiscard]] std::vector<DiagEntry> get_recent(size_t n) const {
        std::lock_guard lock(mutex_);
        std::vector<DiagEntry> result;
        size_t count = std::min(n, buffer_.size());
        result.reserve(count);

        if (buffer_.size() < capacity_) {

            size_t start = buffer_.size() > count ? buffer_.size() - count : 0;
            for (size_t i = buffer_.size(); i > start; --i) {
                result.push_back(buffer_[i - 1]);
            }
        } else {

            for (size_t i = 0; i < count; ++i) {
                size_t idx = (write_pos_ + capacity_ - 1 - i) % capacity_;
                result.push_back(buffer_[idx]);
            }
        }
        return result;
    }


    [[nodiscard]] size_t total() const {
        std::lock_guard lock(mutex_);
        return total_count_;
    }


    [[nodiscard]] size_t size() const {
        std::lock_guard lock(mutex_);
        return buffer_.size();
    }


    void dump_to_file(const std::filesystem::path& path) const {
        std::lock_guard lock(mutex_);

        if (auto parent = path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) return;


        auto entries = get_ordered_entries();
        for (const auto& entry : entries) {
            auto time_t_val = std::chrono::system_clock::to_time_t(entry.time);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.time.time_since_epoch()) % 1000;

            std::tm tm_buf{};
            gmtime_r(&time_t_val, &tm_buf);

            file << std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                               tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                               tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                               static_cast<int>(ms.count()));
            file << " [" << entry.category << "] " << entry.message;

            if (!entry.metadata.empty()) {
                file << " {";
                bool first = true;
                for (const auto& [key, val] : entry.metadata) {
                    if (!first) file << ", ";
                    file << key << "=" << val;
                    first = false;
                }
                file << "}";
            }
            file << '\n';
        }
    }


    void clear() {
        std::lock_guard lock(mutex_);
        buffer_.clear();
        write_pos_ = 0;
        total_count_ = 0;
    }

private:
    size_t capacity_;
    std::vector<DiagEntry> buffer_;
    size_t write_pos_ = 0;
    size_t total_count_ = 0;
    mutable std::mutex mutex_;


    [[nodiscard]] std::vector<DiagEntry> get_ordered_entries() const {
        std::vector<DiagEntry> entries;
        if (buffer_.size() < capacity_) {
            entries = buffer_;
        } else {
            entries.reserve(capacity_);
            for (size_t i = 0; i < capacity_; ++i) {
                entries.push_back(buffer_[(write_pos_ + i) % capacity_]);
            }
        }
        return entries;
    }
};

} // namespace cc::utils
