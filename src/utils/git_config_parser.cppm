module;

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

export module cc.utils.git_config_parser;

export namespace cc::utils {

namespace fs = std::filesystem;

// Parsed git config structure: sections[section_name][key] = value
struct GitConfig {
    std::map<std::string, std::map<std::string, std::string>> sections;
};

// Parse a .gitconfig-style file content into a GitConfig struct
inline GitConfig parse_git_config(std::string_view content) {
    GitConfig config;
    std::istringstream stream{std::string(content)};
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        // Trim leading/trailing whitespace
        auto ltrim = line.find_first_not_of(" \t");
        if (ltrim == std::string::npos) continue;
        line = line.substr(ltrim);
        auto rtrim = line.find_last_not_of(" \t\r\n");
        if (rtrim != std::string::npos) line = line.substr(0, rtrim + 1);

        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        // Section header [section] or [section "subsection"]
        if (line[0] == '[') {
            auto end_bracket = line.find(']');
            if (end_bracket == std::string::npos) continue;

            std::string section_content = line.substr(1, end_bracket - 1);

            // Check for subsection: [section "subsection"]
            auto quote_start = section_content.find('"');
            if (quote_start != std::string::npos) {
                auto quote_end = section_content.find('"', quote_start + 1);
                std::string section_name = section_content.substr(0, quote_start);
                // Trim trailing space from section name
                while (!section_name.empty() && section_name.back() == ' ')
                    section_name.pop_back();
                std::string subsection = (quote_end != std::string::npos)
                    ? section_content.substr(quote_start + 1, quote_end - quote_start - 1)
                    : "";
                current_section = section_name + "." + subsection;
            } else {
                current_section = section_content;
                // Normalize to lowercase
                std::transform(current_section.begin(), current_section.end(),
                             current_section.begin(), ::tolower);
            }
            continue;
        }

        // Key-value pair
        if (!current_section.empty()) {
            auto eq_pos = line.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);

                // Trim key and value
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                    key.pop_back();
                auto val_start = value.find_first_not_of(" \t");
                if (val_start != std::string::npos)
                    value = value.substr(val_start);
                else
                    value.clear();

                // Normalize key to lowercase
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                config.sections[current_section][key] = value;
            } else {
                // Boolean key (no value means true)
                std::string key = line;
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                    key.pop_back();
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                config.sections[current_section][key] = "true";
            }
        }
    }

    return config;
}

// Parse a git config file from disk
inline std::expected<GitConfig, std::string> parse_git_config_file(const fs::path& path) {
    if (!fs::exists(path)) {
        return std::unexpected("Config file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected("Cannot open config file: " + path.string());
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return parse_git_config(content);
}

// Get a value from a parsed config by section and key
inline std::optional<std::string> get_value(const GitConfig& config,
                                            std::string_view section,
                                            std::string_view key) {
    auto sec_it = config.sections.find(std::string(section));
    if (sec_it == config.sections.end()) return std::nullopt;

    auto key_it = sec_it->second.find(std::string(key));
    if (key_it == sec_it->second.end()) return std::nullopt;

    return key_it->second;
}

} // namespace cc::utils
