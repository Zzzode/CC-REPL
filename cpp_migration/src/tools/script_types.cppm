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

// 脚本语言类型
enum class ScriptLanguage {
    TypeScript,
    JavaScript,
    Python,
    Shell,
    Unknown
};

// 根据文件扩展名检测脚本语言
inline auto detect_script_language(const std::filesystem::path& file) -> ScriptLanguage {
    auto ext = file.extension().string();

    // 转为小写便于比较
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

    // 无扩展名时检查文件名模式
    auto filename = file.filename().string();
    if (filename == "Makefile" || filename == "Dockerfile") {
        return ScriptLanguage::Shell;
    }

    return ScriptLanguage::Unknown;
}

// 获取脚本语言对应的运行时路径
inline auto get_script_runner(ScriptLanguage lang) -> std::optional<std::filesystem::path> {
    switch (lang) {
        case ScriptLanguage::TypeScript:
            // 优先使用 bun（项目默认运行时），其次 ts-node/npx tsx
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

// 脚本执行结果
struct ScriptResult {
    int exit_code;                            // 进程退出码
    std::string output;                       // 标准输出
    std::string errors;                       // 标准错误
    std::vector<Diagnostic> diagnostics;      // 解析后的诊断信息
    std::chrono::milliseconds duration{0};    // 实际执行时长
};

// 将 ScriptLanguage 转为可读字符串
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
