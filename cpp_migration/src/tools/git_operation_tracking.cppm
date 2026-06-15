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
import cc.utils.bash_execution;

export namespace cc::tools {


struct GitOperation {
    std::string type;
    std::filesystem::path repo;
    std::vector<std::filesystem::path> affected_files;
    std::chrono::system_clock::time_point timestamp;
};

namespace detail {

    inline std::mutex g_ops_mutex;
    inline std::vector<GitOperation> g_operations;

    [[nodiscard]] inline auto command_has_output(const char* command) -> bool {
        std::array<char, 128> buffer{};
        FILE* pipe = cc::utils::bash::popen_spawn(command);
        if (!pipe) return false;
        const bool has_output = fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr;
        cc::utils::bash::pclose_spawn(pipe);
        return has_output;
    }
}


inline auto record_git_operation(GitOperation op) -> void {
    std::lock_guard lock(detail::g_ops_mutex);


    if (op.timestamp == std::chrono::system_clock::time_point{}) {
        op.timestamp = std::chrono::system_clock::now();
    }

    detail::g_operations.push_back(std::move(op));


    if (detail::g_operations.size() > 1000) {
        detail::g_operations.erase(
            detail::g_operations.begin(),
            detail::g_operations.begin() + 500
        );
    }
}


inline auto get_recent_git_operations(size_t n) -> std::vector<GitOperation> {
    std::lock_guard lock(detail::g_ops_mutex);

    if (detail::g_operations.empty()) {
        return {};
    }

    size_t count = std::min(n, detail::g_operations.size());
    auto begin = detail::g_operations.end() - static_cast<ptrdiff_t>(count);

    return std::vector<GitOperation>(begin, detail::g_operations.end());
}


inline auto has_uncommitted_changes() -> bool {
    return detail::command_has_output("git status --porcelain 2>/dev/null");
}


inline auto get_changed_files_since(
    std::chrono::system_clock::time_point since
) -> std::vector<std::filesystem::path> {
    std::lock_guard lock(detail::g_ops_mutex);

    std::vector<std::filesystem::path> changed;

    for (const auto& op : detail::g_operations) {
        if (op.timestamp >= since) {
            for (const auto& file : op.affected_files) {

                if (std::find(changed.begin(), changed.end(), file) == changed.end()) {
                    changed.push_back(file);
                }
            }
        }
    }

    return changed;
}

} // namespace cc::tools
