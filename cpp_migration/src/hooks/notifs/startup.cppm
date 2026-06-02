module;
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

export module cc.hooks.notifs.startup;

export namespace cc::hooks::notifs {

inline bool check_first_run();

// 获取启动阶段的所有通知（如更新提示、欢迎信息等）
inline std::vector<std::string> get_startup_notifications() {
    std::vector<std::string> notifications;

    if (check_first_run()) {
        notifications.emplace_back("Welcome to CC-REPL! Type /help to get started.");
    }

    return notifications;
}

// 显示欢迎通知，区分首次运行和后续运行
inline void show_welcome_notification(bool first_run) {
    if (first_run) {
        std::fprintf(stderr, "Welcome to CC-REPL! Type /help to get started.\n");
    } else {
        std::fprintf(stderr, "CC-REPL ready.\n");
    }
}

// 显示版本更新可用通知
inline void show_update_available_notification(std::string_view version) {
    std::fprintf(stderr, "Update available: v%.*s — run '/upgrade' to update.\n",
                 static_cast<int>(version.size()), version.data());
}

// 检查是否为首次运行（通过检测配置文件是否存在）
inline bool check_first_run() {
    // 检查用户主目录下的配置标记文件
    auto config_path = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "")
                       / ".cc-repl" / ".initialized";
    return !std::filesystem::exists(config_path);
}

} // namespace cc::hooks::notifs
