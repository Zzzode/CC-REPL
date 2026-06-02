module;
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

export module cc.screens.resume_screen;

export namespace cc::screens {

inline auto repeat_resume_text(std::string_view text, int count) -> std::string {
    std::string result;
    for (int i = 0; i < count; ++i) {
        result += text;
    }
    return result;
}

// Preview data for a resumable session
struct SessionPreview {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point last_active;
    int message_count = 0;
    std::string last_message_preview;
};

// Get a list of sessions that can be resumed
inline auto get_resumable_sessions(int max = 10) -> std::vector<SessionPreview> {
    std::vector<SessionPreview> sessions;
    // Scan session storage directory for saved sessions
    const char* home = std::getenv("HOME");
    if (!home) return sessions;
    auto sessions_dir = std::filesystem::path(home) / ".cc-repl" / "sessions";
    if (!std::filesystem::exists(sessions_dir)) return sessions;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(sessions_dir)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().stem().string();
            auto last_write = std::filesystem::last_write_time(entry.path());
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            sessions.push_back(SessionPreview{
                .id = name,
                .name = name,
                .last_active = sctp,
                .message_count = 0,
                .last_message_preview = {},
            });
            if (static_cast<int>(sessions.size()) >= max) break;
        }
    } catch (...) {}
    // Sort by most recent first
    std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
        return a.last_active > b.last_active;
    });
    return sessions;
}

// Render the session resume screen
inline auto render_resume_screen(std::vector<SessionPreview> sessions,
                                  int selected,
                                  int width,
                                  int height) -> std::string {
    std::ostringstream out;
    int inner_width = std::max(40, width - 4);

    // Title
    out << "╭" << repeat_resume_text("─", inner_width) << "╮\n";
    out << "│ \033[1mResume Session\033[0m"
        << std::string(std::max(0, inner_width - 15), ' ') << "│\n";
    out << "├" << repeat_resume_text("─", inner_width) << "┤\n";

    if (sessions.empty()) {
        out << "│" << std::string(inner_width, ' ') << "│\n";
        out << "│ \033[2mNo sessions to resume\033[0m"
            << std::string(std::max(0, inner_width - 22), ' ') << "│\n";
        out << "│" << std::string(inner_width, ' ') << "│\n";
    } else {
        int max_visible = std::max(3, height - 6);
        int start = 0;
        if (selected >= max_visible) {
            start = selected - max_visible + 1;
        }
        int end = std::min(static_cast<int>(sessions.size()), start + max_visible);

        for (int i = start; i < end; ++i) {
            const auto& session = sessions[i];
            bool is_selected = (i == selected);

            out << "│ ";
            if (is_selected) {
                out << "\033[36m❯\033[0m ";
            } else {
                out << "  ";
            }

            // Session name (bold if selected)
            if (is_selected) {
                out << "\033[1m" << session.name << "\033[0m";
            } else {
                out << session.name;
            }

            // Message count and time
            auto time_t_val = std::chrono::system_clock::to_time_t(session.last_active);
            std::tm tm_buf{};
            localtime_r(&time_t_val, &tm_buf);

            std::ostringstream meta;
            meta << session.message_count << " msgs • "
                 << std::put_time(&tm_buf, "%b %d %H:%M");
            std::string meta_str = meta.str();

            int name_len = static_cast<int>(session.name.size()) + (is_selected ? 4 : 4);
            int gap = inner_width - name_len - static_cast<int>(meta_str.size()) - 1;
            if (gap > 0) out << std::string(gap, ' ');
            out << "\033[2m" << meta_str << "\033[0m";
            out << "│\n";

            // Preview (only for selected)
            if (is_selected && !session.last_message_preview.empty()) {
                std::string preview = session.last_message_preview;
                int max_preview = inner_width - 6;
                if (static_cast<int>(preview.size()) > max_preview) {
                    preview = preview.substr(0, max_preview - 1) + "…";
                }
                out << "│     \033[2m" << preview << "\033[0m";
                int preview_pad = inner_width - 5 - static_cast<int>(preview.size());
                if (preview_pad > 0) out << std::string(preview_pad, ' ');
                out << "│\n";
            }
        }

        // Scroll indicators
        if (start > 0) {
            out << "│ \033[2m  ↑ " << start << " more\033[0m"
                << std::string(std::max(0, inner_width - 10), ' ') << "│\n";
        }
        int remaining = static_cast<int>(sessions.size()) - end;
        if (remaining > 0) {
            out << "│ \033[2m  ↓ " << remaining << " more\033[0m"
                << std::string(std::max(0, inner_width - 10), ' ') << "│\n";
        }
    }

    // Footer
    out << "├" << repeat_resume_text("─", inner_width) << "┤\n";
    std::string hint = "[Enter] Resume  [d] Delete  [Esc] Cancel";
    out << "│ \033[2m" << hint << "\033[0m";
    int hint_pad = inner_width - 1 - static_cast<int>(hint.size());
    if (hint_pad > 0) out << std::string(hint_pad, ' ');
    out << "│\n";
    out << "╰" << repeat_resume_text("─", inner_width) << "╯";

    return out.str();
}

} // namespace cc::screens
