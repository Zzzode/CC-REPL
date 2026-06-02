// Thinkback Play command - plays back recorded thinking animations
module;
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
export module cc.commands.thinkback_play;
export namespace cc::commands::thinkback_play {

namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "thinkback-play"; }

[[nodiscard]] inline auto run(std::string_view recording = {}) -> CommandResponse {
    // Look for thinkback skill/plugin
    std::string skill_dir;
    
    // Check common locations
    if (auto* home = std::getenv("HOME"); home && home[0] != '\0') {
        fs::path skills_path = fs::path(home) / ".claude" / "skills" / "thinkback";
        if (fs::exists(skills_path)) {
            skill_dir = skills_path.string();
        }
    }
    
    if (skill_dir.empty()) {
        return {
            .ok = false,
            .message = "Thinkback plugin not found.\n\n"
                       "Install it with: `npx skills install thinkback`\n"
                       "Or check ~/.claude/skills/thinkback/"
        };
    }
    
    if (recording.empty()) {
        return {
            .ok = false,
            .message = "Usage: /thinkback-play <recording-id>\n\n"
                       "List recordings with: /thinkback list"
        };
    }
    
    // In full implementation: read the recording file and replay the animation
    return {
        .ok = true,
        .message = "Playing thinkback recording: " + std::string(recording) + "\n"
                   "(Animation playback from " + skill_dir + ")"
    };
}

}
