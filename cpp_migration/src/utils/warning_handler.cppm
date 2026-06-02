module;
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstddef>
#include <atomic>

export module cc.utils.warning_handler;

export namespace cc::utils {

// 警告去重与抑制处理器（单例模式）
class WarningHandler {
public:
    // 获取全局单例
    static WarningHandler& instance() {
        static WarningHandler handler;
        return handler;
    }

    // 发出去重警告：相同 ID 仅输出一次
    // 返回 true 表示首次触发，false 表示已被去重
    bool warn(std::string_view id, std::string_view message) {
        std::lock_guard lock(mutex_);
        std::string id_str(id);

        // 检查是否被抑制
        if (suppressed_ids_.contains(id_str)) return false;

        // 去重：相同 ID 只记录一次
        auto [it, inserted] = seen_warnings_.try_emplace(id_str, std::string(message));
        if (!inserted) return false; // 已经见过

        warning_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // 抑制指定 ID 的警告（不再输出）
    void suppress(std::string_view id) {
        std::lock_guard lock(mutex_);
        suppressed_ids_.emplace(id);
    }

    // 取消抑制
    void unsuppress(std::string_view id) {
        std::lock_guard lock(mutex_);
        suppressed_ids_.erase(std::string(id));
    }

    // 检查某个警告 ID 是否已被触发
    [[nodiscard]] bool has_warned(std::string_view id) const {
        std::lock_guard lock(mutex_);
        return seen_warnings_.contains(std::string(id));
    }

    // 检查某个警告 ID 是否被抑制
    [[nodiscard]] bool is_suppressed(std::string_view id) const {
        std::lock_guard lock(mutex_);
        return suppressed_ids_.contains(std::string(id));
    }

    // 获取已触发的警告总数
    [[nodiscard]] size_t get_warning_count() const {
        return warning_count_.load(std::memory_order_relaxed);
    }

    // 获取特定警告的消息（如果存在）
    [[nodiscard]] std::string get_warning_message(std::string_view id) const {
        std::lock_guard lock(mutex_);
        auto it = seen_warnings_.find(std::string(id));
        if (it != seen_warnings_.end()) return it->second;
        return {};
    }

    // 重置所有状态
    void reset() {
        std::lock_guard lock(mutex_);
        seen_warnings_.clear();
        suppressed_ids_.clear();
        warning_count_.store(0, std::memory_order_relaxed);
    }

private:
    WarningHandler() = default;
    ~WarningHandler() = default;

    // 禁止复制和移动
    WarningHandler(const WarningHandler&) = delete;
    WarningHandler& operator=(const WarningHandler&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> seen_warnings_; // id → message
    std::unordered_set<std::string> suppressed_ids_;             // 被抑制的 ID 集合
    std::atomic<size_t> warning_count_{0};
};

} // namespace cc::utils
