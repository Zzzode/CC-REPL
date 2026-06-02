/// @file voice_keyterms.cppm
/// @brief Voice keyterms for improving STT accuracy in the voice_stream endpoint.
/// Provides domain-specific vocabulary hints (Deepgram "keywords") so the STT
/// engine correctly recognizes coding terminology, project names, and branch
/// names that would otherwise be misheard.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <cctype>

export module cc.services.voice.keyterms;

export namespace cc::services::voice::keyterms {

// =========================================================================
// Global keyterms
// =========================================================================

constexpr std::array<std::string_view, 9> GLOBAL_KEYTERMS = {
    "MCP",
    "symlink",
    "grep",
    "regex",
    "localhost",
    "codebase",
    "TypeScript",
    "JSON",
    "OAuth",
};

// =========================================================================
// Helpers
// =========================================================================

std::vector<std::string> split_identifier(std::string_view name) {
    std::vector<std::string> result;
    std::string current_word;

    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];

        if (std::isupper(static_cast<unsigned char>(c))) {
            if (!current_word.empty()) {
                if (current_word.length() > 2 && current_word.length() <= 20) {
                    result.push_back(current_word);
                }
                current_word.clear();
            }
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '-' || c == '_' || c == '/' || c == '.' || std::isspace(static_cast<unsigned char>(c))) {
            if (!current_word.empty()) {
                if (current_word.length() > 2 && current_word.length() <= 20) {
                    result.push_back(current_word);
                }
                current_word.clear();
            }
        } else {
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    if (!current_word.empty() && current_word.length() > 2 && current_word.length() <= 20) {
        result.push_back(current_word);
    }

    return result;
}

std::vector<std::string> file_name_words(const std::filesystem::path& file_path) {
    auto stem = file_path.stem().string();
    return split_identifier(stem);
}

std::string to_lower(std::string_view sv) {
    std::string result(sv);
    for (auto& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// =========================================================================
// Keyterm Generator
// =========================================================================

constexpr size_t MAX_KEYTERMS = 50;

class KeytermGenerator {
public:
    KeytermGenerator() {
        for (auto term : GLOBAL_KEYTERMS) {
            keyterms_.insert(std::string(term));
        }
    }

    void add_project_name(const std::filesystem::path& project_root) {
        try {
            auto project_name = project_root.filename().string();
            if (project_name.length() > 2 && project_name.length() <= 50) {
                keyterms_.insert(project_name);
            }
        } catch (...) {
            // Ignore errors
        }
    }

    void add_branch_name(std::string_view branch) {
        try {
            auto words = split_identifier(branch);
            for (const auto& word : words) {
                keyterms_.insert(word);
            }
        } catch (...) {
            // Ignore errors
        }
    }

    void add_recent_files(const std::vector<std::filesystem::path>& files) {
        for (const auto& file_path : files) {
            if (keyterms_.size() >= MAX_KEYTERMS) {
                break;
            }
            auto words = file_name_words(file_path);
            for (const auto& word : words) {
                if (keyterms_.size() >= MAX_KEYTERMS) {
                    break;
                }
                keyterms_.insert(word);
            }
        }
    }

    void add_custom_keyterm(std::string_view term) {
        if (keyterms_.size() < MAX_KEYTERMS) {
            keyterms_.insert(std::string(term));
        }
    }

    std::vector<std::string> get_keyterms() const {
        std::vector<std::string> result;
        result.reserve(keyterms_.size());
        for (const auto& term : keyterms_) {
            result.push_back(term);
        }
        if (result.size() > MAX_KEYTERMS) {
            result.resize(MAX_KEYTERMS);
        }
        return result;
    }

    void clear() {
        keyterms_.clear();
        for (auto term : GLOBAL_KEYTERMS) {
            keyterms_.insert(std::string(term));
        }
    }

    size_t size() const {
        return keyterms_.size();
    }

private:
    std::set<std::string> keyterms_;
};

// =========================================================================
// Standalone function for getting voice keyterms
// =========================================================================

std::vector<std::string> get_voice_keyterms(
    const std::optional<std::filesystem::path>& project_root = std::nullopt,
    const std::optional<std::string>& branch_name = std::nullopt,
    const std::optional<std::vector<std::filesystem::path>>& recent_files = std::nullopt) {

    KeytermGenerator generator;

    if (project_root) {
        generator.add_project_name(*project_root);
    }

    if (branch_name) {
        generator.add_branch_name(*branch_name);
    }

    if (recent_files) {
        generator.add_recent_files(*recent_files);
    }

    return generator.get_keyterms();
}

} // namespace cc::services::voice::keyterms
