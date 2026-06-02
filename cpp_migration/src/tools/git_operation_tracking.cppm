module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <array>
#include <cstdio>

export module cc.tools.git_operation_tracking;

export namespace cc::tools {

// Git 操作记录
struct GitOperation {
    std::string type;                              // 操作类型（commit, push, pull, checkout 等）
    std::filesystem::path repo;                    // 仓库路径
    std::vector<std::filesystem::path> affected_files; // 受影响的文件
    std::chrono::system_clock::time_point timestamp;   // 操作时间戳
};

namespace detail {
    // 全局操作历史记录（线程安全）
    inline std::mutex g_ops_mutex;
    inline std::vector<GitOperation> g_operations;

    [[nodiscard]] inline auto command_has_output(const char* command) -> bool {
        std::array<char, 128> buffer{};
        FILE* pipe = popen(command, "r");
        if (!pipe) return false;
        const bool has_output = fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr;
        pclose(pipe);
        return has_output;
    }
}

// 记录一次 git 操作
inline auto record_git_operation(GitOperation op) -> void {
    std::lock_guard lock(detail::g_ops_mutex);

    // 自动填充时间戳（如果未设置）
    if (op.timestamp == std::chrono::system_clock::time_point{}) {
        op.timestamp = std::chrono::system_clock::now();
    }

    detail::g_operations.push_back(std::move(op));

    // 限制历史记录大小，保留最近 1000 条
    if (detail::g_operations.size() > 1000) {
        detail::g_operations.erase(
            detail::g_operations.begin(),
            detail::g_operations.begin() + 500
        );
    }
}

// 获取最近 N 条 git 操作记录
inline auto get_recent_git_operations(size_t n) -> std::vector<GitOperation> {
    std::lock_guard lock(detail::g_ops_mutex);

    if (detail::g_operations.empty()) {
        return {};
    }

    size_t count = std::min(n, detail::g_operations.size());
    auto begin = detail::g_operations.end() - static_cast<ptrdiff_t>(count);

    return std::vector<GitOperation>(begin, detail::g_operations.end());
}

// 检查是否有未提交的变更（需要调用 git 命令）
inline auto has_uncommitted_changes() -> bool {
    return detail::command_has_output("git status --porcelain 2>/dev/null");
}

// 获取指定时间点之后变更的文件列表
inline auto get_changed_files_since(
    std::chrono::system_clock::time_point since
) -> std::vector<std::filesystem::path> {
    std::lock_guard lock(detail::g_ops_mutex);

    std::vector<std::filesystem::path> changed;

    for (const auto& op : detail::g_operations) {
        if (op.timestamp >= since) {
            for (const auto& file : op.affected_files) {
                // 去重：避免同一个文件出现多次
                if (std::find(changed.begin(), changed.end(), file) == changed.end()) {
                    changed.push_back(file);
                }
            }
        }
    }

    return changed;
}

} // namespace cc::tools
