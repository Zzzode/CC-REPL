module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <optional>
#include <algorithm>

export module cc.tools.script_types;

import cc.tools.script_diagnostics;

export namespace cc::tools {


enum class ScriptLanguage {
    TypeScript,
    JavaScript,
    Python,
    Shell,
    Unknown
};


inline auto detect_script_language(const std::filesystem::path& file) -> ScriptLanguage {
    auto ext = file.extension().string();


    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts") {
        return ScriptLanguage::TypeScript;
    }
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") {
        return ScriptLanguage::JavaScript;
    }
    if (ext == ".py" || ext == ".pyw") {
        return ScriptLanguage::Python;
    }
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh" || ext == ".fish") {
        return ScriptLanguage::Shell;
    }


    auto filename = file.filename().string();
    if (filename == "Makefile" || filename == "Dockerfile") {
        return ScriptLanguage::Shell;
    }

    return ScriptLanguage::Unknown;
}


inline auto get_script_runner(ScriptLanguage lang) -> std::optional<std::filesystem::path> {
    switch (lang) {
        case ScriptLanguage::TypeScript:

            return std::filesystem::path("/usr/local/bin/bun");

        case ScriptLanguage::JavaScript:
            return std::filesystem::path("/usr/local/bin/node");

        case ScriptLanguage::Python:
            return std::filesystem::path("/usr/bin/python3");

        case ScriptLanguage::Shell:
            return std::filesystem::path("/bin/bash");

        case ScriptLanguage::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}


struct ScriptResult {
    int exit_code;
    std::string output;
    std::string errors;
    std::vector<Diagnostic> diagnostics;
    std::chrono::milliseconds duration{0};
};


inline auto script_language_to_string(ScriptLanguage lang) -> std::string_view {
    switch (lang) {
        case ScriptLanguage::TypeScript:  return "TypeScript";
        case ScriptLanguage::JavaScript:  return "JavaScript";
        case ScriptLanguage::Python:      return "Python";
        case ScriptLanguage::Shell:       return "Shell";
        case ScriptLanguage::Unknown:     return "Unknown";
    }
    return "Unknown";
}

} // namespace cc::tools
