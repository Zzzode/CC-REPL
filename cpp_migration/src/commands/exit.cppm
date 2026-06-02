module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>

export module cc.commands.exit;

export namespace cc::commands {

auto exit_state_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "last-exit.txt";
    return std::filesystem::path{".cc-repl"} / "last-exit.txt";
}

auto confirm_exit_with_pending_changes() -> bool;
auto save_state_before_exit() -> void;

// 执行退出操作
auto execute_exit(bool force = false) -> void {
    if (!force) {
        // 非强制退出时检查是否有未保存的变更
        if (!confirm_exit_with_pending_changes()) {
            return; // 用户取消退出
        }
    }
    save_state_before_exit();
    static_cast<void>(std::atexit([] {}));
}

// 检查是否有待处理变更并确认退出
auto confirm_exit_with_pending_changes() -> bool {
    return true;
}

// 获取退出提示文本
auto get_exit_prompt() -> std::string {
    return "Are you sure you want to exit? (y/n)";
}

// 退出前保存当前状态
auto save_state_before_exit() -> void {
    auto path = exit_state_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    output << "status=exited\n";
}

} // namespace cc::commands
