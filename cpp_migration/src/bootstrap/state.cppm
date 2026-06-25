/// @file state.cppm
/// @brief Bootstrap state initialization.
/// Migrated from src/bootstrap/state.ts
module;

#include <string>
#include <filesystem>
#include <optional>
#include <chrono>

export module cc.bootstrap.state;

export namespace cc::bootstrap {

/// Bootstrap configuration gathered during startup
struct BootstrapConfig {
    std::filesystem::path project_root;
    std::filesystem::path home_dir;
    std::filesystem::path config_dir;        // ~/.claude/
    std::filesystem::path sessions_dir;      // ~/.claude/sessions/
    std::optional<std::string> initial_model;
    bool is_git_repo = false;
    std::optional<std::string> git_remote_url;
    std::string session_id;
    std::chrono::system_clock::time_point startup_time;
};

/// Initialize bootstrap configuration by scanning the environment
[[nodiscard]] inline BootstrapConfig initialize_bootstrap(
    const std::filesystem::path& cwd,
    std::optional<std::string> override_model = std::nullopt
) {
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
    auto config_dir = home / ".claude";
    
    // Find project root by walking up to find .git
    auto project_root = cwd;
    auto current = cwd;
    while (current.has_parent_path() && current != current.parent_path()) {
        if (std::filesystem::exists(current / ".git")) {
            project_root = current;
            break;
        }
        current = current.parent_path();
    }
    
    // Create config directories if needed
    std::filesystem::create_directories(config_dir);
    auto sessions_dir = config_dir / "sessions";
    std::filesystem::create_directories(sessions_dir);
    
    // Generate session ID
    auto now = std::chrono::system_clock::now();
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    auto session_id = "session_" + std::to_string(epoch_ms);
    
    return BootstrapConfig{
        .project_root = project_root,
        .home_dir = home,
        .config_dir = config_dir,
        .sessions_dir = sessions_dir,
        .initial_model = override_model,
        .is_git_repo = std::filesystem::exists(project_root / ".git"),
        .git_remote_url = std::nullopt,
        .session_id = session_id,
        .startup_time = now,
    };
}

} // namespace cc::bootstrap
