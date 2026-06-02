/// @file team_memory.cppm
/// @brief Team shared memory management and prompts.
/// Migrated from src/memdir/teamMemPaths.ts, teamMemPrompts.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>

export module cc.memdir.team_memory;

import cc.memdir.paths;

export namespace cc::memdir {

/// Team memory file entry
struct TeamMemoryFile {
    std::filesystem::path path;
    std::string filename;
    std::string content;
};

/// Get all team memory files in the team-memory directory
[[nodiscard]] inline std::vector<TeamMemoryFile> get_team_memory_files(
    const std::filesystem::path& project_root
) {
    std::vector<TeamMemoryFile> files;
    auto dir = get_team_memory_dir(project_root);
    
    if (!std::filesystem::exists(dir)) return files;
    
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;
        
        std::ifstream file(entry.path());
        if (!file.is_open()) continue;
        
        std::ostringstream ss;
        ss << file.rdbuf();
        
        files.push_back(TeamMemoryFile{
            .path = entry.path(),
            .filename = entry.path().filename().string(),
            .content = ss.str(),
        });
    }
    
    return files;
}

/// Generate team memory prompt section for system prompt
[[nodiscard]] inline std::string generate_team_memory_prompt(
    const std::vector<TeamMemoryFile>& files
) {
    if (files.empty()) return "";
    
    std::string prompt = "\n<team-memory>\n";
    for (const auto& file : files) {
        prompt += "## " + file.filename + "\n";
        prompt += file.content + "\n\n";
    }
    prompt += "</team-memory>\n";
    
    return prompt;
}

} // namespace cc::memdir
