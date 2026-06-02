module;
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

export module cc.utils.session_start;

export namespace cc::utils {

namespace fs = std::filesystem;

struct SessionStartConfig {
    std::optional<std::string> resume_id;
    std::optional<fs::path> cwd;
    bool headless = false;
    std::string model = "claude-sonnet-4-20250514";
};

namespace detail {
    // Generate a unique session ID (UUID-like)
    inline std::string generate_session_id() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dist(0, 15);

        const char* hex = "0123456789abcdef";
        std::string uuid;
        uuid.reserve(36);

        // Format: 8-4-4-4-12
        int segments[] = {8, 4, 4, 4, 12};
        bool first = true;
        for (int seg : segments) {
            if (!first) uuid += '-';
            first = false;
            for (int i = 0; i < seg; ++i) {
                uuid += hex[dist(gen)];
            }
        }
        return uuid;
    }

    inline fs::path get_sessions_base_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }
} // namespace detail

// Create the session directory structure
fs::path create_session_directory(std::string_view session_id) {
    auto dir = detail::get_sessions_base_dir() / std::string(session_id);
    fs::create_directories(dir);
    return dir;
}

// Start a new session (or resume an existing one)
std::expected<std::string, std::string> start_session(const SessionStartConfig& config) {
    // If resuming, validate the session exists
    if (config.resume_id) {
        auto dir = detail::get_sessions_base_dir() / *config.resume_id;
        if (!fs::exists(dir)) {
            return std::unexpected("Session not found: " + *config.resume_id);
        }
        return *config.resume_id;
    }

    // Create new session
    std::string session_id = detail::generate_session_id();
    auto session_dir = create_session_directory(session_id);

    // Create metadata file
    auto meta_path = session_dir / "metadata.json";
    std::ofstream meta(meta_path);
    if (meta.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);

        meta << "{\n";
        meta << "  \"id\": \"" << session_id << "\",\n";
        meta << "  \"model\": \"" << config.model << "\",\n";
        meta << "  \"headless\": " << (config.headless ? "true" : "false") << ",\n";
        meta << "  \"created_at\": " << t << "\n";
        meta << "}\n";
    }

    return session_id;
}

} // namespace cc::utils
