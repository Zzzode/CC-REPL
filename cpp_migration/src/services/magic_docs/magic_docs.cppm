/// @file magic_docs.cppm
/// @brief Magic documentation service.
/// Auto-generates documentation prompts from project structure, detects project
/// type (language, framework), analyzes README/CLAUDE.md, and generates
/// relevant context for LLM interactions.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <filesystem>

export module cc.services.magic_docs;

import cc.types.types;

export namespace cc::services::magic_docs {

using cc::core::Error;
using cc::core::ErrorCode;
namespace fs = std::filesystem;

// ============================================================
// 项目类型检测
// ============================================================

// 支持的编程语言
enum class Language : std::uint8_t {
    Unknown,
    Cpp,
    Rust,
    Go,
    Python,
    TypeScript,
    JavaScript,
    Java,
    Swift,
};

// 支持的框架
enum class Framework : std::uint8_t {
    Unknown,
    CMake,
    Bazel,
    Cargo,
    NPM,
    Poetry,
    Gradle,
    SwiftPM,
};

// 项目类型检测结果
struct ProjectType {
    Language primary_language{Language::Unknown};
    Framework build_system{Framework::Unknown};
    std::vector<Language> secondary_languages;
    std::vector<std::string> detected_markers;  // 触发检测的文件
};

// 文档文件信息
struct DocFile {
    std::string path;
    std::string content;
    std::size_t token_estimate{0};  // 粗略 token 估算
};

// 上下文生成结果
struct GeneratedContext {
    std::string system_prompt;          // 为 LLM 生成的系统提示
    std::vector<DocFile> relevant_docs; // 相关文档
    ProjectType project_type;           // 检测到的项目类型
    std::size_t total_tokens{0};        // 总 token 估算
};

// 配置
struct MagicDocsConfig {
    std::size_t max_context_tokens{8192};
    bool include_readme{true};
    bool include_claude_md{true};
    bool scan_subdirectories{true};
    std::size_t max_doc_depth{3};
};

// ============================================================
// MagicDocsService - 自动文档与上下文生成
// ============================================================

class MagicDocsService {
public:
    explicit MagicDocsService(MagicDocsConfig config = {})
        : config_(std::move(config)) {}

    // 分析项目并生成 LLM 上下文
    [[nodiscard]] std::expected<GeneratedContext, Error> generate_context(
        const fs::path& project_root) const
    {
        GeneratedContext ctx;
        // 检测项目类型
        ctx.project_type = detect_project_type(project_root);
        // 收集文档文件
        ctx.relevant_docs = collect_docs(project_root);
        // 生成系统提示
        ctx.system_prompt = build_system_prompt(ctx.project_type, ctx.relevant_docs);
        // 计算总 token
        ctx.total_tokens = estimate_tokens(ctx.system_prompt);
        for (const auto& doc : ctx.relevant_docs) {
            ctx.total_tokens += doc.token_estimate;
        }
        return ctx;
    }

    // 仅检测项目类型
    [[nodiscard]] ProjectType detect_project_type(const fs::path& root) const {
        ProjectType result;
        // 检测标记文件
        static constexpr struct { std::string_view file; Language lang; Framework fw; } markers[] = {
            {"CMakeLists.txt", Language::Cpp, Framework::CMake},
            {"Cargo.toml", Language::Rust, Framework::Cargo},
            {"go.mod", Language::Go, Framework::Unknown},
            {"package.json", Language::TypeScript, Framework::NPM},
            {"pyproject.toml", Language::Python, Framework::Poetry},
            {"build.gradle", Language::Java, Framework::Gradle},
            {"Package.swift", Language::Swift, Framework::SwiftPM},
            {"BUILD", Language::Cpp, Framework::Bazel},
        };
        for (const auto& [file, lang, fw] : markers) {
            auto p = root / file;
            if (fs::exists(p)) {
                if (result.primary_language == Language::Unknown) {
                    result.primary_language = lang;
                    result.build_system = fw;
                } else {
                    result.secondary_languages.push_back(lang);
                }
                result.detected_markers.emplace_back(file);
            }
        }
        return result;
    }

    // 更新配置
    void set_config(MagicDocsConfig config) noexcept { config_ = std::move(config); }
    [[nodiscard]] const MagicDocsConfig& config() const noexcept { return config_; }

private:
    MagicDocsConfig config_;

    // 收集项目文档文件
    [[nodiscard]] std::vector<DocFile> collect_docs(const fs::path& root) const {
        std::vector<DocFile> docs;
        static constexpr std::string_view doc_names[] = {
            "README.md", "CLAUDE.md", "CONTEXT.md", "CONTRIBUTING.md",
        };
        for (auto name : doc_names) {
            auto path = root / name;
            if (fs::exists(path)) {
                docs.push_back({
                    .path = path.string(),
                    .content = "",  // 实际实现中读取文件
                    .token_estimate = estimate_file_tokens(path),
                });
            }
        }
        return docs;
    }

    // 构建系统提示
    [[nodiscard]] std::string build_system_prompt(
        const ProjectType& pt,
        const std::vector<DocFile>& docs) const
    {
        auto lang_name = language_name(pt.primary_language);
        auto fw_name = framework_name(pt.build_system);
        return std::format(
            "This is a {} project using {}. {} documentation files detected.",
            lang_name, fw_name, docs.size());
    }

    // Token 估算 (粗略: 1 token ≈ 4 字符)
    [[nodiscard]] std::size_t estimate_tokens(std::string_view text) const noexcept {
        return text.size() / 4;
    }

    [[nodiscard]] std::size_t estimate_file_tokens(const fs::path& path) const noexcept {
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        return ec ? 0 : static_cast<std::size_t>(size) / 4;
    }

    static constexpr std::string_view language_name(Language lang) noexcept {
        switch (lang) {
            case Language::Cpp:        return "C++";
            case Language::Rust:       return "Rust";
            case Language::Go:         return "Go";
            case Language::Python:     return "Python";
            case Language::TypeScript: return "TypeScript";
            case Language::JavaScript: return "JavaScript";
            case Language::Java:       return "Java";
            case Language::Swift:      return "Swift";
            default:                   return "Unknown";
        }
    }

    static constexpr std::string_view framework_name(Framework fw) noexcept {
        switch (fw) {
            case Framework::CMake:   return "CMake";
            case Framework::Bazel:   return "Bazel";
            case Framework::Cargo:   return "Cargo";
            case Framework::NPM:     return "NPM";
            case Framework::Poetry:  return "Poetry";
            case Framework::Gradle:  return "Gradle";
            case Framework::SwiftPM: return "SwiftPM";
            default:                 return "unknown build system";
        }
    }
};

} // namespace cc::services::magic_docs
