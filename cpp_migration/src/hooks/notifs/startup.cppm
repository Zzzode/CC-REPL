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


inline std::vector<std::string> get_startup_notifications() {
    std::vector<std::string> notifications;

    if (check_first_run()) {
        notifications.emplace_back("Welcome to CC-REPL! Type /help to get started.");
    }

    return notifications;
}


inline void show_welcome_notification(bool first_run) {
    if (first_run) {
        std::fprintf(stderr, "Welcome to CC-REPL! Type /help to get started.\n");
    } else {
        std::fprintf(stderr, "CC-REPL ready.\n");
    }
}


inline void show_update_available_notification(std::string_view version) {
    std::fprintf(stderr, "Update available: v%.*s — run '/upgrade' to update.\n",
                 static_cast<int>(version.size()), version.data());
}


inline bool check_first_run() {

    auto config_path = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "")
                       / ".cc-repl" / ".initialized";
    return !std::filesystem::exists(config_path);
}

} // namespace cc::hooks::notifs
